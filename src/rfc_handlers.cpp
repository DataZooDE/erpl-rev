#include "rfc_handlers.hpp"
#include "rfc_metadata.hpp"
#include "sap_uc.hpp"
#include "duckdb_bridge.hpp"
#include "cdc_dialect.hpp"
#include "json_util.hpp"
#include "logging.hpp"
#include "erpl_rev_telemetry.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace erpl_rev {

namespace {

// RAII telemetry scope for a bridge FM handler. Times the call and, on
// destruction, emits one `rfc_call` feature with {fm, status, duration_ms}.
// `fm` is a fixed enum; the SQL/target/handle/error text NEVER leave the box.
// Call fail(error_class) from the catch block to mark it failed and emit an
// enumerated $exception. Emits through GlobalTelemetry() because handlers run
// on SAP SDK worker threads.
class RfcCallScope {
public:
    explicit RfcCallScope(const char *fm)
        : fm_(fm), t0_(std::chrono::steady_clock::now()) {}
    ~RfcCallScope() {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0_).count();
        GlobalTelemetry().rfcCall(fm_, ok_, ms);
    }
    RfcCallScope(const RfcCallScope &) = delete;
    RfcCallScope &operator=(const RfcCallScope &) = delete;

    void fail(const char *error_class) {
        ok_ = false;
        GlobalTelemetry().error(error_class, fm_);
    }

private:
    const char *fm_;
    std::chrono::steady_clock::time_point t0_;
    bool ok_ = true;
};

// One shared DuckDB for the server lifetime. Query/Ingest/cursor handlers each use
// their OWN duckdb::Connection (DuckDB allows many concurrent connections on one DB),
// so normal data operations need NO global lock and run in parallel. g_mtx guards
// only the quack network server start/stop (extension load is not thread-safe).
std::unique_ptr<DuckDbBridge> g_bridge;
std::mutex g_mtx;

// Default cap on rows returned by Z_DUCKDB_QUERY. A SQL console can issue a
// SELECT over millions of rows; returning them all as one JSON string overflows
// the RFC string buffer (RfcUTF8ToSAPUC -> RFC_BUFFER_TOO_SMALL) and the ABAP
// JSON parser / ALV. Cap the rows shipped (the true total is still reported);
// override with ERPL_REV_QUERY_MAX_ROWS (0 = unlimited).
long long QueryRowCap() {
    const char *v = std::getenv("ERPL_REV_QUERY_MAX_ROWS");
    if (v && *v) { try { return std::stoll(v); } catch (...) {} }
    return 10000;
}

// Split a comma-separated list, trimming spaces; empty input => empty vector.
std::vector<std::string> SplitCsv(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    std::stringstream ss(s);
    while (std::getline(ss, cur, ',')) {
        size_t a = cur.find_first_not_of(" \t");
        size_t b = cur.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
    }
    return out;
}

// Build a JSON array string from the row JSON objects.
std::string RowsToJsonArray(const std::vector<std::string> &rows) {
    std::string out = "[";
    for (size_t i = 0; i < rows.size(); i++) { if (i) out += ","; out += rows[i]; }
    out += "]";
    return out;
}

// Replace every occurrence of `from` in `s` with `to`.
std::string ReplaceAll(std::string s, const std::string &from, const std::string &to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    return s;
}
std::string UpperOf(std::string s) {
    for (char &c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}
CdcMode CdcModeOf(const std::string &m) {
    return UpperOf(m) == "FULL_IUD" ? CdcMode::FullIud : CdcMode::DeleteOnly;
}
// A JSON array of plain (escaped, quoted) strings.
std::string JsonStrArray(const std::vector<std::string> &v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); i++) { if (i) out += ","; out += json::QuoteString(v[i]); }
    return out + "]";
}

// Build a JSON array of {"name":..,"type":..} from the columns.
std::string ColumnsToJson(const std::vector<QueryColumn> &cols) {
    std::string out = "[";
    for (size_t i = 0; i < cols.size(); i++) {
        if (i) out += ",";
        out += "{\"name\":" + json::QuoteString(cols[i].name) +
               ",\"type\":" + json::QuoteString(cols[i].type) + "}";
    }
    out += "]";
    return out;
}

} // namespace

void InstallHandlers(const std::string &db_path, const std::string &init_sql) {
    g_bridge = std::make_unique<DuckDbBridge>(db_path, init_sql);   // empty path => in-memory

    RFC_ERROR_INFO info;
    if (RfcInstallServerFunction(nullptr, BuildPingDesc(), ZPingImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(STFC_CONNECTION)", info);
    if (RfcInstallServerFunction(nullptr, BuildQueryDesc(), ZQueryImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_QUERY)", info);
    if (RfcInstallServerFunction(nullptr, BuildIngestDesc(), ZIngestImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_INGEST)", info);
    if (RfcInstallServerFunction(nullptr, BuildSnapshotMergeDesc(), ZSnapshotMergeImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_SNAPSHOT_MERGE)", info);
    if (RfcInstallServerFunction(nullptr, BuildOpenDesc(), ZOpenImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_OPEN)", info);
    if (RfcInstallServerFunction(nullptr, BuildFetchDesc(), ZFetchImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_FETCH)", info);
    if (RfcInstallServerFunction(nullptr, BuildCloseDesc(), ZCloseImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_CLOSE)", info);
    if (RfcInstallServerFunction(nullptr, BuildCdcPlanDesc(), ZCdcPlanImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_CDC_PLAN)", info);
    if (RfcInstallServerFunction(nullptr, BuildCdcApplyDesc(), ZCdcApplyImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_CDC_APPLY)", info);

    log::get().Debug("rfc", "handlers installed",
                     {{"functions", "STFC_CONNECTION,Z_DUCKDB_QUERY,Z_DUCKDB_INGEST,"
                                    "Z_DUCKDB_SNAPSHOT_MERGE,Z_DUCKDB_CDC_PLAN,Z_DUCKDB_CDC_APPLY,"
                                    "Z_DUCKDB_OPEN,Z_DUCKDB_FETCH,Z_DUCKDB_CLOSE", true},
                      {"db", db_path.empty() ? ":memory:" : db_path}});
}

void ShutdownHandlers() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_bridge.reset();   // close DuckDB + attached catalogs while the runtime is alive
}

std::string StartQuackServer(const std::string &listen, bool allow_other_host,
                             const std::string &token) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_bridge->StartQuack(listen, allow_other_host, token);
}

void StopQuackServer(const std::string &listen) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_bridge->StopQuack(listen);
}

void StartTunnelForward(const std::string &import_sql) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_bridge->StartTunnel(import_sql);
}

std::string TunnelForwardInfo(const std::string &local_port) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_bridge->TunnelInfo(local_port);
}

} // namespace erpl_rev

using namespace erpl_rev;

extern "C" RFC_RC SAP_API ZPingImpl(RFC_CONNECTION_HANDLE,
                                    RFC_FUNCTION_HANDLE funcHandle,
                                    RFC_ERROR_INFO *errorInfo) {
    RfcCallScope _tc("ping");
    try {
        std::string req = GetChars(funcHandle, "REQUTEXT", 255);
        SetChars(funcHandle, "ECHOTEXT", req);
        SetChars(funcHandle, "RESPTEXT", "PONG from erpl-rev: " + req);
        log::get().Info("rfc", "STFC_CONNECTION", {{"req", req}});
        return RFC_OK;
    } catch (const std::exception &e) {
        _tc.fail("rfc_error");
        log::get().Error("rfc", "STFC_CONNECTION failed", {{"error", e.what()}});
        std::memset(errorInfo, 0, sizeof(*errorInfo));
        errorInfo->code = RFC_EXTERNAL_FAILURE;
        auto m = std2uc(std::string("ping failed: ") + e.what());
        uccpy(errorInfo->message, m.data(), sizeof(errorInfo->message)/sizeof(SAP_UC));
        return RFC_EXTERNAL_FAILURE;
    }
}

// Data FMs report problems via EV_ERROR (return RFC_OK) so ABAP gets a clean
// error string instead of a SYSTEM_FAILURE exception.
extern "C" RFC_RC SAP_API ZQueryImpl(RFC_CONNECTION_HANDLE,
                                     RFC_FUNCTION_HANDLE funcHandle,
                                     RFC_ERROR_INFO *) {
    RfcCallScope _tc("query");
    try {
        std::string sql = GetString(funcHandle, "IV_SQL");
        log::get().Info("rfc", "Z_DUCKDB_QUERY", {{"sql", sql}});

        const long long cap = QueryRowCap();
        // No global lock: Query opens its own DuckDB connection, so reads run in
        // parallel (with each other and with ingests).
        QueryResult qr = g_bridge->Query(sql, cap);

        SetString(funcHandle, "EV_COLUMNS",   ColumnsToJson(qr.columns));
        SetString(funcHandle, "EV_ROWS",      RowsToJsonArray(qr.rows));
        // EV_ROW_COUNT is the TOTAL the query produced; EV_ROWS holds at most
        // `cap` of them. ABAP compares the two to know it was truncated.
        SetString(funcHandle, "EV_ROW_COUNT", std::to_string(qr.row_count));
        SetString(funcHandle, "EV_ERROR",     "");
        log::get().Debug("rfc", "Z_DUCKDB_QUERY ok",
                         {{"total", (long long)qr.row_count},
                          {"shipped", (long long)qr.rows.size()},
                          {"truncated", qr.truncated ? "true" : "false", false}});
    } catch (const std::exception &e) {
        _tc.fail("sql_error");
        log::get().Error("rfc", "Z_DUCKDB_QUERY failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_ERROR", e.what());
    }
    return RFC_OK;
}

extern "C" RFC_RC SAP_API ZIngestImpl(RFC_CONNECTION_HANDLE,
                                      RFC_FUNCTION_HANDLE funcHandle,
                                      RFC_ERROR_INFO *) {
    RfcCallScope _tc("ingest");
    try {
        std::string target  = GetString(funcHandle, "IV_TARGET");
        std::string mode    = GetString(funcHandle, "IV_MODE");
        std::string keys    = GetString(funcHandle, "IV_KEYS");
        std::string pqout   = GetString(funcHandle, "IV_PARQUET_OUT");
        std::string initsql = GetString(funcHandle, "IV_INIT_SQL");
        std::string ddl     = GetString(funcHandle, "IV_DDL");
        std::string data    = GetString(funcHandle, "IV_DATA");
        std::string xdata   = GetXString(funcHandle, "IV_XDATA");  // binary sXML rows
        std::string opcol   = GetString(funcHandle, "IV_OP_COL");  // I/U/D col for MERGE
        log::get().Info("rfc", "Z_DUCKDB_INGEST",
                        {{"target", target}, {"mode", mode}, {"keys", keys},
                         {"op_col", opcol},
                         {"init_len", (long long)initsql.size()},
                         {"ddl_len", (long long)ddl.size()},
                         {"xdata_len", (long long)xdata.size()}});

        auto upper = mode;
        for (char &c : upper) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        IngestMode m = (upper == "MERGE")  ? IngestMode::Merge
                     : (upper == "UPSERT") ? IngestMode::Upsert
                                           : IngestMode::Insert;
        // No global lock: each ingest opens its own DuckDB connection (its TEMP
        // staging is connection-local), so concurrent packages (async pipeline /
        // partitioned loads) ingest in parallel. Full-load is pure INSERT (never
        // conflicts); UPSERT packages are disjoint key ranges. MERGE applies an
        // I/U/D delta package (IV_OP_COL) on one connection-local transaction.
        // Prefer the binary-sXML payload (replicate path); else JSON.
        long long n = xdata.empty()
                ? g_bridge->Ingest(target, data, m, SplitCsv(keys), pqout, initsql, ddl)
                : g_bridge->IngestBxml(target, xdata, m, SplitCsv(keys), pqout, initsql, ddl, opcol);
        SetString(funcHandle, "EV_ROWS_AFFECTED", std::to_string(n));
        SetString(funcHandle, "EV_ERROR", "");
        log::get().Debug("rfc", "Z_DUCKDB_INGEST ok", {{"rows_affected", n}});
    } catch (const std::exception &e) {
        _tc.fail("ingest_error");
        log::get().Error("rfc", "Z_DUCKDB_INGEST failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_ERROR", e.what());
    }
    return RFC_OK;
}

// Snapshot diff/merge: upsert a freshly landed full snapshot onto the target and
// delete the target keys absent from it (physical-delete reconciliation for the
// SNAPSHOT delta method), in one server transaction. Counts come back as strings.
extern "C" RFC_RC SAP_API ZSnapshotMergeImpl(RFC_CONNECTION_HANDLE,
                                             RFC_FUNCTION_HANDLE funcHandle,
                                             RFC_ERROR_INFO *) {
    RfcCallScope _tc("snapshot_merge");
    try {
        std::string target  = GetString(funcHandle, "IV_TARGET");
        std::string staging = GetString(funcHandle, "IV_STAGING");
        std::string keys    = GetString(funcHandle, "IV_KEYS");
        log::get().Info("rfc", "Z_DUCKDB_SNAPSHOT_MERGE",
                        {{"target", target}, {"staging", staging}, {"keys", keys}});

        SnapshotResult r = g_bridge->SnapshotMerge(target, staging, SplitCsv(keys));
        SetString(funcHandle, "EV_INS",   std::to_string(r.ins));
        SetString(funcHandle, "EV_UPD",   std::to_string(r.upd));
        SetString(funcHandle, "EV_DEL",   std::to_string(r.del));
        SetString(funcHandle, "EV_ERROR", "");
        log::get().Debug("rfc", "Z_DUCKDB_SNAPSHOT_MERGE ok",
                         {{"ins", r.ins}, {"upd", r.upd}, {"del", r.del}});
    } catch (const std::exception &e) {
        _tc.fail("sql_error");
        log::get().Error("rfc", "Z_DUCKDB_SNAPSHOT_MERGE failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_ERROR", e.what());
    }
    return RFC_OK;
}

// --- Trigger-CDC FMs --------------------------------------------------------
// PLAN registers the target + generates ALL the platform SQL (provision/teardown
// DDL, read/prune SQL) as one opaque JSON plan; ABAP just executes the strings.
// IV_ACTION: PROVISION (register, return provision DDL), CYCLE (return read SQL
// with the current position, no re-register), SEED / DISABLE (state transitions).
extern "C" RFC_RC SAP_API ZCdcPlanImpl(RFC_CONNECTION_HANDLE,
                                       RFC_FUNCTION_HANDLE funcHandle,
                                       RFC_ERROR_INFO *) {
    RfcCallScope _tc("cdc_plan");
    try {
        std::string target   = GetString(funcHandle, "IV_TARGET");
        std::string source   = GetString(funcHandle, "IV_SOURCE");
        std::string keys     = GetString(funcHandle, "IV_KEYS");
        std::string mode     = GetString(funcHandle, "IV_MODE");
        std::string platform = GetString(funcHandle, "IV_PLATFORM");
        std::string action   = UpperOf(GetString(funcHandle, "IV_ACTION"));
        if (action.empty()) action = "PROVISION";
        log::get().Info("rfc", "Z_DUCKDB_CDC_PLAN", {{"target", target}, {"action", action}});

        if (action == "SEED") {
            g_bridge->CdcSetStatus(target, "SEEDED");
            SetString(funcHandle, "EV_PLAN", "");
            SetString(funcHandle, "EV_ERROR", "");
            return RFC_OK;
        }

        // Spec comes from the params (PROVISION) or the existing state (CYCLE/DISABLE).
        CdcState st = g_bridge->CdcGet(target);
        std::string src  = source.empty()   ? st.source   : source;
        std::string ks   = keys.empty()     ? st.keys     : keys;
        std::string md   = mode.empty()     ? (st.mode.empty() ? "DELETE_ONLY" : st.mode) : mode;
        std::string plat = platform.empty() ? (st.platform.empty() ? "HANA" : st.platform) : platform;
        if (src.empty() || ks.empty())
            throw std::runtime_error("CDC: source and keys required to plan " + target);

        CdcSpec spec;
        spec.source = src;
        spec.keys = SplitCsv(ks);
        spec.mode = CdcModeOf(md);
        // FULL_IUD logs the full row image: take the column set from the (seeded)
        // DuckDB target and upper-case it to the SAP/HANA column names the triggers
        // reference (replicate lower-cases on the way in).
        if (spec.mode == CdcMode::FullIud) {
            QueryResult tc = g_bridge->Query("SELECT * FROM " + target + " LIMIT 0");
            for (auto &c : tc.columns) spec.columns.push_back(UpperOf(c.name));
        }
        CdcPlan plan = MakeDialect(plat)->Plan(spec);

        if (action == "PROVISION")
            g_bridge->CdcRegister(target, src, ks, plat, md, plan.log_table);
        else if (action == "DISABLE")
            g_bridge->CdcSetStatus(target, "DISABLED");

        const long long pos = g_bridge->CdcGet(target).position;
        const std::string read = ReplaceAll(plan.read_sql, "%POS%", std::to_string(pos));

        std::string js = "{";
        js += "\"log_table\":"       + json::QuoteString(plan.log_table);
        js += ",\"seq_name\":"       + json::QuoteString(plan.seq_name);
        js += ",\"op_col\":"         + json::QuoteString(plan.op_col);
        js += ",\"seq_col\":"        + json::QuoteString(plan.seq_col);
        js += ",\"position\":"       + std::to_string(pos);
        js += ",\"key_cols\":"       + JsonStrArray(plan.key_cols);
        js += ",\"provision_ddl\":"  + JsonStrArray(plan.provision_ddl);
        js += ",\"teardown_ddl\":"   + JsonStrArray(plan.teardown_ddl);
        js += ",\"read_sql\":"       + json::QuoteString(read);
        js += ",\"read_from\":"      + json::QuoteString(plan.read_from);
        js += ",\"prune_sql\":"      + json::QuoteString(plan.prune_sql);
        js += "}";
        SetString(funcHandle, "EV_PLAN", js);
        SetString(funcHandle, "EV_ERROR", "");
    } catch (const std::exception &e) {
        _tc.fail("cdc_error");
        log::get().Error("rfc", "Z_DUCKDB_CDC_PLAN failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_PLAN", "");
        SetString(funcHandle, "EV_ERROR", e.what());
    }
    return RFC_OK;
}

// APPLY consumes one staged log batch: coalesce -> MERGE I/U/D -> advance position
// -> drop staging, atomically. Returns counts + the prune bound ABAP deletes up to.
extern "C" RFC_RC SAP_API ZCdcApplyImpl(RFC_CONNECTION_HANDLE,
                                        RFC_FUNCTION_HANDLE funcHandle,
                                        RFC_ERROR_INFO *) {
    RfcCallScope _tc("cdc_apply");
    try {
        std::string target  = GetString(funcHandle, "IV_TARGET");
        std::string staging = GetString(funcHandle, "IV_STAGING");
        std::string keys    = GetString(funcHandle, "IV_KEYS");
        log::get().Info("rfc", "Z_DUCKDB_CDC_APPLY",
                        {{"target", target}, {"staging", staging}, {"keys", keys}});
        CdcApplyResult r = g_bridge->CdcApply(target, staging, SplitCsv(keys));
        SetString(funcHandle, "EV_INS",     std::to_string(r.ins));
        SetString(funcHandle, "EV_UPD",     std::to_string(r.upd));
        SetString(funcHandle, "EV_DEL",     std::to_string(r.del));
        SetString(funcHandle, "EV_PRUNE",   std::to_string(r.prune_bound));
        SetString(funcHandle, "EV_APPLIED", r.applied ? "X" : "");
        SetString(funcHandle, "EV_ERROR",   "");
        log::get().Debug("rfc", "Z_DUCKDB_CDC_APPLY ok",
                         {{"ins", r.ins}, {"upd", r.upd}, {"del", r.del},
                          {"prune", r.prune_bound}});
    } catch (const std::exception &e) {
        _tc.fail("cdc_error");
        log::get().Error("rfc", "Z_DUCKDB_CDC_APPLY failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_ERROR", e.what());
    }
    return RFC_OK;
}

// --- Streaming cursor FMs ---------------------------------------------------
// OPEN starts a streaming cursor and returns its handle + column metadata (JSON)
// so ABAP can build its target structure once. FETCH returns one BXML page
// (XSTRING) plus fetched/done. CLOSE frees it. Errors come back via EV_ERROR.
extern "C" RFC_RC SAP_API ZOpenImpl(RFC_CONNECTION_HANDLE,
                                    RFC_FUNCTION_HANDLE funcHandle,
                                    RFC_ERROR_INFO *) {
    RfcCallScope _tc("cursor_open");
    try {
        std::string sql = GetString(funcHandle, "IV_SQL");
        log::get().Info("rfc", "Z_DUCKDB_OPEN", {{"sql", sql}});
        // No global lock: OpenCursor uses its own connection + an internal
        // handle-map mutex, so cursors open/fetch in parallel with everything else.
        CursorOpen open = g_bridge->OpenCursor(sql);
        SetString(funcHandle, "EV_HANDLE",  open.handle);
        SetString(funcHandle, "EV_COLUMNS", ColumnsToJson(open.columns));
        SetString(funcHandle, "EV_ERROR",   "");
        log::get().Debug("rfc", "Z_DUCKDB_OPEN ok",
                         {{"handle", open.handle},
                          {"cols", (long long)open.columns.size()}});
    } catch (const std::exception &e) {
        _tc.fail("sql_error");
        log::get().Error("rfc", "Z_DUCKDB_OPEN failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_ERROR", e.what());
    }
    return RFC_OK;
}

extern "C" RFC_RC SAP_API ZFetchImpl(RFC_CONNECTION_HANDLE,
                                     RFC_FUNCTION_HANDLE funcHandle,
                                     RFC_ERROR_INFO *) {
    RfcCallScope _tc("cursor_fetch");
    try {
        std::string handle = GetString(funcHandle, "IV_HANDLE");
        std::string prows  = GetString(funcHandle, "IV_PAGE_ROWS");
        long long page_rows = 0;
        if (!prows.empty()) { try { page_rows = std::stoll(prows); } catch (...) {} }
        if (page_rows <= 0) page_rows = 8192;   // default page = 4 x vector size

        CursorPage page = g_bridge->FetchCursor(handle, page_rows);

        SetXString(funcHandle, "EV_XDATA",   page.bxml);
        SetString (funcHandle, "EV_FETCHED", std::to_string(page.fetched));
        SetString (funcHandle, "EV_DONE",    page.done ? "X" : "");
        SetString (funcHandle, "EV_ERROR",   "");
        log::get().Debug("rfc", "Z_DUCKDB_FETCH ok",
                         {{"handle", handle},
                          {"fetched", (long long)page.fetched},
                          {"bytes", (long long)page.bxml.size()},
                          {"done", page.done ? "true" : "false", false}});
    } catch (const std::exception &e) {
        _tc.fail("cursor_error");
        log::get().Error("rfc", "Z_DUCKDB_FETCH failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_ERROR", e.what());
    }
    return RFC_OK;
}

extern "C" RFC_RC SAP_API ZCloseImpl(RFC_CONNECTION_HANDLE,
                                     RFC_FUNCTION_HANDLE funcHandle,
                                     RFC_ERROR_INFO *) {
    RfcCallScope _tc("cursor_close");
    try {
        std::string handle = GetString(funcHandle, "IV_HANDLE");
        g_bridge->CloseCursor(handle);
        SetString(funcHandle, "EV_ERROR", "");
        log::get().Debug("rfc", "Z_DUCKDB_CLOSE ok", {{"handle", handle}});
    } catch (const std::exception &e) {
        _tc.fail("cursor_error");
        log::get().Error("rfc", "Z_DUCKDB_CLOSE failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_ERROR", e.what());
    }
    return RFC_OK;
}
