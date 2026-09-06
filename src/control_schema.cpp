#include "control_schema.hpp"

#include <stdexcept>

namespace erpl_rev {
namespace schema {

namespace {

void Exec(duckdb::Connection &con, const std::string &sql, const char *what) {
    auto r = con.Query(sql);
    if (r->HasError())
        throw std::runtime_error(std::string("control schema: ") + what + " failed: " +
                                 r->GetError());
}

bool ColumnExists(duckdb::Connection &con, const std::string &table, const std::string &column) {
    auto r = con.Query("SELECT count(*) FROM duckdb_columns() WHERE lower(table_name)=lower('" +
                       table + "') AND lower(column_name)=lower('" + column + "')");
    if (r->HasError()) return false;
    return r->RowCount() > 0 && r->GetValue(0, 0).GetValue<int64_t>() > 0;
}

}  // namespace

void AddColumnIfMissing(duckdb::Connection &con, const std::string &table,
                        const std::string &column, const std::string &type) {
    if (ColumnExists(con, table, column)) return;
    Exec(con, "ALTER TABLE " + table + " ADD COLUMN " + column + " " + type,
         "add column");
}

const std::vector<Migration> &Migrations() {
    // clang-format off
    static const std::vector<Migration> kMigrations = {

    // ---------------------------------------------------------------------
    // v1 -- the schema as it stood before versioning existed.
    //
    // VERBATIM from DuckDbBridge's constructor at 1a97fbb. Do not reformat, do
    // not "improve", do not add columns here. Its only job is to be a no-op
    // against every file already in the field, so that those files can then
    // receive v2 and later. A cleaned-up v1 would not match them.
    // ---------------------------------------------------------------------
    {1, "base schema (pre-versioning)", {
        "CREATE TABLE IF NOT EXISTS _erpl_rev_delta_state ("
        "target VARCHAR PRIMARY KEY, method VARCHAR NOT NULL, source_from VARCHAR NOT NULL, "
        "keys VARCHAR NOT NULL, chg_col VARCHAR, wm_kind VARCHAR, wm_value VARCHAR, "
        "safety_secs INTEGER DEFAULT 120, cadence VARCHAR DEFAULT 'nightly', extra VARCHAR, "
        "last_run_ts TIMESTAMPTZ, rows_applied BIGINT, status VARCHAR DEFAULT 'IDLE', "
        "lease_ts TIMESTAMPTZ, last_error VARCHAR)",

        "CREATE SEQUENCE IF NOT EXISTS _erpl_rev_run_seq START 1",

        "CREATE TABLE IF NOT EXISTS _erpl_rev_run_stats ("
        "run_id BIGINT PRIMARY KEY DEFAULT nextval('_erpl_rev_run_seq'), "
        "ts TIMESTAMPTZ DEFAULT now(), "
        "target VARCHAR, source VARCHAR, run_type VARCHAR, method VARCHAR, status VARCHAR, "
        "duration_ms BIGINT, rows_read BIGINT, rows_ins BIGINT, rows_upd BIGINT, rows_del BIGINT, "
        "wm_from VARCHAR, wm_to VARCHAR, jobs INTEGER, error_text VARCHAR)",

        "CREATE TABLE IF NOT EXISTS _erpl_rev_cdc ("
        "target VARCHAR PRIMARY KEY, source VARCHAR NOT NULL, keys VARCHAR NOT NULL, "
        "platform VARCHAR DEFAULT 'HANA', mode VARCHAR DEFAULT 'DELETE_ONLY', "
        "status VARCHAR DEFAULT 'PROVISIONED', log_table VARCHAR, position BIGINT DEFAULT 0, "
        "provisioned_ts TIMESTAMPTZ, seeded_ts TIMESTAMPTZ, last_run_ts TIMESTAMPTZ, error VARCHAR)",

        "CREATE SEQUENCE IF NOT EXISTS _erpl_rev_cli_seq START 1",

        "CREATE TABLE IF NOT EXISTS _erpl_rev_cli_cmd ("
        "cmd_id BIGINT PRIMARY KEY, created_ts TIMESTAMPTZ DEFAULT now(), "
        "verb VARCHAR NOT NULL, params VARCHAR NOT NULL, "
        "status VARCHAR DEFAULT 'PENDING', "
        "claimed_ts TIMESTAMPTZ, finished_ts TIMESTAMPTZ, "
        "result VARCHAR, error VARCHAR)",
    }},

    // ---------------------------------------------------------------------
    // v2 -- what the delta engine needs to be correct and schedulable.
    //
    // Landed together, and ahead of the milestones that consume some of them:
    // adding a column is free, but a migration event on a customer system is
    // not, so they are batched.
    // ---------------------------------------------------------------------
    {2, "delta state: watermark, load type, scheduling, logging", {
        // Watermark math. time_col is the TIMS half of a DATS+TIMS pair;
        // safety_units is the offset for a counter watermark, where a duration
        // in seconds means nothing.
        "SELECT 1",  // placeholder: the ALTERs run in the code path below
    }},

    {3, "cdc: mode rename and operations columns", {
        // The trigger mode is a stored string, so renaming the enum in C++ is
        // only half the change. Rewrite the value; never touch the position,
        // because a rename must not re-seed anyone's trigger set.
        "UPDATE _erpl_rev_cdc SET mode='IMAGE_IUD' WHERE mode='FULL_IUD'",
    }},

    {4, "run stats: load type, portions, validation, lag", {
        "SELECT 1",
    }},

    // v5 -- the streaming daemon's singleton row. One row, ever: a second daemon
    // claiming it is how a duplicate start is detected and turned into
    // "attach and report" rather than two daemons running the same targets.
    // v6 -- the SAP-to-server clock offset, recorded per run.
    //
    // A DATS/TIMS change value is wall-clock in SAP's timezone, which the server
    // cannot know. It CAN know the difference: BEGIN_CYCLE is told SAP's clock
    // and compares it with its own. Storing that per run turns a wall-clock
    // value into a real instant without a timezone database and without
    // assuming the two machines agree.
    {5, "daemon: singleton row and tick budget", {
        "CREATE TABLE IF NOT EXISTS _erpl_rev_daemon ("
        "id INTEGER PRIMARY KEY DEFAULT 1, instance_id VARCHAR, heartbeat_ts TIMESTAMPTZ, "
        "tick_secs INTEGER DEFAULT 2, max_workers INTEGER DEFAULT 2, "
        "full_load_share DOUBLE DEFAULT 0.5, status VARCHAR DEFAULT 'STOPPED', "
        "stop BOOLEAN DEFAULT false, ticks BIGINT DEFAULT 0, started_ts TIMESTAMPTZ)",
        "INSERT INTO _erpl_rev_daemon (id) SELECT 1 "
        "WHERE NOT EXISTS (SELECT 1 FROM _erpl_rev_daemon WHERE id=1)",
    }},

    // v6 -- the SAP-to-server clock offset, recorded per run.
    //
    // A DATS/TIMS change value is wall-clock in SAP's timezone, which the server
    // cannot know. It CAN know the DIFFERENCE: BEGIN_CYCLE is told SAP's clock
    // and compares it with its own. Storing that per run turns a wall-clock
    // value into a real instant without a timezone database and without assuming
    // the two machines agree -- which, measured on A4H, they did not.
    {6, "run stats: SAP-to-server clock offset", {
        "SELECT 1",
    }},

    // v7 -- the one flag that lets a reload empty a target.
    //
    // A reload that staged no rows is ambiguous: the source really is empty, or
    // a wrong client, a bad filter or a mistyped source name matched nothing.
    // The reader cannot tell the two apart -- it DROPs and CREATEs its staging
    // table before its first SELECT, so an empty stage is what both look like.
    // One reading deletes a replica of any size and reports SUCCESS, and it is
    // not recoverable; the other is one flag and one re-run away.
    {7, "delta state: allow a reload to empty a target", {
        "SELECT 1",
    }},

    };
    // clang-format on
    return kMigrations;
}

int LatestVersion() { return Migrations().back().version; }

int CurrentVersion(duckdb::Connection &con) {
    auto t = con.Query("SELECT count(*) FROM duckdb_tables() "
                       "WHERE table_name='_erpl_rev_schema_version'");
    if (t->HasError() || t->RowCount() == 0) return 0;
    if (t->GetValue(0, 0).GetValue<int64_t>() == 0) return 0;
    auto v = con.Query("SELECT coalesce(max(version),0) FROM _erpl_rev_schema_version");
    if (v->HasError() || v->RowCount() == 0) return 0;
    return static_cast<int>(v->GetValue(0, 0).GetValue<int64_t>());
}

namespace {

// The column additions for a version. Kept as code rather than SQL strings so
// they can go through AddColumnIfMissing and stay re-runnable.
void ApplyColumnAdds(duckdb::Connection &con, int version) {
    if (version == 2) {
        const char *st = "_erpl_rev_delta_state";
        AddColumnIfMissing(con, st, "time_col", "VARCHAR");
        AddColumnIfMissing(con, st, "safety_units", "BIGINT DEFAULT 0");
        // The fencing token a cycle commits against. The lease is advisory and
        // can expire under a long-running healthy cycle; this cannot.
        AddColumnIfMissing(con, st, "active_run_id", "BIGINT");
        AddColumnIfMissing(con, st, "load_type_default", "VARCHAR DEFAULT 'D'");
        AddColumnIfMissing(con, st, "fail_count", "INTEGER DEFAULT 0");
        AddColumnIfMissing(con, st, "parked_until", "TIMESTAMPTZ");
        AddColumnIfMissing(con, st, "park_reason", "VARCHAR");
        // Also the lease budget: a cycle may legitimately block far longer than
        // the old fixed 600s, so the TTL is derived from this.
        AddColumnIfMissing(con, st, "max_cycle_secs", "INTEGER DEFAULT 3600");
        AddColumnIfMissing(con, st, "log_enabled", "BOOLEAN DEFAULT false");
        AddColumnIfMissing(con, st, "log_full_loads", "BOOLEAN DEFAULT false");
        AddColumnIfMissing(con, st, "xform_view", "VARCHAR");
        AddColumnIfMissing(con, st, "validation_policy", "VARCHAR");
    } else if (version == 3) {
        const char *cdc = "_erpl_rev_cdc";
        AddColumnIfMissing(con, cdc, "trigger_table", "VARCHAR");
        AddColumnIfMissing(con, cdc, "last_probe_ts", "TIMESTAMPTZ");
        AddColumnIfMissing(con, cdc, "shadow_rows", "BIGINT");
        AddColumnIfMissing(con, cdc, "shadow_oldest_ts", "TIMESTAMPTZ");
        AddColumnIfMissing(con, cdc, "tuning", "VARCHAR");
    } else if (version == 4) {
        const char *rs = "_erpl_rev_run_stats";
        AddColumnIfMissing(con, rs, "load_type", "VARCHAR");
        AddColumnIfMissing(con, rs, "portion_count", "INTEGER");
        AddColumnIfMissing(con, rs, "validation_status", "VARCHAR");
        AddColumnIfMissing(con, rs, "lag_ms", "BIGINT");
    } else if (version == 6) {
        AddColumnIfMissing(con, "_erpl_rev_run_stats", "clock_skew_secs", "BIGINT DEFAULT 0");
    } else if (version == 7) {
        AddColumnIfMissing(con, "_erpl_rev_delta_state", "allow_empty_reload",
                           "BOOLEAN DEFAULT false");
    }
}

}  // namespace

void Migrate(duckdb::Connection &con, const std::string &binary_version) {
    const int have = CurrentVersion(con);
    const int want = LatestVersion();

    if (have > want)
        throw std::runtime_error(
            "control schema: this DuckDB file is at version " + std::to_string(have) +
            ", but this erpl-rev binary only knows version " + std::to_string(want) +
            ". Refusing to open it -- run the newer binary, or point --db at another file.");

    Exec(con,
         "CREATE TABLE IF NOT EXISTS _erpl_rev_schema_version ("
         "version INTEGER PRIMARY KEY, name VARCHAR, "
         "applied_ts TIMESTAMPTZ DEFAULT now(), binary_version VARCHAR)",
         "create version table");

    for (const auto &m : Migrations()) {
        if (m.version <= have) continue;

        // Deliberately NOT wrapped in an explicit transaction. DuckDB refuses to
        // commit two ALTER TABLEs against the same table in one transaction
        // ("another transaction has altered this table"), and a migration that
        // adds several columns to one table is the normal case.
        //
        // That is safe here because every step is idempotent -- AddColumnIfMissing
        // probes duckdb_columns(), the data rewrites carry their own WHERE -- and
        // because the version row is written only after every step succeeded. A
        // crash mid-migration therefore leaves the version unrecorded, and the
        // next boot simply re-runs the whole step, skipping what already landed.
        // The idempotence test is what keeps this true.
        for (const auto &stmt : m.sql)
            if (stmt != "SELECT 1") Exec(con, stmt, m.name);
        ApplyColumnAdds(con, m.version);
        Exec(con,
             "INSERT INTO _erpl_rev_schema_version (version, name, binary_version) VALUES (" +
                 std::to_string(m.version) + ", '" + m.name + "', '" + binary_version + "')",
             "record version");
    }
}

}  // namespace schema
}  // namespace erpl_rev
