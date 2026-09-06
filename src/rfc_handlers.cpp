#include "rfc_handlers.hpp"
#include "cycle.hpp"
#include "drift.hpp"
#include "load_type.hpp"
#include "metrics.hpp"
#include "publish.hpp"
#include "tick_planner.hpp"
#include "rfc_metadata.hpp"
#include "sap_uc.hpp"
#include "duckdb_bridge.hpp"
#include "cdc_dialect.hpp"
#include <map>
#include <set>
#include "cdc_status.hpp"
#include "split_planner.hpp"
#include "validation.hpp"
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
// A SQL string literal, quotes doubled. The target and the watermark reach here
// from an operator's command line.
std::string SqlLit(const std::string &v) {
    std::string q = "'";
    for (char c : v) { if (c == '\'') q += "''"; else q += c; }
    return q + "'";
}

// Run a statement, or say which one failed. A silent statement inside a
// transaction is how a partial write gets reported as success.
void Exec(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    if (r->HasError()) throw std::runtime_error(r->GetError());
}

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
std::string LowerOf(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string UpperOf(std::string s) {
    for (char &c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}
CdcMode CdcModeOf(const std::string &m) {
    const auto u = UpperOf(m);
    // FULL_IUD is the pre-rename spelling. It stays accepted permanently: it is a
    // stored value on every system provisioned before the rename, and a mode the
    // dialect does not recognise would silently fall back to DELETE_ONLY -- i.e.
    // stop capturing inserts and updates.
    if (u == "IMAGE_IUD" || u == "FULL_IUD") return CdcMode::ImageIud;
    if (u == "KEYS_IUD") return CdcMode::KeysIud;
    return CdcMode::DeleteOnly;
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

extern "C" RFC_RC SAP_API ZPlanImpl(RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE,
                                    RFC_ERROR_INFO *);

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
    if (RfcInstallServerFunction(nullptr, BuildPlanDesc(), ZPlanImpl, &info) != RFC_OK)
        throw_rfc("RfcInstallServerFunction(Z_DUCKDB_PLAN)", info);

    log::get().Debug("rfc", "handlers installed",
                     {{"functions", "STFC_CONNECTION,Z_DUCKDB_QUERY,Z_DUCKDB_INGEST,"
                                    "Z_DUCKDB_SNAPSHOT_MERGE,Z_DUCKDB_CDC_PLAN,Z_DUCKDB_CDC_APPLY,"
                                    "Z_DUCKDB_OPEN,Z_DUCKDB_FETCH,Z_DUCKDB_CLOSE,Z_DUCKDB_PLAN", true},
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

bool StartMetricsServer(int port, std::string &error) {
    if (!g_bridge) { error = "no database open"; return false; }
    return metrics::server::Start(*g_bridge, port, error);
}

void StopMetricsServer() { metrics::server::Stop(); }

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
        // IMAGE_IUD logs the full row image: take the column set from the (seeded)
        // DuckDB target and upper-case it to the SAP/HANA column names the triggers
        // reference (replicate lower-cases on the way in).
        if (spec.mode == CdcMode::ImageIud) {
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
        // The executor needs these to run a KEYS_IUD cycle: which mode it is in,
        // what to re-read from, and which keys need re-reading.
        js += ",\"mode\":"           + json::QuoteString(md);
        js += ",\"source\":"         + json::QuoteString(src);
        js += ",\"netkeys_sql\":"    + json::QuoteString(plan.netkeys_sql);
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
        // Empty unless this is a KEYS_IUD cycle, whose shadow log carries keys
        // only and whose row values come from a re-read of the source.
        std::string images  = GetString(funcHandle, "IV_IMAGES");
        log::get().Info("rfc", "Z_DUCKDB_CDC_APPLY",
                        {{"target", target}, {"staging", staging}, {"keys", keys},
                         {"images", images}});
        CdcApplyResult r = g_bridge->CdcApply(target, staging, SplitCsv(keys), images);
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



// A scalar out of a flat JSON object. IV_PARAMS is written by our own ABAP, so
// this only has to handle what we send: quoted strings and bare numbers. It is
// deliberately not a general parser -- the JSON here is an internal wire format,
// not user input.
std::string JsonField(const std::string &json, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    auto at = json.find(needle);
    if (at == std::string::npos) return {};
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    ++at;
    while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at]))) ++at;
    if (at >= json.size()) return {};
    if (json[at] == '"') {
        std::string out;
        for (++at; at < json.size() && json[at] != '"'; ++at) {
            if (json[at] == '\\' && at + 1 < json.size()) ++at;
            out += json[at];
        }
        return out;
    }
    std::string out;
    while (at < json.size() && json[at] != ',' && json[at] != '}' &&
           !std::isspace(static_cast<unsigned char>(json[at])))
        out += json[at++];
    return out;
}

// The ARRAY token for a key, brackets included, or empty.
//
// JsonField stops at the first ',' or '}' for a non-string value, so asking it
// for an array returns the first fragment of one. That is the third time in this
// codebase a scalar extractor has been pointed at a structured value -- the ABAP
// side needed the same split, and the command queue's result reader had it too.
// Different language, same shape: the scalar version is always the one already
// in reach.
std::string JsonArray(const std::string &json, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    auto at = json.find(needle);
    if (at == std::string::npos) return {};
    at = json.find('[', at + needle.size());
    if (at == std::string::npos) return {};
    int depth = 0;
    for (auto i = at; i < json.size(); ++i) {
        if (json[i] == '[') ++depth;
        else if (json[i] == ']' && --depth == 0) return json.substr(at, i - at + 1);
    }
    return {};
}

// The tick plan, as JSON. Reads the three control tables the planner needs and
// hands the pure function its rows, so the daemon and the batch tick get the
// same answer from the same code.
std::string PlanTickJson(duckdb::Connection &con) {
    std::vector<plan::TargetRow> targets;
    auto tr = con.Query(
        "SELECT target, method, cadence, status, coalesce(load_type_default,'D'), "
        "coalesce(one_shot_spent,false) AS one_shot_spent, "
        "coalesce(epoch(last_run_ts),0), coalesce(epoch(lease_ts),0), "
        "coalesce(epoch(parked_until),0), coalesce(fail_count,0), "
        "coalesce(max_cycle_secs,3600), coalesce(rows_applied,0) "
        "FROM _erpl_rev_delta_state");
    if (tr->HasError()) throw std::runtime_error("plan: state read failed: " + tr->GetError());
    for (size_t i = 0; i < tr->RowCount(); ++i) {
        plan::TargetRow t;
        t.target = tr->GetValue(0, i).ToString();
        t.method = tr->GetValue(1, i).ToString();
        t.cadence = tr->GetValue(2, i).ToString();
        t.status = tr->GetValue(3, i).ToString();
        t.load_type_default = tr->GetValue(4, i).ToString();
        t.one_shot_spent = tr->GetValue(5, i).GetValue<bool>();
        t.last_run_epoch = tr->GetValue(6, i).GetValue<double>();
        t.lease_epoch = tr->GetValue(7, i).GetValue<double>();
        t.parked_until_epoch = tr->GetValue(8, i).GetValue<double>();
        t.fail_count = static_cast<int>(tr->GetValue(9, i).GetValue<int64_t>());
        t.max_cycle_secs = static_cast<int>(tr->GetValue(10, i).GetValue<int64_t>());
        t.last_rows = tr->GetValue(11, i).GetValue<int64_t>();
        targets.push_back(t);
    }

    std::vector<plan::CdcRow> cdc;
    auto cr = con.Query("SELECT target, status, coalesce(shadow_rows,0) FROM _erpl_rev_cdc");
    if (!cr->HasError())
        for (size_t i = 0; i < cr->RowCount(); ++i) {
            plan::CdcRow c;
            c.target = cr->GetValue(0, i).ToString();
            c.status = cr->GetValue(1, i).ToString();
            c.shadow_rows = cr->GetValue(2, i).GetValue<int64_t>();
            cdc.push_back(c);
        }

    plan::DaemonRow d;
    auto dr = con.Query("SELECT coalesce(tick_secs,2), coalesce(max_workers,2), "
                        "coalesce(full_load_share,0.5), coalesce(stop,false) "
                        "FROM _erpl_rev_daemon LIMIT 1");
    if (!dr->HasError() && dr->RowCount() > 0) {
        d.tick_secs = static_cast<int>(dr->GetValue(0, 0).GetValue<int64_t>());
        d.max_workers = static_cast<int>(dr->GetValue(1, 0).GetValue<int64_t>());
        d.full_load_share = dr->GetValue(2, 0).GetValue<double>();
        d.stop = dr->GetValue(3, 0).GetValue<bool>();
    }

    const auto p = plan::PlanTick(targets, cdc, d, static_cast<double>(std::time(nullptr)));
    std::string out = "{\"stop\":" + std::string(p.stop ? "true" : "false") +
                      ",\"sleep_secs\":" + std::to_string(p.sleep_secs) + ",\"cycles\":[";
    for (size_t i = 0; i < p.cycles.size(); ++i) {
        if (i) out += ",";
        out += "{\"target\":" + json::QuoteString(p.cycles[i].target) +
               ",\"method\":" + json::QuoteString(p.cycles[i].method) +
               ",\"load_type\":" + json::QuoteString(p.cycles[i].load_type) +
               ",\"worker\":" + (p.cycles[i].worker ? "true" : "false") + "}";
    }
    return out + "]}";
}

// --- Planning ---------------------------------------------------------------
// One FM, many actions, because the alternative is a new stub (and a new upgrade
// event on every installed system) per decision the server needs to make.
// Everything travels as JSON in IV_PARAMS / EV_PLAN, so the signature never
// changes again.
//
// BEGIN_CYCLE and CYCLE_COMMIT are the cycle contract: the server decides the
// read bounds and owns the commit, ABAP reads and stages. Note the reply carries
// VALUES -- a floor, a ceiling, a column name -- never SQL text, so the driver's
// "parameters are only ever read as values" posture holds here too.
extern "C" RFC_RC SAP_API ZPlanImpl(RFC_CONNECTION_HANDLE,
                                    RFC_FUNCTION_HANDLE funcHandle,
                                    RFC_ERROR_INFO *) {
    RfcCallScope _tc("plan");
    try {
        const std::string action = UpperOf(GetString(funcHandle, "IV_ACTION"));
        const std::string target = GetString(funcHandle, "IV_TARGET");
        const std::string params = GetString(funcHandle, "IV_PARAMS");
        log::get().Info("rfc", "Z_DUCKDB_PLAN",
                        {{"action", action}, {"target", target}});

        auto con = g_bridge->Connect();
        std::string plan;

        if (action == "BEGIN_CYCLE") {
            const auto lt = ParseLoadType(JsonField(params, "load_type").empty()
                                              ? "D" : JsonField(params, "load_type"));
            const auto b = cycle::Begin(con, target, lt,
                                        static_cast<int64_t>(std::time(nullptr)),
                                        JsonField(params, "sap_now"));
            plan = std::string("{") +
                   "\"run_id\":" + std::to_string(b.run_id) + "," +
                   "\"stage\":" + json::QuoteString(b.stage_table) + "," +
                   "\"source_from\":" + json::QuoteString(b.source_from) + "," +
                   "\"keys\":" + json::QuoteString(b.keys) + "," +
                   "\"chg_col\":" + json::QuoteString(b.chg_col) + "," +
                   "\"time_col\":" + json::QuoteString(b.time_col) + "," +
                   "\"has_floor\":" + (b.bounds.has_floor ? "true" : "false") + "," +
                   "\"floor\":" + json::QuoteString(b.bounds.floor) + "," +
                   "\"has_ceiling\":" + (b.bounds.has_ceiling ? "true" : "false") + "," +
                   "\"ceiling_bounds_read\":" +
                       (b.bounds.ceiling_bounds_read ? "true" : "false") + "," +
                   "\"ceiling\":" + json::QuoteString(b.bounds.ceiling) + "," +
                   "\"as_of_date\":" + json::QuoteString(b.bounds.as_of_date) + "," +
                   "\"read_rows\":" + (b.plan.read_rows ? "true" : "false") + "," +
                   "\"truncate\":" + (b.plan.truncate_target ? "true" : "false") +
                   "}";
        } else if (action == "CYCLE_COMMIT") {
            cycle::CommitCounts c;
            const auto rr = JsonField(params, "rows_read");
            if (!rr.empty()) c.rows_read = std::atoll(rr.c_str());
            const auto r = cycle::Commit(con, target,
                                         std::atoll(JsonField(params, "run_id").c_str()), c);
            plan = std::string("{") +
                   "\"ins\":" + std::to_string(r.ins) + "," +
                   "\"upd\":" + std::to_string(r.upd) + "," +
                   "\"del\":" + std::to_string(r.del) + "," +
                   "\"logged\":" + std::to_string(r.logged) + "," +
                   "\"wm\":" + json::QuoteString(r.new_watermark) +
                   "}";
        } else if (action == "CDC_APPLY") {
            // The KEYS_IUD apply. It is an action rather than a parameter on
            // Z_DUCKDB_CDC_APPLY because that FM's interface cannot be extended
            // on a live system: the metadata updates, the generated include does
            // not, and the caller fails to compile against a parameter the
            // catalogue says exists.
            const auto staging = JsonField(params, "staging");
            const auto keys    = JsonField(params, "keys");
            const auto images  = JsonField(params, "images");
            CdcApplyResult r = g_bridge->CdcApply(target, staging, SplitCsv(keys), images);
            plan = std::string("{") +
                   "\"ins\":" + std::to_string(r.ins) + "," +
                   "\"upd\":" + std::to_string(r.upd) + "," +
                   "\"del\":" + std::to_string(r.del) + "," +
                   "\"prune\":" + std::to_string(r.prune_bound) + "," +
                   "\"applied\":" + (r.applied ? "true" : "false") +
                   "}";
        } else if (action == "DRIFT") {
            // The DDIC field list arrives as JSON on the first package of a
            // replication, so this costs no round trip of its own.
            drift::Schema ddic;
            {
                size_t at = 0;
                while ((at = params.find("{", at)) != std::string::npos) {
                    const auto end = params.find("}", at);
                    if (end == std::string::npos) break;
                    const auto obj = params.substr(at, end - at + 1);
                    drift::Field f;
                    f.name = JsonField(obj, "name");
                    f.datatype = JsonField(obj, "datatype");
                    const auto len = JsonField(obj, "length");
                    const auto dec = JsonField(obj, "decimals");
                    if (!len.empty()) f.length = std::atoi(len.c_str());
                    if (!dec.empty()) f.decimals = std::atoi(dec.c_str());
                    if (!f.name.empty()) ddic.push_back(f);
                    at = end + 1;
                }
            }

            drift::Schema have;
            {
                auto r = con.Query("SELECT column_name, data_type FROM duckdb_columns() "
                                   "WHERE lower(table_name)=lower('" + target + "')");
                if (!r->HasError())
                    for (size_t i = 0; i < r->RowCount(); ++i) {
                        drift::Field f;
                        f.name = r->GetValue(0, i).ToString();
                        // The target's DuckDB type is compared through the same
                        // mapping the DDIC side uses, so a difference means a real
                        // difference and not a spelling one.
                        f.datatype = r->GetValue(1, i).ToString();
                        have.push_back(f);
                    }
            }

            // A target that does not exist yet is not drift -- it is a first load.
            if (have.empty()) {
                plan = "{\"blocked\":false,\"new_target\":true}";
            } else {
                // Compare on names only when the target was built from these very
                // fields: DuckDB reports its own type names, so a type-level diff
                // here would flag every column on every run.
                drift::Schema ddic_names, have_names;
                for (const auto &f : ddic) ddic_names.push_back({f.name, "", 0, 0});
                for (const auto &f : have) have_names.push_back({f.name, "", 0, 0});
                const auto d = drift::Diff(ddic_names, have_names);

                std::string alters;
                for (const auto &f : d.added) {
                    for (const auto &orig : ddic)
                        if (orig.name == f.name) {
                            auto r = con.Query("ALTER TABLE " + target + " ADD COLUMN " +
                                               f.name + " " + drift::DuckType(orig));
                            if (!r->HasError()) alters += (alters.empty() ? "" : ",") +
                                                          json::QuoteString(f.name);
                        }
                }
                if (d.blocked)
                    con.Query("UPDATE _erpl_rev_delta_state SET status='BLOCKED' "
                              "WHERE target='" + target + "'");
                plan = std::string("{\"blocked\":") + (d.blocked ? "true" : "false") +
                       ",\"added\":[" + alters + "]" +
                       ",\"detail\":" + json::QuoteString(drift::Explain(d)) + "}";
            }
        } else if (action == "TICK") {
            plan = PlanTickJson(con);
        } else if (action == "SUBS") {
            // Subscriptions. The publish and the offset advance are one
            // transaction, and that transaction happens HERE -- a round trip
            // through SAP for an operation touching no SAP data could not be
            // atomic with it. The command queue carries the request; the work
            // is server-side.
            const auto op = JsonField(params, "op");
            if (op == "create") {
                CreateSubscription(con, JsonField(params, "name"), target,
                                   JsonField(params, "sink"));
                plan = "{\"created\":" + json::QuoteString(JsonField(params, "name")) + "}";
            } else if (op == "advance") {
                const auto r = Advance(con, JsonField(params, "name"));
                plan = "{\"published\":" + std::to_string(r.published) + ",\"offset\":" +
                       std::to_string(r.new_offset) + "}";
            } else if (op == "ls") {
                // Through the bridge, so the rows come back in the same JSON
                // shape every other read uses rather than a second rendering.
                QueryResult qr = g_bridge->Query(
                    "SELECT name, target, sink_spec, \"offset\", status "
                    "FROM _erpl_rev_subscription ORDER BY name");
                plan = "{\"subscriptions\":" + RowsToJsonArray(qr.rows) + "}";
            } else {
                throw std::runtime_error("SUBS: unknown op '" + op + "'. Known: create, "
                                         "advance, ls.");
            }
        } else if (action == "CDC_PROBE") {
            // What to ASK the database. The server owns the dialect, so the
            // probe SQL and the expected object list come from one place; ABAP
            // runs the three statements and posts the names back to CDC_STATUS.
            // Two round trips rather than one because the catalogue lives in
            // HANA and the registry lives here, and neither can see the other.
            // The registration, read the same way CDC_PLAN reads it.
            CdcState cst = g_bridge->CdcGet(target);
            if (cst.source.empty() || cst.keys.empty())
                throw std::runtime_error("CDC_PROBE: " + target + " is not a registered "
                                         "trigger target");
            CdcSpec spec;
            spec.source = cst.source;
            spec.keys = SplitCsv(cst.keys);
            spec.mode = CdcModeOf(cst.mode.empty() ? "DELETE_ONLY" : cst.mode);
            if (spec.mode == CdcMode::ImageIud) {
                QueryResult tc = g_bridge->Query("SELECT * FROM " + target + " LIMIT 0");
                for (auto &c : tc.columns) spec.columns.push_back(UpperOf(c.name));
            }
            auto dia_p = MakeDialect(cst.platform.empty() ? "HANA" : cst.platform);
            const CdcDialect &dia = *dia_p;
            const auto plan_ddl = dia.Plan(spec);
            std::string trigs;
            for (size_t i = 0; i < plan_ddl.trigger_names.size(); ++i) {
                if (i) trigs += ",";
                trigs += json::QuoteString(plan_ddl.trigger_names[i]);
            }
            plan = "{\"tables_sql\":" + json::QuoteString(dia.ProbeTablesSql()) +
                   ",\"sequences_sql\":" + json::QuoteString(dia.ProbeSequencesSql()) +
                   ",\"triggers_sql\":" + json::QuoteString(dia.ProbeTriggersSql()) +
                   ",\"log_table\":" + json::QuoteString(plan_ddl.log_table) +
                   ",\"seq_name\":" + json::QuoteString(plan_ddl.seq_name) +
                   ",\"triggers\":[" + trigs + "]}";
        } else if (action == "CDC_STATUS") {
            // The verdict, from what the database actually reported. Status is
            // DERIVED here, never a stored enum trusted on its own: a trigger
            // dropped out of band by a system copy, a transport or a DBA is
            // invisible to the registry and silently captures nothing.
            auto names = [&](const char *key) {
                std::vector<std::string> out;
                const auto arr = JsonField(params, key);
                size_t at = 0;
                while (at < arr.size()) {
                    const auto b = arr.find('"', at);
                    if (b == std::string::npos) break;
                    const auto e = arr.find('"', b + 1);
                    if (e == std::string::npos) break;
                    out.push_back(arr.substr(b + 1, e - b - 1));
                    at = e + 1;
                }
                return out;
            };
            cdc::Probe have;
            have.tables = names("tables");
            have.sequences = names("sequences");
            have.enabled_triggers = names("enabled_triggers");
            have.disabled_triggers = names("disabled_triggers");

            cdc::Expected want;
            want.log_table = JsonField(params, "log_table");
            want.seq_name = JsonField(params, "seq_name");
            want.triggers = names("triggers");

            CdcState cs = g_bridge->CdcGet(target);
            const bool deactivated = cs.status == "DISABLED";
            const auto res = cdc::Derive(want, have, deactivated);
            const auto name = cdc::StatusName(res.status);

            // Persisted, so the tick planner and the operator see the same
            // answer. An INCONSISTENT trigger set must not keep cycling: the
            // position would advance past changes that were never captured.
            g_bridge->CdcSetStatus(target, name);

            std::string miss;
            for (size_t i = 0; i < res.missing.size(); ++i) {
                if (i) miss += ",";
                miss += json::QuoteString(res.missing[i]);
            }
            plan = "{\"status\":" + json::QuoteString(name) + ",\"explain\":" +
                   json::QuoteString(cdc::Explain(res)) + ",\"missing\":[" + miss + "]}";
        } else if (action == "CDC_REPAIR") {
            // ONLY the missing objects. Re-running the whole provision DDL
            // would recreate the shadow table and reset the position,
            // discarding every change captured since -- a "repair" that loses
            // exactly the data it was run to protect.
            CdcState cst = g_bridge->CdcGet(target);
            if (cst.source.empty() || cst.keys.empty())
                throw std::runtime_error("CDC_REPAIR: " + target + " is not a registered "
                                         "trigger target");
            CdcSpec spec;
            spec.source = cst.source;
            spec.keys = SplitCsv(cst.keys);
            spec.mode = CdcModeOf(cst.mode.empty() ? "DELETE_ONLY" : cst.mode);
            if (spec.mode == CdcMode::ImageIud) {
                QueryResult tc = g_bridge->Query("SELECT * FROM " + target + " LIMIT 0");
                for (auto &c : tc.columns) spec.columns.push_back(UpperOf(c.name));
            }
            auto dia_r = MakeDialect(cst.platform.empty() ? "HANA" : cst.platform);
            const auto full = dia_r->Plan(spec);

            auto names2 = [&](const char *key) {
                std::vector<std::string> out;
                const auto arr = JsonField(params, key);
                size_t at = 0;
                while (at < arr.size()) {
                    const auto b = arr.find('"', at);
                    if (b == std::string::npos) break;
                    const auto e = arr.find('"', b + 1);
                    if (e == std::string::npos) break;
                    out.push_back(arr.substr(b + 1, e - b - 1));
                    at = e + 1;
                }
                return out;
            };
            cdc::Probe have;
            have.tables = names2("tables");
            have.sequences = names2("sequences");
            have.enabled_triggers = names2("enabled_triggers");
            have.disabled_triggers = names2("disabled_triggers");
            cdc::Expected want;
            want.log_table = full.log_table;
            want.seq_name = full.seq_name;
            want.triggers = full.trigger_names;

            const auto st_r = cdc::Derive(want, have, false);
            const auto ddl = cdc::RepairPlan(st_r, full.provision_ddl);
            std::string arr;
            for (size_t i = 0; i < ddl.size(); ++i) {
                if (i) arr += ",";
                arr += json::QuoteString(ddl[i]);
            }
            plan = "{\"ddl\":[" + arr + "],\"count\":" + std::to_string(ddl.size()) + "}";
        } else if (action == "SPLIT") {
            // Cut a mass load into portions, and PERSIST them before any worker
            // starts -- that is what makes the run restartable. ABAP supplies
            // the facts it alone has (the histogram, the fiscal ranges, the
            // bytes per row); the server decides every boundary, so one code
            // path cuts every strategy.
            split::SplitRequest req;
            const auto strat = JsonField(params, "strategy");
            // The strategy names the operator types, mapped here rather than in
            // ABAP: the server decides every boundary, so it also owns what the
            // strategy words mean.
            const std::string sname = strat.empty() ? "records" : LowerOf(strat);
            if (sname == "records")      req.strategy = split::Strategy::Records;
            else if (sname == "size")    req.strategy = split::Strategy::Size;
            else if (sname == "time")    req.strategy = split::Strategy::Time;
            else if (sname == "fiscal")  req.strategy = split::Strategy::Fiscal;
            else if (sname == "list")    req.strategy = split::Strategy::List;
            else if (sname == "key")     req.strategy = split::Strategy::Key;
            else throw std::runtime_error("SPLIT: unknown strategy '" + strat +
                                          "'. Known: records, size, time, fiscal, list, key.");
            req.part_col = JsonField(params, "part_col");
            req.user_where = JsonField(params, "where");
            const auto lr = JsonField(params, "limit_rows");
            const auto lm = JsonField(params, "limit_mb");
            const auto bpr = JsonField(params, "bytes_per_row");
            if (!lr.empty()) req.limit_rows = std::atoll(lr.c_str());
            if (!lm.empty()) req.limit_mb = std::atoll(lm.c_str());
            if (!bpr.empty()) req.bytes_per_row = std::atoll(bpr.c_str());
            req.range_min = JsonField(params, "range_min");
            req.range_max = JsonField(params, "range_max");
            const auto tot = JsonField(params, "total_rows");
            if (!tot.empty()) req.total_rows = std::atoll(tot.c_str());
            req.time_unit = JsonField(params, "time_unit");
            req.time_from = JsonField(params, "time_from");
            req.time_to = JsonField(params, "time_to");
            {
                // The histogram: {"bucket":"...","rows":N} objects.
                size_t at = 0;
                const auto h = JsonArray(params, "histogram");
                while ((at = h.find("{", at)) != std::string::npos) {
                    const auto end = h.find("}", at);
                    if (end == std::string::npos) break;
                    const auto obj = h.substr(at, end - at + 1);
                    split::Bucket b;
                    b.value = JsonField(obj, "bucket");
                    const auto n = JsonField(obj, "rows");
                    b.rows = n.empty() ? 0 : std::atoll(n.c_str());
                    if (!b.value.empty()) req.histogram.push_back(b);
                    at = end + 1;
                }
            }
            const auto portions = split::PlanSplit(req);
            // Say WHY, not just "none". A split that produces nothing is always
            // a missing input, and which one is the entire question.
            if (portions.empty()) {
                std::string why = "SPLIT produced no portions for " + target + " (strategy " +
                                  sname + ")";
                if ((req.strategy == split::Strategy::Records ||
                     req.strategy == split::Strategy::Size) && req.histogram.empty() &&
                    req.range_min.empty())
                    why += ": no histogram and no partition bounds -- pass --part-col";
                else if (req.strategy == split::Strategy::Records && req.limit_rows <= 0)
                    why += ": --limit-rows is required";
                else if (req.strategy == split::Strategy::Size && req.limit_mb <= 0)
                    why += ": --limit-mb is required";
                else if (req.strategy == split::Strategy::Time)
                    why += ": a time range is required";
                throw std::runtime_error(why);
            }

            // Persisted first, in one transaction: a portion list that exists
            // only in the caller's memory cannot be restarted after a crash,
            // which is the whole reason to split rather than stream.
            Exec(con, "BEGIN");
            try {
                Exec(con, "CREATE TABLE IF NOT EXISTS _erpl_rev_portion ("
                          "target VARCHAR, run_id BIGINT, portion_no INTEGER, "
                          "predicate VARCHAR, est_rows BIGINT, status VARCHAR DEFAULT 'PENDING', "
                          "attempts INTEGER DEFAULT 0, started_ts TIMESTAMPTZ, "
                          "finished_ts TIMESTAMPTZ, rows_done BIGINT)");
                Exec(con, "DELETE FROM _erpl_rev_portion WHERE target=" + SqlLit(target) +
                              " AND status <> 'DONE'");
                for (const auto &p : portions)
                    Exec(con, "INSERT INTO _erpl_rev_portion "
                              "(target, run_id, portion_no, predicate, est_rows, status) VALUES (" +
                              SqlLit(target) + ",0," + std::to_string(p.portion_no) + "," +
                              SqlLit(p.predicate) + "," + std::to_string(p.est_rows) +
                              ",'PENDING')");
                Exec(con, "COMMIT");
            } catch (...) {
                Exec(con, "ROLLBACK");
                throw;
            }

            std::string arr;
            for (size_t i = 0; i < portions.size(); ++i) {
                if (i) arr += ",";
                arr += "{\"portion_no\":" + std::to_string(portions[i].portion_no) +
                       ",\"predicate\":" + json::QuoteString(portions[i].predicate) +
                       ",\"est_rows\":" + std::to_string(portions[i].est_rows) + "}";
            }
            // A flat predicate array alongside the detailed one: the driver
            // needs the predicates in order and nothing else, and parsing
            // objects out of a nested array in ABAP is how the repair DDL got
            // mangled twice.
            std::string preds;
            for (size_t i = 0; i < portions.size(); ++i) {
                if (i) preds += ",";
                preds += json::QuoteString(portions[i].predicate);
            }
            plan = "{\"portions\":[" + arr + "],\"predicates\":[" + preds +
                   "],\"count\":" + std::to_string(portions.size()) + "}";
        } else if (action == "VALIDATE") {
            // The DuckDB half of a two-sided comparison. ABAP renders the SAP
            // side with fingerprint_cell and sends its per-key fingerprints;
            // here the same rows are rendered from the replica with the
            // matching expressions, and the two are compared.
            //
            // Canonical TEXT per column, not a row count: a replica that is the
            // right size and the wrong content passes every count check there
            // is. FLTP is excluded because binary floating point does not
            // round-trip through decimal text and would report mismatches on
            // correct data.
            validation::Policy pol;
            const auto mode = JsonField(params, "mode");
            pol.mode = mode == "full" ? validation::Mode::Full : validation::Mode::Sample;
            const auto sr = JsonField(params, "sample_rows");
            if (!sr.empty()) pol.sample_rows = std::atoll(sr.c_str());

            std::vector<validation::Field> fields;
            {
                const auto arr = JsonArray(params, "fields");
                size_t at = 0;
                while ((at = arr.find("{", at)) != std::string::npos) {
                    const auto end = arr.find("}", at);
                    if (end == std::string::npos) break;
                    const auto obj = arr.substr(at, end - at + 1);
                    validation::Field f;
                    f.name = JsonField(obj, "name");
                    f.datatype = JsonField(obj, "datatype");
                    const auto len = JsonField(obj, "length");
                    const auto dec = JsonField(obj, "decimals");
                    if (!len.empty()) f.length = std::atoi(len.c_str());
                    if (!dec.empty()) f.decimals = std::atoi(dec.c_str());
                    if (!f.name.empty() && validation::IsComparable(f)) fields.push_back(f);
                    at = end + 1;
                }
            }
            if (fields.empty())
                throw std::runtime_error("VALIDATE: no comparable columns for " + target);

            // The registered keys, so both sides pair rows by identity.
            std::vector<std::string> vkeys;
            {
                auto kr = con.Query("SELECT coalesce(keys,'') FROM _erpl_rev_delta_state "
                                    "WHERE target=" + SqlLit(target));
                if (!kr->HasError() && kr->RowCount() > 0)
                    vkeys = SplitCsv(kr->GetValue(0, 0).ToString());
            }
            const auto vplan = validation::BuildPlan(pol, target, fields, vkeys);
            QueryResult ours = g_bridge->Query(vplan.sql);

            // Paired by KEY, not by position.
            //
            // Positional comparison assumed two engines given the same ORDER BY
            // produce the same sequence. They need not -- collation, NULL
            // placement and numeric-versus-text ordering all differ between
            // HANA and DuckDB -- and a single misalignment reports every row
            // after it as wrong, or worse, lines two genuinely different sets
            // up index for index and reports PASSED.
            std::map<std::string, std::string> mine;
            for (const auto &row : ours.rows)
                mine[JsonField(row, "k")] = JsonField(row, "fp");

            long long compared = 0, mismatched = 0;
            std::string first_bad;
            std::set<std::string> seen;
            {
                const auto arr = JsonArray(params, "rows");
                size_t at = 0;
                while ((at = arr.find("{", at)) != std::string::npos) {
                    const auto end2 = arr.find("}", at);
                    if (end2 == std::string::npos) break;
                    const auto obj = arr.substr(at, end2 - at + 1);
                    at = end2 + 1;
                    const auto k = JsonField(obj, "k");
                    if (k.empty()) continue;
                    ++compared;
                    seen.insert(k);
                    const auto it = mine.find(k);
                    if (it == mine.end()) {
                        // In SAP, absent from the replica: a lost row, which is
                        // the failure this whole product exists to prevent.
                        ++mismatched;
                        if (first_bad.empty()) first_bad = k + " (missing from the replica)";
                    } else if (it->second != JsonField(obj, "fp")) {
                        ++mismatched;
                        if (first_bad.empty()) first_bad = k;
                    }
                }
            }
            // ...and the other direction. A row in the replica that the source
            // does not have is just as wrong, and counting only one direction
            // calls a replica with extra rows identical.
            if (pol.mode == validation::Mode::Full) {
                for (const auto &kv : mine) {
                    if (seen.count(kv.first)) continue;
                    ++mismatched;
                    if (first_bad.empty())
                        first_bad = kv.first + " (in the replica, not at the source)";
                }
            }

            // Recorded like any other run: a validation nobody can find later
            // did not happen as far as an auditor is concerned.
            Exec(con, "INSERT INTO _erpl_rev_run_stats "
                      "(run_id, target, source, run_type, method, status, rows_read, "
                      "validation_status) VALUES (nextval('_erpl_rev_run_seq')," +
                      SqlLit(target) + "," + SqlLit(target) + ",'VALIDATE','OPERATOR'," +
                      SqlLit(mismatched == 0 ? "SUCCESS" : "ERROR") + "," +
                      std::to_string(compared) + "," +
                      SqlLit(mismatched == 0 ? "PASSED" : "FAILED") + ")");

            plan = "{\"compared\":" + std::to_string(compared) + ",\"mismatched\":" +
                   std::to_string(mismatched) + ",\"verdict\":" +
                   json::QuoteString(mismatched == 0 ? "PASSED" : "FAILED") +
                   ",\"first_mismatch\":" + json::QuoteString(first_bad) + "}";
        } else if (action == "UNPARK") {
            // Parking is what stops a broken target hammering SAP; without a way
            // back it is a one-way door that only raw SQL can open. Clearing the
            // failure count too, because a target the operator has just fixed
            // should not resume at the far end of an exponential backoff.
            auto up = con.Query("UPDATE _erpl_rev_delta_state SET parked_until=NULL, "
                                "park_reason=NULL, fail_count=0, "
                                "status=CASE WHEN coalesce(status,'')='BLOCKED' THEN 'IDLE' "
                                "ELSE status END WHERE target=" + SqlLit(target));
            if (up->HasError())
                throw std::runtime_error("UNPARK failed: " + up->GetError());
            const auto n = up->RowCount() > 0 && !up->GetValue(0, 0).IsNull()
                               ? up->GetValue(0, 0).GetValue<int64_t>()
                               : 0;
            if (n == 0)
                throw std::runtime_error("UNPARK: no delta registration for " + target);
            plan = "{\"unparked\":" + json::QuoteString(target) + "}";
        } else if (action == "SET_WM") {
            // An operator moving the position, deliberately -- to re-deliver a
            // window after a downstream loss, or to adopt a position after a
            // restore. It writes engine state on purpose, so it records a run of
            // its own: a watermark that moved with no record of who moved it is
            // the hardest kind of replication question to answer afterwards.
            const auto to = JsonField(params, "wm_value");
            if (to.empty()) throw std::runtime_error("SET_WM: wm_value is required");
            auto before = con.Query("SELECT coalesce(wm_value,'') FROM _erpl_rev_delta_state "
                                    "WHERE target=" + SqlLit(target));
            if (before->HasError() || before->RowCount() == 0)
                throw std::runtime_error("SET_WM: no delta registration for " + target);
            const auto from = before->GetValue(0, 0).ToString();

            Exec(con, "BEGIN");
            try {
                Exec(con, "UPDATE _erpl_rev_delta_state SET wm_value=" + SqlLit(to) +
                              " WHERE target=" + SqlLit(target));
                Exec(con, "INSERT INTO _erpl_rev_run_stats "
                          "(run_id, target, source, run_type, method, status, wm_from, wm_to) "
                          "VALUES (nextval('_erpl_rev_run_seq')," + SqlLit(target) + "," +
                          SqlLit(target) + ",'SET_WM','OPERATOR','SUCCESS'," + SqlLit(from) +
                          "," + SqlLit(to) + ")");
                Exec(con, "COMMIT");
            } catch (...) {
                Exec(con, "ROLLBACK");
                throw;
            }
            plan = "{\"wm_from\":" + json::QuoteString(from) + ",\"wm_to\":" +
                   json::QuoteString(to) + "}";
        } else if (action == "PREVIEW") {
            // The first rows of a target, read through the SAME path a
            // subscriber reads -- the transform view when one is registered, the
            // table otherwise. Reading the table directly here would let an
            // operator approve output that is not what gets published.
            const auto n = JsonField(params, "rows");
            const long long rows = n.empty() ? 20 : std::atoll(n.c_str());
            auto xf = con.Query("SELECT coalesce(xform_view,'') FROM _erpl_rev_delta_state "
                                "WHERE target=" + SqlLit(target));
            std::string from_rel = target;
            if (!xf->HasError() && xf->RowCount() > 0) {
                const auto v = xf->GetValue(0, 0).ToString();
                if (!v.empty()) from_rel = v;
            }
            QueryResult qr = g_bridge->Query("SELECT * FROM " + from_rel + " LIMIT " +
                                             std::to_string(rows));
            plan = "{\"rows\":" + RowsToJsonArray(qr.rows) + "}";
        } else if (action == "RETAIN") {
            // Prune the change log to the slowest subscriber, or to the window
            // when nothing is subscribed.
            const auto w = JsonField(params, "window_secs");
            const long long window = w.empty() ? 0 : std::atoll(w.c_str());
            const long long pruned = Retain(con, target, window);
            plan = "{\"pruned\":" + std::to_string(pruned) + "}";
        } else {
            throw std::runtime_error(
                "unknown plan action '" + action +
                "'. Known: BEGIN_CYCLE, CYCLE_COMMIT, TICK, CDC_APPLY, DRIFT, SUBS, "
                "RETAIN, SET_WM, PREVIEW, CDC_PROBE, CDC_STATUS, CDC_REPAIR, SPLIT, VALIDATE, UNPARK.");
        }

        SetString(funcHandle, "EV_PLAN", plan);
        SetString(funcHandle, "EV_ERROR", "");
    } catch (const std::exception &e) {
        // Never let an exception cross the RFC boundary: ABAP gets a clean
        // EV_ERROR, which is the convention every other handler follows.
        _tc.fail("plan_error");
        log::get().Error("rfc", "Z_DUCKDB_PLAN failed", {{"error", e.what()}});
        SetString(funcHandle, "EV_PLAN", "");
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
