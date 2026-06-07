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
    FullIud,     // AFTER INSERT/UPDATE/DELETE — for sources without a usable change column
};

// What to provision. `keys` are the FULL target key columns (including the client,
// e.g. MANDT,CARRID,CONNID,FLDATE) — they identify a row for delete/upsert. Object
// names are derived in the customer namespace (ZCDC_*) from the source if blank.
struct CdcSpec {
    std::string source;               // SAP source table, e.g. "SFLIGHT"
    std::vector<std::string> keys;    // key columns, in order
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
    std::string prune_sql;
    std::string op_col = "_OP";
    std::string seq_col = "_SEQ";
    std::vector<std::string> key_cols;        // log key column names (== spec.keys)
};

class CdcDialect {
public:
    virtual ~CdcDialect() = default;
    virtual CdcPlan Plan(const CdcSpec &spec) const = 0;
    virtual std::string Name() const = 0;
};

class HanaDialect : public CdcDialect {
public:
    CdcPlan Plan(const CdcSpec &spec) const override;
    std::string Name() const override { return "HANA"; }
};

// AnyDB (Oracle/DB2/MSSQL/ASE): not implemented in v1 — Plan() throws.
class AnyDbDialect : public CdcDialect {
public:
    CdcPlan Plan(const CdcSpec &spec) const override;
    std::string Name() const override { return "ANYDB"; }
};

// platform: "HANA" (default) or anything else -> AnyDbDialect (which refuses).
std::unique_ptr<CdcDialect> MakeDialect(const std::string &platform);

}  // namespace erpl_rev
