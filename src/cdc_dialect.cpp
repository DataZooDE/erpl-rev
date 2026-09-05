#include "cdc_dialect.hpp"
#include "sql_name.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace erpl_rev {

namespace {

// Name and quoting rules live in sql_name.hpp: the ZCDC_* token rule is shared
// with the per-target change log, and having two copies over two namespaces of
// customer-supplied names is exactly the bug that is hard to fix later.
using erpl_rev::sqlname::Token;
using erpl_rev::sqlname::Upper;

std::string Lower(const std::string &v) {
    std::string r = v;
    for (char &c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string Quote(const std::string &id) { return erpl_rev::sqlname::QuoteIdent(id); }

// Build the INSERT-into-log that a trigger body runs, capturing the logged columns
// (keys, or the full row image for IMAGE_IUD) plus the op flag, the next sequence
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

    const std::string tok = Token(spec.source);
    CdcPlan p;
    p.key_cols = spec.keys;
    // IMAGE_IUD logs the full row image (so inserts/updates can be upserted
    // server-side). DELETE_ONLY and KEYS_IUD log only the keys -- a delete needs
    // nothing more, and a KEYS_IUD cycle re-reads the source for the values.
    p.log_cols = (spec.mode == CdcMode::ImageIud && !spec.columns.empty()) ? spec.columns : spec.keys;
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
    if (spec.mode == CdcMode::KeysIud || spec.mode == CdcMode::ImageIud) {
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
    // read_from: the keys + op + seq (cast to INTEGER), plus the trigger's commit
    // timestamp as TEXT. The ABAP ADBC reader binds these cleanly where it chokes
    // on HANA TIMESTAMP / BIGINT host types -- hence the casts rather than the
    // native columns.
    //
    // _TS is what makes trigger replication measurable: it is the moment the
    // change committed in SAP, so the difference between it and the apply time is
    // the real end-to-end latency. Without it the trigger tier could only ever
    // report how long a cycle took, not how stale the data was.
    std::string rcols;
    for (const auto &k : p.log_cols) rcols += Quote(k) + ",";
    rcols += Quote(p.op_col) + ",CAST(" + Quote(p.seq_col) + " AS INTEGER) AS " + Quote(p.seq_col);
    rcols += ",TO_VARCHAR(\"_TS\", 'YYYYMMDDHH24MISS') AS \"_TS\"";
    p.read_from = "(SELECT " + rcols + " FROM " + Quote(p.log_table) + ") AS LOGREAD";
    p.prune_sql = "DELETE FROM " + Quote(p.log_table) +
                  " WHERE " + Quote(p.seq_col) + " <= %CONF%";

    // teardown: drop triggers, then the log table, then the sequence.
    for (const auto &t : p.trigger_names)
        p.teardown_ddl.push_back("DROP TRIGGER " + Quote(t));
    p.teardown_ddl.push_back("DROP TABLE " + Quote(p.log_table));
    p.teardown_ddl.push_back("DROP SEQUENCE " + Quote(p.seq_name));

    // The net insert/update keys a KEYS_IUD cycle must re-read. Coalescing to one
    // op per key happens HERE and not in ABAP, so the executor never has to
    // reason about what a batch of interleaved changes means -- and so a key
    // whose net op turned out to be a delete is not re-read at all.
    if (spec.mode == CdcMode::KeysIud) {
        std::string keycsv;
        for (size_t i = 0; i < spec.keys.size(); ++i) {
            if (i) keycsv += ",";
            keycsv += Lower(spec.keys[i]);
        }
        p.netkeys_sql =
            "SELECT " + keycsv + " FROM (SELECT * FROM " + p.log_table + "__cdclog"
            " QUALIFY row_number() OVER (PARTITION BY " + keycsv +
            " ORDER BY \"" + p.seq_col + "\" DESC)=1) WHERE lower(\"" + p.op_col +
            "\") IN ('i','u')";
    }

    return p;
}

// HANA's own catalogue views. Restricted to the ZCDC_ namespace and the current
// schema, so the probe cannot see -- let alone report -- customer objects.
std::string HanaDialect::ProbeTablesSql() const {
    return "SELECT TABLE_NAME FROM SYS.TABLES WHERE SCHEMA_NAME = CURRENT_SCHEMA "
           "AND TABLE_NAME LIKE 'ZCDC/_%' ESCAPE '/'";
}

std::string HanaDialect::ProbeSequencesSql() const {
    return "SELECT SEQUENCE_NAME FROM SYS.SEQUENCES WHERE SCHEMA_NAME = CURRENT_SCHEMA "
           "AND SEQUENCE_NAME LIKE 'ZCDC/_%' ESCAPE '/'";
}

std::string HanaDialect::ProbeTriggersSql() const {
    // IS_VALID matters as much as existence: an invalid trigger is present in the
    // catalogue and fires nothing, which looks healthy and captures nothing.
    return "SELECT TRIGGER_NAME, IS_VALID FROM SYS.TRIGGERS WHERE SCHEMA_NAME = CURRENT_SCHEMA "
           "AND TRIGGER_NAME LIKE 'ZCDC/_%' ESCAPE '/'";
}

std::string AnyDbDialect::ProbeTablesSql() const {
    throw std::runtime_error("CDC: only SAP HANA is supported in this release");
}
std::string AnyDbDialect::ProbeSequencesSql() const {
    throw std::runtime_error("CDC: only SAP HANA is supported in this release");
}
std::string AnyDbDialect::ProbeTriggersSql() const {
    throw std::runtime_error("CDC: only SAP HANA is supported in this release");
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
