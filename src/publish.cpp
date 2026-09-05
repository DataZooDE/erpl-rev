#include "publish.hpp"

#include <stdexcept>
#include <vector>

#include "cycle.hpp"

namespace erpl_rev {

namespace {

void Exec(duckdb::Connection &con, const std::string &sql, const char *what) {
    auto r = con.Query(sql);
    if (r->HasError())
        throw std::runtime_error(std::string("publish: ") + what + " failed: " + r->GetError());
}

std::string Scalar(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    if (r->HasError()) throw std::runtime_error("publish: query failed: " + r->GetError());
    if (r->RowCount() == 0 || r->GetValue(0, 0).IsNull()) return "";
    return r->GetValue(0, 0).ToString();
}

std::string Lit(const std::string &v) {
    std::string out = "'";
    for (char c : v) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}

}  // namespace

std::string PublishSql(const Sink &sink, const std::string &source_relation) {
    if (sink.kind == SinkKind::Parquet) {
        std::string opt = "FORMAT parquet";
        if (!sink.partition_by.empty())
            // OVERWRITE_OR_IGNORE, or a re-publish onto an existing dataset
            // directory fails rather than replacing it.
            opt += ", PARTITION_BY (" + sink.partition_by + "), OVERWRITE_OR_IGNORE 1";
        return "COPY (SELECT * FROM " + source_relation + ") TO '" + sink.dest + "' (" + opt + ");";
    }
    if (sink.kind == SinkKind::Table) {
        if (sink.mode == SinkMode::Append)
            return "INSERT INTO " + sink.dest + " SELECT * FROM " + source_relation + ";";
        return "DROP TABLE IF EXISTS " + sink.dest + "; CREATE TABLE " + sink.dest +
               " AS SELECT * FROM " + source_relation + ";";
    }
    throw std::runtime_error("publish: unsupported sink kind (expected PARQUET or TABLE)");
}

Sink ParseSink(const std::string &spec) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : spec) {
        if (c == ':') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    if (parts.size() < 2)
        throw std::runtime_error("publish: sink spec must be KIND:DEST[:MODE], got '" + spec + "'");

    Sink s;
    if (parts[0] == "PARQUET") s.kind = SinkKind::Parquet;
    else if (parts[0] == "TABLE") s.kind = SinkKind::Table;
    else throw std::runtime_error("publish: unknown sink kind '" + parts[0] + "'");
    s.dest = parts[1];
    if (parts.size() > 2 && parts[2] == "APPEND") s.mode = SinkMode::Append;
    return s;
}

void CreateSubscription(duckdb::Connection &con, const std::string &name,
                        const std::string &target, const std::string &sink_spec) {
    ParseSink(sink_spec);   // reject a bad spec now, not on the first advance
    Exec(con,
         "CREATE TABLE IF NOT EXISTS _erpl_rev_subscription ("
         "name VARCHAR PRIMARY KEY, target VARCHAR NOT NULL, sink_spec VARCHAR NOT NULL, "
         "\"offset\" BIGINT DEFAULT 0, dedup_keys VARCHAR, last_publish_ts TIMESTAMPTZ, "
         "status VARCHAR DEFAULT 'ACTIVE')",
         "create subscription table");
    Exec(con,
         "INSERT OR REPLACE INTO _erpl_rev_subscription (name, target, sink_spec, \"offset\") "
         "VALUES (" + Lit(name) + "," + Lit(target) + "," + Lit(sink_spec) + ",0)",
         "create subscription");
}

AdvanceResult Advance(duckdb::Connection &con, const std::string &name) {
    auto r = con.Query("SELECT target, sink_spec, \"offset\", coalesce(dedup_keys,'') "
                       "FROM _erpl_rev_subscription WHERE name=" + Lit(name));
    if (r->HasError()) throw std::runtime_error("publish: subscription read failed: " + r->GetError());
    if (r->RowCount() == 0) throw std::runtime_error("publish: no subscription named " + name);

    const std::string target = r->GetValue(0, 0).ToString();
    const std::string spec = r->GetValue(1, 0).ToString();
    const long long offset = r->GetValue(2, 0).GetValue<int64_t>();
    const std::string dedup = r->GetValue(3, 0).ToString();

    const std::string log = cycle::ChangeLogName(target);
    if (Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name=" + Lit(log)) != "1")
        return {};   // nothing has ever been logged for this target

    // The batch is bounded by a high-water read ONCE, so rows appended while this
    // publish runs are not silently skipped by the offset advance.
    const auto hi_s = Scalar(con, "SELECT coalesce(max(_seq),0) FROM " + log);
    const long long hi = hi_s.empty() ? 0 : std::stoll(hi_s);
    if (hi <= offset) return {offset == 0 ? 0 : 0, offset};

    std::string src = "(SELECT * FROM " + log + " WHERE _seq > " + std::to_string(offset) +
                      " AND _seq <= " + std::to_string(hi);
    if (!dedup.empty())
        // Last write wins per key within the batch.
        src += " QUALIFY row_number() OVER (PARTITION BY " + dedup + " ORDER BY _seq DESC)=1";
    src += ")";

    // Only the target's own columns travel; the control columns are the log's
    // bookkeeping, not part of the replicated row.
    std::string cols;
    {
        auto tc = con.Query("SELECT * FROM " + target + " LIMIT 0");
        if (tc->HasError()) throw std::runtime_error("publish: target describe failed");
        for (duckdb::idx_t c = 0; c < tc->ColumnCount(); ++c) {
            if (!cols.empty()) cols += ",";
            cols += tc->names[c];
        }
    }
    const std::string projected = "(SELECT " + cols + " FROM " + src + ")";

    AdvanceResult out;
    out.published = std::stoll(Scalar(con, "SELECT count(*) FROM " + projected));

    // One transaction: publish and advance together. A failed publish that still
    // moved the offset would claim rows that never landed, and no later advance
    // would revisit them.
    Exec(con, "BEGIN", "begin");
    try {
        Exec(con, PublishSql(ParseSink(spec), projected), "publish");
        Exec(con, "UPDATE _erpl_rev_subscription SET \"offset\"=" + std::to_string(hi) +
                      ", last_publish_ts=now() WHERE name=" + Lit(name),
             "advance offset");
        Exec(con, "COMMIT", "commit");
    } catch (...) {
        con.Query("ROLLBACK");
        throw;
    }
    out.new_offset = hi;
    return out;
}

long long Retain(duckdb::Connection &con, const std::string &target, long long window_secs) {
    const std::string log = cycle::ChangeLogName(target);
    if (Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name=" + Lit(log)) != "1")
        return 0;

    // The low-water mark is the SLOWEST subscription, not the fastest: pruning to
    // the fastest would delete rows a slower one has not read, which is silent
    // data loss for that subscription. A target with no subscriptions has no
    // reader to wait for, so only the window protects it.
    long long low = 0;
    bool have_subs = false;
    if (Scalar(con, "SELECT count(*) FROM duckdb_tables() "
                    "WHERE table_name='_erpl_rev_subscription'") == "1") {
        const auto n = Scalar(con, "SELECT count(*) FROM _erpl_rev_subscription "
                                   "WHERE target=" + Lit(target));
        have_subs = n != "0" && !n.empty();
        if (have_subs)
            low = std::stoll(Scalar(con, "SELECT coalesce(min(\"offset\"),0) "
                                         "FROM _erpl_rev_subscription WHERE target=" + Lit(target)));
    }

    std::string where = have_subs ? "_seq <= " + std::to_string(low) : "true";
    if (window_secs > 0)
        where += " AND _changed_at < now() - INTERVAL '" + std::to_string(window_secs) + "' SECOND";

    const auto before = std::stoll(Scalar(con, "SELECT count(*) FROM " + log));
    Exec(con, "DELETE FROM " + log + " WHERE " + where, "retain");
    const auto after = std::stoll(Scalar(con, "SELECT count(*) FROM " + log));
    return before - after;
}

}  // namespace erpl_rev
