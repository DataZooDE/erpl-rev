#include "cdc_dialect.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace erpl_rev {

namespace {

std::string Upper(const std::string &s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return r;
}

// A safe, in-namespace object-name token from a source name: uppercase, and any
// char that isn't A-Z/0-9 becomes '_' (so /BIC/FOO -> _BIC_FOO).
std::string NameToken(const std::string &s) {
    std::string r = Upper(s);
    for (char &c : r)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    return r;
}

std::string Quote(const std::string &id) { return "\"" + id + "\""; }

// Build the INSERT-into-log that a trigger body runs, capturing the logged columns
// (keys, or the full row image for FULL_IUD) plus the op flag, the next sequence
// value and the commit timestamp, from the OLD/NEW row image `alias`.
std::string LogInsert(const CdcPlan &p, const std::string &alias, const char op) {
    std::string cols, vals;
    for (const auto &k : p.log_cols) {
        cols += Quote(k) + ",";
        vals += ":" + alias + "." + Quote(k) + ",";
    }
    cols += Quote(p.op_col) + "," + Quote(p.seq_col) + ",\"_TS\"";
    vals += std::string("'") + op + "'," + Quote(p.seq_name) + ".NEXTVAL,CURRENT_TIMESTAMP";
    return "INSERT INTO " + Quote(p.log_table) + " (" + cols + ") VALUES (" + vals + ")";
}

std::string Trigger(const CdcPlan &p, const std::string &src, const std::string &name,
                    const std::string &event, const std::string &refrow,
                    const std::string &alias, const char op) {
    return "CREATE TRIGGER " + Quote(name) + " AFTER " + event + " ON " + Quote(src) +
           " REFERENCING " + refrow + " ROW AS " + alias + " FOR EACH ROW BEGIN " +
           LogInsert(p, alias, op) + "; END";
}

}  // namespace

CdcPlan HanaDialect::Plan(const CdcSpec &spec) const {
    if (spec.source.empty()) throw std::runtime_error("CDC: empty source table");
    if (spec.keys.empty()) throw std::runtime_error("CDC: no key columns for " + spec.source);

    const std::string tok = NameToken(spec.source);
    CdcPlan p;
    p.key_cols = spec.keys;
    // FULL_IUD logs the full row image (so inserts/updates can be upserted server-side);
    // DELETE_ONLY logs only the keys (a delete needs nothing more).
    p.log_cols = (spec.mode == CdcMode::FullIud && !spec.columns.empty()) ? spec.columns : spec.keys;
    p.log_table = spec.log_table.empty() ? "ZCDC_" + tok + "_LOG" : spec.log_table;
    p.seq_name = spec.seq_name.empty() ? "ZCDC_" + tok + "_SEQ" : spec.seq_name;
    const std::string tpfx = spec.trig_prefix.empty() ? "ZCDC_" + tok : spec.trig_prefix;

    // 1) sequence (monotonic log position)
    p.provision_ddl.push_back("CREATE SEQUENCE " + Quote(p.seq_name) +
                              " START WITH 1 INCREMENT BY 1");

    // 2) log table: key columns as NVARCHAR (HANA converts the source types on
    //    insert; the server casts back to the target's key types when applying)
    //    + op flag + monotonic seq + commit timestamp.
    std::string cols;
    for (const auto &k : p.log_cols)
        cols += Quote(k) + " NVARCHAR(" + std::to_string(spec.key_len) + "),";
    cols += Quote(p.op_col) + " NVARCHAR(1)," + Quote(p.seq_col) + " BIGINT,\"_TS\" TIMESTAMP";
    p.provision_ddl.push_back("CREATE COLUMN TABLE " + Quote(p.log_table) + " (" + cols + ")");

    // 3) trigger(s): delete-only by default; full I/U/D for column-less sources.
    auto add_trig = [&](const std::string &suffix, const std::string &event,
                        const std::string &refrow, const std::string &alias, char op) {
        const std::string name = tpfx + "_" + suffix;
        p.trigger_names.push_back(name);
        p.provision_ddl.push_back(Trigger(p, spec.source, name, event, refrow, alias, op));
    };
    add_trig("D", "DELETE", "OLD", "oldr", 'D');
    if (spec.mode == CdcMode::FullIud) {
        add_trig("I", "INSERT", "NEW", "newr", 'I');
        add_trig("U", "UPDATE", "NEW", "newr", 'U');
    }

    // read (incremental by position) + prune (watermark-driven, bounded by the
    // server-confirmed position). %POS% / %CONF% are substituted per cycle.
    std::string sel;
    for (const auto &k : p.log_cols) sel += Quote(k) + ",";
    sel += Quote(p.op_col) + "," + Quote(p.seq_col);
    p.read_sql = "SELECT " + sel + " FROM " + Quote(p.log_table) +
                 " WHERE " + Quote(p.seq_col) + " > %POS% ORDER BY " + Quote(p.seq_col);
    // read_from: the keys + op + seq (cast to INTEGER), no _TS — the ABAP ADBC reader
    // binds these cleanly where it chokes on HANA TIMESTAMP / BIGINT host types.
    std::string rcols;
    for (const auto &k : p.log_cols) rcols += Quote(k) + ",";
    rcols += Quote(p.op_col) + ",CAST(" + Quote(p.seq_col) + " AS INTEGER) AS " + Quote(p.seq_col);
    p.read_from = "(SELECT " + rcols + " FROM " + Quote(p.log_table) + ") AS LOGREAD";
    p.prune_sql = "DELETE FROM " + Quote(p.log_table) +
                  " WHERE " + Quote(p.seq_col) + " <= %CONF%";

    // teardown: drop triggers, then the log table, then the sequence.
    for (const auto &t : p.trigger_names)
        p.teardown_ddl.push_back("DROP TRIGGER " + Quote(t));
    p.teardown_ddl.push_back("DROP TABLE " + Quote(p.log_table));
    p.teardown_ddl.push_back("DROP SEQUENCE " + Quote(p.seq_name));

    return p;
}

CdcPlan AnyDbDialect::Plan(const CdcSpec &) const {
    throw std::runtime_error(
        "CDC: platform 'ANYDB' is not supported in v1 (only HANA); see ADR-0004 roadmap");
}

std::unique_ptr<CdcDialect> MakeDialect(const std::string &platform) {
    if (platform.empty() || Upper(platform) == "HANA")
        return std::make_unique<HanaDialect>();
    return std::make_unique<AnyDbDialect>();
}

}  // namespace erpl_rev
