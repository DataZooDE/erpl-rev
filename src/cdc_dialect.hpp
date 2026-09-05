// CdcDialect: generates the platform-specific SQL/DDL for the opt-in trigger-CDC
// delta tier (ADR-0004). All the CDC complexity lives in the server: given a CDC
// target spec, the dialect emits the log-table DDL, the trigger DDL(s), the
// incremental read SQL, the prune SQL and the teardown DDL — opaque strings the
// thin ABAP executor runs on the SAP database (via cl_sql_statement / ADBC) and
// streams back. ABAP makes no CDC decisions.
//
// v1 targets SAP HANA (A4H runs on HANA, so it is fully E2E-provable). The
// Dialect interface is pluggable; AnyDB ships as a stub that refuses.
#pragma once

#include <string>
#include <vector>
#include <memory>

namespace erpl_rev {

enum class CdcMode {
    DeleteOnly,  // AFTER DELETE only — inserts/updates come from the watermark tier
    KeysIud,     // AFTER INSERT/UPDATE/DELETE, logging keys only — the cycle re-reads
                 // the source for the row image. The default: cheaper on the write
                 // path of a wide hot table than logging a full image per change.
    ImageIud,    // AFTER INSERT/UPDATE/DELETE, logging the whole row — for sources
                 // that cannot be re-read cheaply. Stored as FULL_IUD before the
                 // rename; that spelling is still accepted on read, forever.
};

// What to provision. `keys` are the FULL target key columns (including the client,
// e.g. MANDT,CARRID,CONNID,FLDATE) — they identify a row for delete/upsert. Object
// names are derived in the customer namespace (ZCDC_*) from the source if blank.
struct CdcSpec {
    std::string source;               // SAP source table, e.g. "SFLIGHT"
    std::vector<std::string> keys;    // key columns, in order
    // All columns to log for IMAGE_IUD (so inserts/updates carry the row image the
    // server upserts). DELETE_ONLY and KEYS_IUD log keys only.
    std::vector<std::string> columns;
    CdcMode mode = CdcMode::DeleteOnly;
    std::string log_table;            // override; default ZCDC_<source>_LOG
    std::string seq_name;             // override; default ZCDC_<source>_SEQ
    std::string trig_prefix;          // override; default ZCDC_<source>
    int key_len = 255;                // NVARCHAR length for each key column in the log
};

// The generated plan. read_sql/prune_sql carry the placeholders %POS% / %CONF%
// (the current position / confirmed bound), substituted by the caller per cycle.
struct CdcPlan {
    std::string log_table;
    std::string seq_name;
    std::vector<std::string> trigger_names;
    std::vector<std::string> provision_ddl;  // sequence, log table, trigger(s) — in order
    std::vector<std::string> teardown_ddl;    // drop trigger(s), table, sequence
    std::string read_sql;
    // A derived-table FROM expression the ABAP ADBC reader stages with SELECT * (it
    // appends "WHERE <seq_col> > <pos>"). Excludes _TS and casts _SEQ to INTEGER so
    // every column binds in the ABAP ADBC layer (which rejects HANA TIMESTAMP/BIGINT).
    std::string read_from;
    std::string prune_sql;
    // KEYS_IUD only: the query returning the net insert/update keys the cycle has
    // to re-read from the source. Empty for the other modes.
    std::string netkeys_sql;
    std::string op_col = "_OP";
    std::string seq_col = "_SEQ";
    std::vector<std::string> key_cols;        // merge/match key columns (== spec.keys)
    std::vector<std::string> log_cols;        // columns written to the log (keys, or
                                              // all columns for IMAGE_IUD full-row)
};

class CdcDialect {
public:
    virtual ~CdcDialect() = default;
    virtual CdcPlan Plan(const CdcSpec &spec) const = 0;
    virtual std::string Name() const = 0;

    // Catalogue probe: what the DATABASE says exists, as opposed to what the
    // registry believes. Three result sets -- tables, sequences, triggers with
    // their enabled flag -- which the status derivation compares against the
    // expected object list. Without this a trigger dropped out of band (system
    // copy, transport, DBA) is invisible until rows go missing.
    virtual std::string ProbeTablesSql() const = 0;
    virtual std::string ProbeSequencesSql() const = 0;
    virtual std::string ProbeTriggersSql() const = 0;
};

class HanaDialect : public CdcDialect {
public:
    CdcPlan Plan(const CdcSpec &spec) const override;
    std::string Name() const override { return "HANA"; }
    std::string ProbeTablesSql() const override;
    std::string ProbeSequencesSql() const override;
    std::string ProbeTriggersSql() const override;
};

// AnyDB (Oracle/DB2/MSSQL/ASE): not implemented in v1 — Plan() throws.
// Not implemented: erpl-rev targets HANA-based ECC and S/4. The seam is kept
// deliberately -- it costs nothing and is what makes "HANA only" a scope choice
// rather than something baked into the design.
class AnyDbDialect : public CdcDialect {
public:
    CdcPlan Plan(const CdcSpec &spec) const override;
    std::string Name() const override { return "ANYDB"; }
    std::string ProbeTablesSql() const override;
    std::string ProbeSequencesSql() const override;
    std::string ProbeTriggersSql() const override;
};

// platform: "HANA" (default) or anything else -> AnyDbDialect (which refuses).
std::unique_ptr<CdcDialect> MakeDialect(const std::string &platform);

}  // namespace erpl_rev
