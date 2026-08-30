#include "duckdb_bridge.hpp"
#include "payload.hpp"
#include "json_util.hpp"
#include "sxml_binary.hpp"

#include <cstdlib>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "duckdb.hpp"

namespace erpl_rev {

namespace {

// SQL single-quote escaping for string literals.
std::string SqlQuote(const std::string &s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "''"; else out += c; }
    out += "'";
    return out;
}

// Render a parsed JSON cell as a SQL literal.
std::string CellToSql(const json::Cell &c) {
    if (c.is_null) return "NULL";
    if (c.is_string) return SqlQuote(c.value);
    return c.value;   // number/bool verbatim
}

// DuckDB folds unquoted identifiers to lower case; we lower-case BXML column
// names (which arrive upper-case from ABAP) to match the stored/target names.
std::string LowerName(const std::string &s) {
    std::string l = s;
    for (char &c : l) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return l;
}

// Sanitize a DuckDB column name into a valid ABAP component name so ABAP can
// build the result structure (cl_abap_structdescr) — else expression columns
// like count(*) ("count_star()") raise CX_SY_STRUCT_COMP_NAME. Uppercase; map
// any char outside [A-Z0-9_] to '_'; trim leading/trailing '_'; ensure a
// letter/underscore start; cap at 30 chars. The SAME sanitized name is used for
// both EV_COLUMNS and the BXML element, so they still bind. (Alias expression
// columns for prettier headers.)
std::string SanitizeColName(const std::string &s) {
    std::string u;
    for (char c : s) {
        char up = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
        u += ((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9') || up == '_')
                 ? up : '_';
    }
    size_t a = u.find_first_not_of('_');
    size_t b = u.find_last_not_of('_');
    u = (a == std::string::npos) ? std::string() : u.substr(a, b - a + 1);
    if (u.empty()) u = "COL";
    if (!((u[0] >= 'A' && u[0] <= 'Z') || u[0] == '_')) u = "C" + u;
    if (u.size() > 30) u = u.substr(0, 30);
    return u;
}

// One cell as raw text for BXML (NULL -> empty element). DuckDB Value::ToString
// gives canonical text (ISO dates/times, plain decimals) that asXML `id`
// deserializes back into the matching ABAP type.
std::string CellText(duckdb::DataChunk &chunk, duckdb::idx_t col, duckdb::idx_t row) {
    auto v = chunk.GetValue(col, row);
    return v.IsNull() ? std::string() : v.ToString();
}

// A single-quoted SQL string literal with embedded quotes doubled.
std::string SqlLit(const std::string &s) {
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "''"; r += c; }
    return r + "'";
}

// The trigger-CDC state-machine transition guard.
bool AllowedCdcTransition(const std::string &from, const std::string &to) {
    if (from == "PROVISIONED") return to == "SEEDED" || to == "DISABLED";
    if (from == "SEEDED")      return to == "ACTIVE" || to == "DISABLED";
    if (from == "ACTIVE")      return to == "ACTIVE" || to == "DISABLED";
    if (from == "DISABLED")    return to == "PROVISIONED";
    return false;
}

} // namespace

// A streaming cursor owns its OWN DuckDB connection (DuckDB permits only one
// active stream per connection) and the lazy result it pages through.
//
// Prefetch: after serving page N, a background task produces page N+1 into
// `pending` (one page ahead, double-buffered), so DuckDB fetch + BXML encode
// overlap the RFC round-trip and ABAP-side decode. At most two pages exist at
// once (one buffered + one building), so memory stays bounded. Only ONE thread
// ever touches `result` at a time: the foreground always .get()s `pending`
// (waiting for the background task) before touching `result` again, and the
// per-cursor `mtx` serialises concurrent FetchCursor calls.
//
// `pending` is declared LAST so it destructs FIRST: a std::async(launch::async)
// future blocks in its destructor until the task completes, guaranteeing the
// background task stops touching `result`/`con` before those are destroyed.
struct Cursor {
    std::unique_ptr<duckdb::Connection> con;
    std::unique_ptr<duckdb::QueryResult> result;
    std::vector<QueryColumn> columns;
    std::mutex mtx;          // serialises Fetch on this cursor
    bool done = false;
    long long page_rows = 0; // fixed at the first FetchCursor; prefetch reuses it
    std::future<CursorPage> pending;
};

namespace {

// Pull whole DataChunks (each <= STANDARD_VECTOR_SIZE) until `page_rows` is
// reached or the stream ends, and BXML-encode them. Touches only `cur` members
// and must be called single-threaded w.r.t. this cursor (see Cursor docs).
CursorPage ProducePage(Cursor &cur, long long page_rows) {
    CursorPage page;
    if (cur.done) { page.done = true; return page; }

    const duckdb::idx_t ncol = cur.columns.size();
    std::vector<std::string> colnames;
    colnames.reserve(ncol);
    for (auto &c : cur.columns) colnames.push_back(c.name);
    // Stream straight into BXML — no intermediate row-major string matrix. The
    // dominant case (every replicated SAP column is stored VARCHAR) reads the
    // string_t bytes flat, with zero Value allocation; BLOB / numeric / computed
    // columns keep the canonical Value::ToString() path so output is byte-identical.
    sxml::StreamEncoder enc(std::move(colnames));

    long long got = 0;
    while (page_rows <= 0 || got < page_rows) {
        auto chunk = cur.result->Fetch();
        if (cur.result->HasError())
            throw std::runtime_error("DuckDB query failed: " + cur.result->GetError());
        if (!chunk || chunk->size() == 0) { cur.done = true; break; }
        chunk->Flatten();   // guarantee FLAT vectors so string_t reads are direct
        const duckdb::idx_t n = chunk->size();
        for (duckdb::idx_t row = 0; row < n; row++) {
            enc.StartRow();
            for (duckdb::idx_t c = 0; c < ncol; c++) {
                auto &vec = chunk->data[c];
                if (vec.GetType().id() == duckdb::LogicalTypeId::VARCHAR) {
                    if (!duckdb::FlatVector::Validity(vec).RowIsValid(row)) {
                        enc.Cell(c, nullptr, 0);
                    } else {
                        auto &s = duckdb::FlatVector::GetData<duckdb::string_t>(vec)[row];
                        enc.Cell(c, s.GetData(), s.GetSize());
                    }
                } else {
                    std::string s = CellText(*chunk, c, row);
                    enc.Cell(c, s.data(), s.size());
                }
            }
            enc.EndRow();
            got++;
        }
    }
    page.fetched = got;
    page.done = cur.done;
    if (got > 0) page.bxml = enc.Finish();
    return page;
}

} // namespace

struct CursorStore {
    std::mutex mtx;
    std::unordered_map<std::string, std::shared_ptr<Cursor>> map;
    long long counter = 0;
    static constexpr size_t kMaxOpen = 64;
};

DuckDbBridge::DuckDbBridge(const std::string &path, const std::string &init_sql)
    : db_(std::make_unique<duckdb::DuckDB>(path.empty() ? nullptr : path.c_str())),
      cursors_(std::make_unique<CursorStore>()) {
    // GLOBAL engine config, inherited by every per-op connection. We never rely on
    // row insertion order (targets get a PRIMARY KEY; reads ORDER BY explicitly),
    // so preserve_insertion_order=false lowers memory and speeds large bulk ops.
    // Ops can tune further (memory_limit/threads/checkpoint_threshold/temp_directory)
    // via ERPL_REV_DUCKDB_INIT, e.g. "SET GLOBAL memory_limit='32GB'".
    duckdb::Connection con(*db_);
    auto r = con.Query("SET GLOBAL preserve_insertion_order=false");
    if (r->HasError())
        throw std::runtime_error("DuckDB config failed: " + r->GetError());

    // Remote (httpfs) reads of MANY files — e.g. a SQL-console read_parquet over a
    // long list like the 12/72-month NYC-taxi example — otherwise open a fresh TLS
    // connection per range request. With default httpfs_connection_caching=false
    // that is a handshake storm that, compounded by retries/CloudFront throttling,
    // hangs the server (and the synchronous SAP GUI) for minutes even though the
    // duckdb CLI runs the same query in seconds. Cache remote file metadata and
    // REUSE HTTP connections so multi-file remote reads stay fast.
    con.Query("SET GLOBAL enable_http_metadata_cache=true");   // core setting; offline-safe
    // httpfs_connection_caching only exists once httpfs is loaded. Load it from the
    // local extension cache (no INSTALL => no network, so an air-gapped boot just
    // skips this); a later remote query auto-loads it anyway.
    if (!con.Query("LOAD httpfs")->HasError())
        con.Query("SET GLOBAL httpfs_connection_caching=true");
    // Delta registry + runtime state — created once, always (independent of the
    // optional boot init_sql). Both the per-target config and the watermark/lease
    // state for incremental extraction live here; ABAP reads/writes it through
    // Z_DUCKDB_QUERY, so there is no new SAP-side state table. (HLD §4.1.)
    auto d = con.Query(
        "CREATE TABLE IF NOT EXISTS _erpl_rev_delta_state ("
        "target VARCHAR PRIMARY KEY, method VARCHAR NOT NULL, source_from VARCHAR NOT NULL, "
        "keys VARCHAR NOT NULL, chg_col VARCHAR, wm_kind VARCHAR, wm_value VARCHAR, "
        "safety_secs INTEGER DEFAULT 120, cadence VARCHAR DEFAULT 'nightly', extra VARCHAR, "
        // TIMESTAMPTZ (not naive TIMESTAMP): now() is tz-aware, so storing it in a
        // naive column and later doing epoch(now()) - epoch(col) yields a spurious
        // local-UTC offset. Matching types keeps cadence/lease arithmetic correct.
        "last_run_ts TIMESTAMPTZ, rows_applied BIGINT, status VARCHAR DEFAULT 'IDLE', "
        "lease_ts TIMESTAMPTZ, last_error VARCHAR)");
    if (d->HasError())
        throw std::runtime_error("DuckDB delta-state init failed: " + d->GetError());
    // Replication run statistics — one durable row per full or incremental run,
    // written by the ABAP apply path (zcl_erpl_rev_util=>record_run via Z_DUCKDB_QUERY),
    // with enough dimensions/measures to build a replication dashboard straight from
    // DuckDB (see docs/stats.md). run_id from a sequence; ts defaults to the server
    // clock (now()) so it agrees with the delta-state clock. The erpl_rev_run_stats
    // view adds derived rows_applied / rows_per_sec / started_at / is_success.
    auto seq = con.Query("CREATE SEQUENCE IF NOT EXISTS _erpl_rev_run_seq START 1");
    if (seq->HasError())
        throw std::runtime_error("DuckDB run-seq init failed: " + seq->GetError());
    auto rstat = con.Query(
        "CREATE TABLE IF NOT EXISTS _erpl_rev_run_stats ("
        "run_id BIGINT PRIMARY KEY DEFAULT nextval('_erpl_rev_run_seq'), "
        "ts TIMESTAMPTZ DEFAULT now(), "
        "target VARCHAR, source VARCHAR, run_type VARCHAR, method VARCHAR, status VARCHAR, "
        "duration_ms BIGINT, rows_read BIGINT, rows_ins BIGINT, rows_upd BIGINT, rows_del BIGINT, "
        "wm_from VARCHAR, wm_to VARCHAR, jobs INTEGER, error_text VARCHAR)");
    if (rstat->HasError())
        throw std::runtime_error("DuckDB run-stats init failed: " + rstat->GetError());
    auto rview = con.Query(
        "CREATE OR REPLACE VIEW erpl_rev_run_stats AS SELECT "
        "run_id, ts AS finished_at, "
        "ts - (COALESCE(duration_ms,0) * INTERVAL '1 millisecond') AS started_at, "
        "target, source, run_type, method, status, duration_ms, "
        "rows_read, rows_ins, rows_upd, rows_del, "
        "(COALESCE(rows_ins,0)+COALESCE(rows_upd,0)+COALESCE(rows_del,0)) AS rows_applied, "
        "CASE WHEN duration_ms > 0 THEN "
        "(COALESCE(rows_ins,0)+COALESCE(rows_upd,0)+COALESCE(rows_del,0))*1000.0/duration_ms "
        "ELSE NULL END AS rows_per_sec, "
        "wm_from, wm_to, jobs, (status='SUCCESS') AS is_success, error_text "
        "FROM _erpl_rev_run_stats");
    if (rview->HasError())
        throw std::runtime_error("DuckDB run-stats view init failed: " + rview->GetError());
    // Trigger-CDC state machine (opt-in physical-delete tier, ADR-0004 / epic #17).
    // One row per CDC target: config + provisioning status + log position. Guarded
    // transitions live in the CDC* methods; the table itself is plain DuckDB so the
    // state survives a server restart.
    auto cdc = con.Query(
        "CREATE TABLE IF NOT EXISTS _erpl_rev_cdc ("
        "target VARCHAR PRIMARY KEY, source VARCHAR NOT NULL, keys VARCHAR NOT NULL, "
        "platform VARCHAR DEFAULT 'HANA', mode VARCHAR DEFAULT 'DELETE_ONLY', "
        "status VARCHAR DEFAULT 'PROVISIONED', log_table VARCHAR, position BIGINT DEFAULT 0, "
        "provisioned_ts TIMESTAMPTZ, seeded_ts TIMESTAMPTZ, last_run_ts TIMESTAMPTZ, error VARCHAR)");
    if (cdc->HasError())
        throw std::runtime_error("DuckDB cdc-state init failed: " + cdc->GetError());
    // Boot init: explicit init_sql (CLI/--init-file) wins; else env fallback. Runs
    // INSTALL/LOAD/CREATE SECRET/ATTACH so replication can publish to external
    // targets (parquet object stores, postgres, ducklake, bigquery, iceberg).
    std::string init = init_sql;
    if (init.empty()) {
        if (const char *extra = std::getenv("ERPL_REV_DUCKDB_INIT"))
            init = extra;
    }
    if (!init.empty()) {
        auto e = con.Query(init);
        if (e->HasError())
            throw std::runtime_error("DuckDB init SQL failed: " + e->GetError());
    }
}

DuckDbBridge::~DuckDbBridge() = default;

CursorOpen DuckDbBridge::OpenCursor(const std::string &sql) {
    auto cur = std::make_shared<Cursor>();
    cur->con = std::make_unique<duckdb::Connection>(*db_);
    auto r = cur->con->SendQuery(sql);   // streaming; runs all statements, streams the last
    if (r->HasError())
        throw std::runtime_error("DuckDB query failed: " + r->GetError());
    std::unordered_map<std::string, int> seen;
    for (duckdb::idx_t c = 0; c < r->ColumnCount(); c++) {
        std::string name = SanitizeColName(r->names[c]);
        if (int &n = seen[name]) {            // collision -> suffix _2, _3, …
            std::string suffix = "_" + std::to_string(++n);
            if (name.size() + suffix.size() > 30) name = name.substr(0, 30 - suffix.size());
            name += suffix;
        } else { n = 1; }
        cur->columns.push_back({name, r->types[c].ToString()});
    }
    cur->result = std::move(r);

    std::lock_guard<std::mutex> lk(cursors_->mtx);
    if (cursors_->map.size() >= CursorStore::kMaxOpen)
        throw std::runtime_error("too many open cursors");
    std::string handle = "cur_" + std::to_string(++cursors_->counter);
    cursors_->map[handle] = cur;
    return {handle, cur->columns};
}

CursorPage DuckDbBridge::FetchCursor(const std::string &handle, long long page_rows) {
    std::shared_ptr<Cursor> cur;
    {
        std::lock_guard<std::mutex> lk(cursors_->mtx);
        auto it = cursors_->map.find(handle);
        if (it == cursors_->map.end())
            throw std::runtime_error("unknown cursor handle: " + handle);
        cur = it->second;
    }
    std::lock_guard<std::mutex> lk(cur->mtx);

    // Page size is fixed at the first FetchCursor (prefetch reuses it); later
    // page_rows changes are ignored so a buffered page stays valid.
    if (cur->page_rows == 0) cur->page_rows = page_rows;
    const long long pr = cur->page_rows;

    // Consume the prefetched page if one is in flight (this overlapped the RFC
    // round-trip); otherwise produce the first page synchronously.
    CursorPage page = cur->pending.valid() ? cur->pending.get()
                                           : ProducePage(*cur, pr);

    // Kick off prefetch of the NEXT page unless the stream is exhausted.
    if (!page.done) {
        Cursor *raw = cur.get();
        cur->pending = std::async(std::launch::async,
                                  [raw, pr] { return ProducePage(*raw, pr); });
    }
    return page;
}

void DuckDbBridge::CloseCursor(const std::string &handle) {
    // Take ownership of the cursor under the map lock, then RELEASE the lock before
    // letting it destruct: ~Cursor blocks in the pending-future destructor until the
    // background prefetch finishes touching result/con, and we must not hold the
    // shared cursor-map mutex while waiting (it would stall every other cursor op).
    std::shared_ptr<Cursor> cur;
    {
        std::lock_guard<std::mutex> lk(cursors_->mtx);
        auto it = cursors_->map.find(handle);
        if (it != cursors_->map.end()) { cur = std::move(it->second); cursors_->map.erase(it); }
    }
    // `cur` destructs here, outside the map lock.
}

// Run a statement on a given connection and throw on error. Each public bridge
// operation uses its OWN connection (DuckDB allows many concurrent connections on
// one database — the cursor prefetch already relies on this), so the operations
// are thread-safe WITHOUT a global lock, enabling parallel ingest and reads.
static void Exec(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    if (r->HasError())
        throw std::runtime_error("DuckDB execute failed: " + r->GetError());
}

void DuckDbBridge::Execute(const std::string &sql) {
    duckdb::Connection con(*db_);
    Exec(con, sql);
}

std::string DuckDbBridge::StartQuack(const std::string &listen, bool allow_other_host,
                                    const std::string &token) {
    // The quack extension is not bundled — pull it from the configured
    // extension repository (idempotent if already cached) and load it.
    Execute("INSTALL quack");
    Execute("LOAD quack");

    std::string sql = "CALL quack_serve(" + SqlQuote(listen);
    if (allow_other_host) sql += ", allow_other_hostname => true";
    if (!token.empty()) sql += ", token => " + SqlQuote(token);
    sql += ")";

    QueryResult r = Query(sql);   // returns the listening URI / HTTP URL / token
    std::string out = "[";
    for (size_t i = 0; i < r.rows.size(); i++) { if (i) out += ","; out += r.rows[i]; }
    out += "]";
    return out;
}

void DuckDbBridge::StopQuack(const std::string &listen) {
    Execute("CALL quack_stop(" + SqlQuote(listen) + ")");
}

QueryResult DuckDbBridge::Query(const std::string &sql, long long max_rows, bool want_total) {
    // Stream the result: SendQuery runs any leading statements (INSTALL/LOAD/…)
    // eagerly and returns the LAST statement's result lazily, so a capped SELECT
    // stops after cap+1 rows instead of materializing + draining the whole result
    // (a capped `SELECT *` over a huge table no longer scans it all). With
    // want_total=true (or no cap), the full result is drained to report the exact
    // total. Non-result final statements (INSERT/DDL/…) just run.
    duckdb::Connection con(*db_);
    auto head = con.SendQuery(sql);
    if (head->HasError())
        throw std::runtime_error("DuckDB query failed: " + head->GetError());
    // Multi-statement: leading statements are materialized + chained via ->next;
    // the LAST is the (streamed) result we return. `head` owns the chain — when it
    // destructs on return, an early-stopped stream is cancelled.
    duckdb::QueryResult *r = head.get();
    while (r->next) r = r->next.get();
    if (r->HasError())
        throw std::runtime_error("DuckDB query failed: " + r->GetError());

    QueryResult out;
    if (r->properties.return_type != duckdb::StatementReturnType::QUERY_RESULT)
        return out;

    const idx_t ncol = r->ColumnCount();
    for (idx_t c = 0; c < ncol; c++)
        out.columns.push_back({r->names[c], r->types[c].ToString()});

    long long total = 0;   // rows seen (== shipped unless want_total drains past cap)
    bool more = false;     // at least one row exists beyond the cap
    bool stop = false;
    while (!stop) {
        auto chunk = r->Fetch();
        if (!chunk || chunk->size() == 0) break;
        const idx_t nrow = chunk->size();
        for (idx_t row = 0; row < nrow; row++) {
            if (max_rows > 0 && (long long)out.rows.size() >= max_rows) {
                more = true;
                if (!want_total) { stop = true; break; }  // stop: the stream is cancelled on return
                total++;                                  // want_total: count the rest, no JSON
                continue;
            }
            total++;
            std::string obj = "{";
            for (idx_t c = 0; c < ncol; c++) {
                if (c) obj += ",";
                obj += json::QuoteString(out.columns[c].name);
                obj += ":";
                auto val = chunk->GetValue(c, row);
                if (val.IsNull()) {
                    obj += "null";
                } else {
                    auto id = r->types[c].id();
                    bool numeric =
                        id == duckdb::LogicalTypeId::TINYINT ||
                        id == duckdb::LogicalTypeId::SMALLINT ||
                        id == duckdb::LogicalTypeId::INTEGER ||
                        id == duckdb::LogicalTypeId::BIGINT ||
                        id == duckdb::LogicalTypeId::HUGEINT ||
                        id == duckdb::LogicalTypeId::UTINYINT ||
                        id == duckdb::LogicalTypeId::USMALLINT ||
                        id == duckdb::LogicalTypeId::UINTEGER ||
                        id == duckdb::LogicalTypeId::UBIGINT ||
                        id == duckdb::LogicalTypeId::FLOAT ||
                        id == duckdb::LogicalTypeId::DOUBLE ||
                        id == duckdb::LogicalTypeId::DECIMAL;
                    bool boolean = id == duckdb::LogicalTypeId::BOOLEAN;
                    if (numeric || boolean) obj += val.ToString();
                    else obj += json::QuoteString(val.ToString());
                }
            }
            obj += "}";
            out.rows.push_back(std::move(obj));
        }
    }
    out.truncated = more;
    // Exact total when drained (want_total or uncapped); otherwise a cap+1
    // sentinel so the caller still detects truncation (row_count > shipped)
    // without us scanning the whole result just to count it.
    out.row_count = want_total ? total : (more ? max_rows + 1 : total);
    return out;
}

namespace {

// Build one INSERT (… ON CONFLICT DO UPDATE for upsert) from column names and
// already-SQL-rendered cell literals (NULL / number / quoted string).
std::string BuildInsert(const std::string &target,
                        const std::vector<std::string> &cols,
                        const std::vector<std::string> &cell_sql,
                        IngestMode mode,
                        const std::vector<std::string> &key_cols) {
    std::ostringstream sql;
    sql << "INSERT INTO " << target << " (";
    for (size_t i = 0; i < cols.size(); i++) { if (i) sql << ","; sql << cols[i]; }
    sql << ") VALUES (";
    for (size_t i = 0; i < cell_sql.size(); i++) { if (i) sql << ","; sql << cell_sql[i]; }
    sql << ")";
    if (mode == IngestMode::Upsert && !key_cols.empty()) {
        sql << " ON CONFLICT (";
        for (size_t i = 0; i < key_cols.size(); i++) { if (i) sql << ","; sql << key_cols[i]; }
        sql << ") DO UPDATE SET ";
        bool first = true;
        for (auto &col : cols) {
            bool is_key = false;
            for (auto &k : key_cols) if (k == col) { is_key = true; break; }
            if (is_key) continue;
            if (!first) sql << ",";
            first = false;
            sql << col << "=excluded." << col;
        }
    }
    return sql.str();
}

} // namespace

long long DuckDbBridge::Ingest(const std::string &target,
                               const std::string &json_rows,
                               IngestMode mode,
                               const std::vector<std::string> &key_cols,
                               const std::string &parquet_out,
                               const std::string &init_sql,
                               const std::string &ddl) {
    duckdb::Connection con(*db_);   // own connection (thread-safe, no global lock)
    // Optional setup, in order: user init SQL (e.g. LOAD/INSTALL), then the
    // typed CREATE TABLE, before any rows are inserted.
    if (!init_sql.empty()) Exec(con, init_sql);
    if (!ddl.empty()) Exec(con, ddl);

    auto rows = json::ParseRows(json_rows);
    if (rows.empty()) {
        if (!parquet_out.empty())
            Exec(con, "COPY " + target + " TO " + SqlQuote(parquet_out) + " (FORMAT PARQUET)");
        return 0;
    }

    // Column order is taken from the first row; all rows must share it.
    std::vector<std::string> cols;
    for (auto &cell : rows.front()) cols.push_back(cell.key);

    long long n = 0;
    for (auto &row : rows) {
        std::vector<std::string> cell_sql;
        cell_sql.reserve(row.size());
        for (auto &c : row) cell_sql.push_back(CellToSql(c));
        Exec(con, BuildInsert(target, cols, cell_sql, mode, key_cols));
        n++;
    }

    if (!parquet_out.empty())
        Exec(con, "COPY " + target + " TO " + SqlQuote(parquet_out) + " (FORMAT PARQUET)");
    return n;
}

long long DuckDbBridge::IngestBxml(const std::string &target,
                                   const std::string &bxml,
                                   IngestMode mode,
                                   const std::vector<std::string> &key_cols,
                                   const std::string &parquet_out,
                                   const std::string &init_sql,
                                   const std::string &ddl,
                                   const std::string &op_col) {
    // Own connection: the appended package and the statement that consumes it
    // run on it, so concurrent ingests (async pipeline / partitioned loads)
    // don't collide and don't serialize.
    // The package may arrive gzip-framed (the ABAP side compresses wide payloads;
    // see src/payload.hpp). Inflating here rather than in the RFC handler keeps
    // the unit suite on the same path the server takes. Bind by pointer so the
    // uncompressed path -- still the default, and up to ~180 MB -- is not copied
    // just to reach the decoder.
    std::string inflated;
    const std::string *payload = &bxml;
    if (IsGzip(bxml)) {
        inflated = MaybeInflate(bxml);
        payload = &inflated;
    }

    duckdb::Connection con(*db_);
    if (!init_sql.empty()) Exec(con, init_sql);
    if (!ddl.empty()) Exec(con, ddl);

    // Streamed, not materialised. `Decode` would build the whole package as a
    // vector<vector<string>> first -- 21 million std::string allocations and
    // ~0.5 GiB resident for a 50k-row, 420-column package -- only for the loop
    // below to copy every cell straight back out into DuckDB's vectors. The
    // streaming form hands each row over as views into `bxml`, so the copy into
    // DuckDB is the only one that happens.
    sxml::Table tbl;   // columns only; rows never populated on this path

    // Bulk path (benchmarked ~250x the old per-row INSERT, see test/bench_ingest):
    // append the package's RAW string cells straight into a QueryAppender, whose
    // ColumnDataCollection is injected into ONE vectorized, atomic statement as a
    // never-materialized CTE named `src_rel`, with per-column casts in the
    // projection. This replaced a physical VARCHAR/BLOB staging TEMP table: that
    // table was written once and read once, but still paid full storage cost on
    // the way in -- string statistics, HyperLogLog distinct counts, UTF-8
    // analysis and dictionary compression, ~16% of ingest time in the flamegraph,
    // all of it discarded immediately afterwards.
    const std::string src_rel = "erpl_pkg";

    // Delta MERGE: the payload may carry a control op column (I/U/D). It is
    // appended (so the DELETE can read it) but is NOT a target column -- exclude
    // it from the INSERT column list and the projection. mode==Merge with no
    // op_col is a plain key-based upsert. Hoisted above the decode because the
    // statement has to exist before the first row is appended.
    const std::string opl = LowerName(op_col);
    const bool merge = (mode == IngestMode::Merge) && !opl.empty();
    auto is_op = [&](const std::string &c) { return !opl.empty() && LowerName(c) == opl; };
    const bool upsert = (mode == IngestMode::Upsert || mode == IngestMode::Merge) &&
                        !key_cols.empty();

    // Columns first, then rows -- the streaming decode calls back with the
    // column names once, before any row, which is exactly the order this needs:
    // the statement cannot be built until the columns are known, and the rows
    // can then be appended as they are parsed.
    std::vector<duckdb::LogicalType> coltypes;
    std::unique_ptr<duckdb::QueryAppender> app;
    duckdb::DataChunk chunk;
    size_t ncols = 0, nrows = 0, in_chunk = 0;

    auto on_columns = [&](const std::vector<std::string> &cols) {
        tbl.columns = cols;
        ncols = cols.size();

        // Target column types, in the BXML column order (target names fold to lower).
        auto desc = con.Query("SELECT * FROM " + target + " LIMIT 0");
        if (desc->HasError())
            throw std::runtime_error("DuckDB describe failed: " + desc->GetError());
        std::unordered_map<std::string, duckdb::LogicalType> tcol;
        for (duckdb::idx_t c = 0; c < desc->ColumnCount(); c++)
            tcol.emplace(LowerName(desc->names[c]), desc->types[c]);
        coltypes.reserve(ncols);
        for (auto &cn : cols) {
            auto it = tcol.find(LowerName(cn));
            coltypes.push_back(it != tcol.end()
                                   ? it->second
                                   : duckdb::LogicalType(duckdb::LogicalTypeId::VARCHAR));
        }

        // Appended package: BLOB for binary target columns, VARCHAR otherwise.
        duckdb::vector<duckdb::LogicalType> vtypes;
        vtypes.reserve(ncols);
        for (size_t i = 0; i < ncols; i++)
            vtypes.push_back(coltypes[i].id() == duckdb::LogicalTypeId::BLOB
                                 ? duckdb::LogicalType::BLOB
                                 : duckdb::LogicalType::VARCHAR);
        duckdb::vector<std::string> vnames(cols.begin(), cols.end());

        // One set-based statement with per-column casts (atomic: a bad cast
        // aborts the whole package and leaves the target unchanged).
        // Use the target's actual (lowercased) column names everywhere. An
        // UPPERCASE target column list in `INSERT … (COLS) … ON CONFLICT`
        // mis-maps the conflict arbiter and nulls the key -- lowercase (matching
        // the stored names) is safe.
        std::string proj;
        for (size_t i = 0; i < cols.size(); i++) {
            if (is_op(cols[i])) continue;
            if (!proj.empty()) proj += ",";
            const std::string cn = LowerName(cols[i]);
            switch (coltypes[i].id()) {
                case duckdb::LogicalTypeId::BLOB:
                case duckdb::LogicalTypeId::VARCHAR:
                    proj += cn;                                                  // passthrough
                    break;
                case duckdb::LogicalTypeId::DATE:
                    // An initial/blank DATS has no valid date: the kernel renders it
                    // as "0000-00-00" (zero) or "    -  -  " (blank, 8 spaces). Map any
                    // date made up of nothing but blanks and dashes, plus the zero-date,
                    // to NULL; a genuinely malformed non-blank date still errors loudly.
                    // `trim(c,' -')` is the two-argument trim (strip that character set
                    // from both ends), exactly equivalent to the `trim(replace(c,'-',''))`
                    // this replaced -- both are empty iff every character is a blank or a
                    // dash -- but one pass over the cell instead of two, and no
                    // intermediate string. Worth it: it runs on every DATS cell of every
                    // row, and the pair showed up at 2.7% of ingest in the flamegraph.
                    proj += "CASE WHEN " + cn + "='0000-00-00' OR trim(" + cn +
                            ",' -') = '' THEN NULL ELSE " + cn + "::DATE END";
                    break;
                default:
                    proj += cn + "::" + coltypes[i].ToString();
                    break;
            }
            proj += " AS " + cn;   // name the projected column so a CTAS clone keeps it
        }
        std::string collist;
        for (size_t i = 0; i < cols.size(); i++) {
            if (is_op(cols[i])) continue;
            if (!collist.empty()) collist += ",";
            collist += LowerName(cols[i]);
        }

        // Apply mode. Upsert/Merge with keys apply as one native, atomic
        // `MERGE INTO`. (DuckDB 1.5.4 fixes the bug that previously forced a
        // DELETE-then-INSERT workaround: #22825 made the INSERT…SELECT ON
        // CONFLICT column match case-insensitive -- our appended package carries
        // SAP's UPPERCASE names, so a conflict-resolving write used to mis-map
        // the arbiter and NULL the key -- and #23014 hardened MERGE INTO WHEN NOT
        // MATCHED binding.) The source projection casts the VARCHAR/BLOB cells to
        // the target types, so the ON keys are type-compatible and a bad cast
        // aborts the single statement, leaving the target unchanged.
        // (See test/test_duckdb_bridge.cpp merge cases.)
        std::string sql;
        if (upsert) {
            auto is_key = [&](const std::string &cn) {
                for (auto &k : key_cols) if (LowerName(k) == cn) return true;
                return false;
            };
            // ON predicate over the keys; SET list + INSERT value list over the non-key
            // (and non-op) columns. Keys are never updated.
            std::string on, setlist, svals;
            for (size_t i = 0; i < key_cols.size(); i++) {
                if (i) on += " AND ";
                const std::string k = LowerName(key_cols[i]);
                on += "t." + k + " = s." + k;
            }
            for (size_t i = 0; i < cols.size(); i++) {
                if (is_op(cols[i])) continue;
                const std::string cn = LowerName(cols[i]);
                if (!svals.empty()) svals += ",";
                svals += "s." + cn;
                if (is_key(cn)) continue;
                if (!setlist.empty()) setlist += ",";
                setlist += cn + "=s." + cn;
            }
            // Source: the cast projection, plus the op control column for delta MERGE.
            std::string src = "SELECT " + proj;
            if (merge) src += ", " + opl;
            src += " FROM " + src_rel;

            sql = "MERGE INTO " + target + " AS t USING (" + src + ") AS s ON " + on;
            if (merge) {
                // op_col (I/U/D) drives the action: delete D rows, upsert I/U rows.
                sql += " WHEN MATCHED AND lower(s." + opl + ") = 'd' THEN DELETE";
                if (!setlist.empty())
                    sql += " WHEN MATCHED AND lower(s." + opl + ") IN ('i','u') THEN UPDATE SET " + setlist;
                sql += " WHEN NOT MATCHED AND lower(s." + opl + ") IN ('i','u') THEN INSERT (" +
                       collist + ") VALUES (" + svals + ")";
            } else {
                if (!setlist.empty()) sql += " WHEN MATCHED THEN UPDATE SET " + setlist;
                sql += " WHEN NOT MATCHED THEN INSERT (" + collist + ") VALUES (" + svals + ")";
            }
        } else {
            std::string sel = "SELECT " + proj + " FROM " + src_rel;
            if (merge) sel += " WHERE lower(" + opl + ") IN ('i','u')";   // exclude deletes
            sql = "INSERT INTO " + target + " (" + collist + ") " + sel;
        }

        app = std::make_unique<duckdb::QueryAppender>(con, sql, vtypes, vnames, src_rel);
        chunk.Initialize(duckdb::Allocator::DefaultAllocator(), vtypes);
        chunk.Reset();
    };

    // One row into the pending DataChunk; flush a full one. Cells are the
    // decoder's views into `bxml`, so AddStringOrBlob's copy is the only one.
    // (No NULLs: a decoded BXML cell is always present -- empty element = "".)
    auto on_row = [&](const std::vector<std::string_view> &vals) {
        for (size_t c = 0; c < ncols; c++) {
            duckdb::Vector &v = chunk.data[c];
            auto out = duckdb::FlatVector::GetData<duckdb::string_t>(v);
            std::string_view cell = c < vals.size() ? vals[c] : std::string_view();
            // AddStringOrBlob: no UTF-8 validation -- fits both VARCHAR text and
            // raw BLOB bytes.
            out[in_chunk] = duckdb::StringVector::AddStringOrBlob(v, cell.data(), cell.size());
        }
        ++in_chunk;
        ++nrows;
        if (in_chunk == STANDARD_VECTOR_SIZE) {
            chunk.SetCardinality(in_chunk);
            app->AppendDataChunk(chunk);
            chunk.Reset();
            in_chunk = 0;
        }
    };

    if (!payload->empty()) sxml::DecodeStreaming(*payload, on_columns, on_row);

    if (nrows == 0) {
        if (!parquet_out.empty())
            Exec(con, "COPY " + target + " TO " + SqlQuote(parquet_out) + " (FORMAT PARQUET)");
        return 0;
    }
    if (in_chunk) {
        chunk.SetCardinality(in_chunk);
        app->AppendDataChunk(chunk);
    }
    // A QueryAppender runs its statement once per flush, and BaseAppender flushes
    // on its own every DEFAULT_FLUSH_COUNT (204800) rows. Our packages are well
    // under that -- one flush, one statement -- but an oversized one must not be
    // able to half-apply, so the whole append is one explicit transaction.
    con.BeginTransaction();
    try {
        app->Close();
    } catch (...) {
        con.Rollback();
        throw;
    }
    con.Commit();

    if (!parquet_out.empty())
        Exec(con, "COPY " + target + " TO " + SqlQuote(parquet_out) + " (FORMAT PARQUET)");
    return static_cast<long long>(nrows);
}

SnapshotResult DuckDbBridge::SnapshotMerge(const std::string &target,
                                           const std::string &staging,
                                           const std::vector<std::string> &keys) {
    duckdb::Connection con(*db_);

    // Target columns (lowercased) for the DO UPDATE SET list.
    auto desc = con.Query("SELECT * FROM " + target + " LIMIT 0");
    if (desc->HasError())
        throw std::runtime_error("DuckDB describe failed: " + desc->GetError());
    std::vector<std::string> cols;
    for (duckdb::idx_t c = 0; c < desc->ColumnCount(); c++)
        cols.push_back(LowerName(desc->names[c]));

    // Join predicate "a.k1 = b.k1 AND a.k2 = b.k2 …" for the anti-joins.
    auto key_join = [&](const std::string &a, const std::string &b) {
        std::string j;
        for (size_t i = 0; i < keys.size(); i++) {
            if (i) j += " AND ";
            const std::string k = LowerName(keys[i]);
            j += a + "." + k + " = " + b + "." + k;
        }
        return j;
    };

    // Exact ins/upd/del counts, computed from the pre-merge sets (cheap key-only
    // anti-joins). ins = snapshot keys new to target; upd = keys in both;
    // del = target keys absent from the snapshot.
    SnapshotResult res;
    {
        const std::string ej = key_join("t", "s");
        std::string q =
            "SELECT "
            "(SELECT count(*) FROM " + staging + " s WHERE NOT EXISTS "
              "(SELECT 1 FROM " + target + " t WHERE " + ej + ")) AS ins,"
            "(SELECT count(*) FROM " + staging + " s WHERE EXISTS "
              "(SELECT 1 FROM " + target + " t WHERE " + ej + ")) AS upd,"
            "(SELECT count(*) FROM " + target + " t WHERE NOT EXISTS "
              "(SELECT 1 FROM " + staging + " s WHERE " + ej + ")) AS del";
        auto cr = con.Query(q);
        if (cr->HasError())
            throw std::runtime_error("DuckDB snapshot count failed: " + cr->GetError());
        res.ins = cr->GetValue(0, 0).GetValue<int64_t>();
        res.upd = cr->GetValue(1, 0).GetValue<int64_t>();
        res.del = cr->GetValue(2, 0).GetValue<int64_t>();
    }

    // Reconcile the full snapshot onto the target with one native MERGE INTO:
    // update matched keys, insert new keys, delete target keys absent from the
    // snapshot — then drop the staging table, atomically. (HLD §4.3.)
    std::string setlist, collist, svals;
    for (auto &cn : cols) {
        if (!collist.empty()) { collist += ","; svals += ","; }
        collist += cn;
        svals += "s." + cn;
        bool is_key = false;
        for (auto &k : keys) if (LowerName(k) == cn) { is_key = true; break; }
        if (is_key) continue;
        if (!setlist.empty()) setlist += ",";
        setlist += cn + "=s." + cn;
    }
    std::string m = "MERGE INTO " + target + " AS t USING " + staging + " AS s ON " +
                    key_join("t", "s");
    if (!setlist.empty()) m += " WHEN MATCHED THEN UPDATE SET " + setlist;   // no-op if all-key
    m += " WHEN NOT MATCHED THEN INSERT (" + collist + ") VALUES (" + svals + ")";
    m += " WHEN NOT MATCHED BY SOURCE THEN DELETE";

    Exec(con, "BEGIN");
    try {
        Exec(con, m);
        Exec(con, "DROP TABLE " + staging);
        Exec(con, "COMMIT");
    } catch (...) {
        Exec(con, "ROLLBACK");
        throw;
    }
    return res;
}

// --- Trigger-CDC state machine ---------------------------------------------

CdcState DuckDbBridge::CdcGet(const std::string &target) {
    duckdb::Connection con(*db_);
    auto r = con.Query(
        "SELECT target, source, keys, platform, mode, log_table, status, position "
        "FROM _erpl_rev_cdc WHERE target = " + SqlLit(target));
    if (r->HasError())
        throw std::runtime_error("DuckDB cdc get failed: " + r->GetError());
    CdcState s;
    if (r->RowCount() == 0) return s;
    s.exists = true;
    s.target = r->GetValue(0, 0).ToString();
    s.source = r->GetValue(1, 0).ToString();
    s.keys = r->GetValue(2, 0).ToString();
    s.platform = r->GetValue(3, 0).ToString();
    s.mode = r->GetValue(4, 0).ToString();
    auto lt = r->GetValue(5, 0);
    s.log_table = lt.IsNull() ? std::string() : lt.ToString();
    s.status = r->GetValue(6, 0).ToString();
    s.position = r->GetValue(7, 0).GetValue<int64_t>();
    return s;
}

void DuckDbBridge::CdcRegister(const std::string &target, const std::string &source,
                               const std::string &keys, const std::string &platform,
                               const std::string &mode, const std::string &log_table) {
    duckdb::Connection con(*db_);
    // (Re)create the config in PROVISIONED with position reset — idempotent, and
    // the legal way back from DISABLED.
    Exec(con,
        "INSERT INTO _erpl_rev_cdc "
        "(target, source, keys, platform, mode, log_table, status, position, provisioned_ts) "
        "VALUES (" + SqlLit(target) + "," + SqlLit(source) + "," + SqlLit(keys) + "," +
        SqlLit(platform.empty() ? "HANA" : platform) + "," +
        SqlLit(mode.empty() ? "DELETE_ONLY" : mode) + "," + SqlLit(log_table) +
        ",'PROVISIONED',0,now()) "
        "ON CONFLICT (target) DO UPDATE SET source=excluded.source, keys=excluded.keys, "
        "platform=excluded.platform, mode=excluded.mode, log_table=excluded.log_table, "
        "status='PROVISIONED', position=0, provisioned_ts=now(), error=NULL");
}

void DuckDbBridge::CdcSetStatus(const std::string &target, const std::string &status) {
    CdcState s = CdcGet(target);
    if (!s.exists) throw std::runtime_error("CDC: no registration for " + target);
    if (s.status == status && status != "ACTIVE") return;   // no-op (ACTIVE re-stamps)
    if (!AllowedCdcTransition(s.status, status))
        throw std::runtime_error("CDC: illegal transition " + s.status + " -> " + status +
                                 " for " + target);
    duckdb::Connection con(*db_);
    std::string stamp;
    if (status == "SEEDED") stamp = ", seeded_ts=now()";
    else if (status == "ACTIVE") stamp = ", last_run_ts=now()";
    Exec(con, "UPDATE _erpl_rev_cdc SET status=" + SqlLit(status) + stamp +
              " WHERE target=" + SqlLit(target));
}

void DuckDbBridge::CdcAdvancePosition(const std::string &target, long long position) {
    CdcState s = CdcGet(target);
    if (!s.exists) throw std::runtime_error("CDC: no registration for " + target);
    if (position < s.position)
        throw std::runtime_error("CDC: position regression for " + target + " (" +
                                 std::to_string(position) + " < " + std::to_string(s.position) + ")");
    duckdb::Connection con(*db_);
    Exec(con, "UPDATE _erpl_rev_cdc SET position=" + std::to_string(position) +
              " WHERE target=" + SqlLit(target));
}

CdcApplyResult DuckDbBridge::CdcApply(const std::string &target, const std::string &staging,
                                      const std::vector<std::string> &keys) {
    CdcState st = CdcGet(target);
    if (!st.exists) throw std::runtime_error("CDC: no registration for " + target);
    CdcApplyResult res;
    res.prune_bound = st.position;   // nothing applied yet -> don't prune past here

    duckdb::Connection con(*db_);

    // Max consumed sequence in this batch (the new position / prune bound). An empty
    // staging table is a no-op: drop it and return with the position unchanged.
    auto mq = con.Query("SELECT max(\"_seq\") FROM " + staging);
    if (mq->HasError())
        throw std::runtime_error("CDC: staging read failed: " + mq->GetError());
    if (mq->RowCount() == 0 || mq->GetValue(0, 0).IsNull()) {
        Exec(con, "DROP TABLE IF EXISTS " + staging);
        return res;   // applied=false
    }
    const long long max_seq = mq->GetValue(0, 0).GetValue<int64_t>();

    // Target + staging column metadata up front: the key match AND the upsert both
    // cast the log's SAP-raw NVARCHAR text to the target column TYPES.
    auto tcols = con.Query("SELECT * FROM " + target + " LIMIT 0");
    if (tcols->HasError()) throw std::runtime_error("CDC: target describe failed: " + tcols->GetError());
    auto scols = con.Query("SELECT * FROM " + staging + " LIMIT 0");
    if (scols->HasError()) throw std::runtime_error("CDC: staging describe failed: " + scols->GetError());
    std::vector<std::string> sset;
    for (duckdb::idx_t c = 0; c < scols->ColumnCount(); c++) sset.push_back(LowerName(scols->names[c]));
    auto in_staging = [&](const std::string &n) {
        for (auto &s : sset) if (s == n) return true; return false;
    };
    auto type_of = [&](const std::string &n) -> std::string {
        for (duckdb::idx_t c = 0; c < tcols->ColumnCount(); c++)
            if (LowerName(tcols->names[c]) == n) return tcols->types[c].ToString();
        return "VARCHAR";
    };

    // Cast a log text value to a target column type. SAP dates/times are YYYYMMDD /
    // HHMMSS (not ISO), so parse them with strptime; everything else casts directly
    // (a NUMC key '0017' -> INTEGER 17, a CHAR key as-is, a decimal string -> DECIMAL).
    auto cast_to = [](const std::string &expr, const std::string &type) -> std::string {
        std::string T = type;
        for (char &ch : T) if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
        if (T == "DATE") return "try_strptime(" + expr + ", '%Y%m%d')::DATE";
        if (T == "TIME") return "try_strptime(" + expr + ", '%H%M%S')::TIME";
        if (T.rfind("TIMESTAMP", 0) == 0)
            return "try_strptime(" + expr + ", '%Y%m%d%H%M%S')::" + type;
        return "CAST(" + expr + " AS " + type + ")";
    };

    // Lower-cased key list (DuckDB stores unquoted identifiers lower case).
    std::vector<std::string> kl;
    for (auto &k : keys) kl.push_back(LowerName(k));
    std::string keycsv;
    for (size_t i = 0; i < kl.size(); i++) { if (i) keycsv += ","; keycsv += kl[i]; }

    // Coalesce: the latest staging row per key (highest "_seq" wins). Out-of-order
    // or duplicate sequences resolve deterministically by the window order.
    const std::string coalesced =
        "(SELECT * FROM " + staging +
        " QUALIFY row_number() OVER (PARTITION BY " + keycsv + " ORDER BY \"_seq\" DESC)=1)";

    // Key match: target value = the log text cast to the target's key type. `a` is the
    // target side, `b` the (coalesced) staging side.
    auto key_join = [&](const std::string &a, const std::string &b) {
        std::string j;
        for (size_t i = 0; i < kl.size(); i++) {
            if (i) j += " AND ";
            j += a + "." + kl[i] + " = " + cast_to(b + "." + kl[i], type_of(kl[i]));
        }
        return j;
    };

    // Net-op subsets of the coalesced batch.
    const std::string net_del = "(SELECT * FROM " + coalesced + " c WHERE lower(c.\"_op\")='d')";
    const std::string net_iu  = "(SELECT * FROM " + coalesced + " c WHERE lower(c.\"_op\") IN ('i','u'))";

    // Data columns to upsert = target columns also present in staging (so delete-only
    // staging, which carries only keys, yields a keys-only upsert == DO NOTHING).
    std::vector<std::string> dcols, dtypes;
    for (duckdb::idx_t c = 0; c < tcols->ColumnCount(); c++) {
        std::string n = LowerName(tcols->names[c]);
        if (in_staging(n)) { dcols.push_back(n); dtypes.push_back(tcols->types[c].ToString()); }
    }

    // Counts (pre-apply, key-only anti-joins): del = net-deletes present in target;
    // ins = net-I/U keys new to target; upd = net-I/U keys already present.
    {
        const std::string dj = key_join("t", "c");
        auto cr = con.Query(
            "SELECT "
            "(SELECT count(*) FROM " + net_del + " c WHERE EXISTS (SELECT 1 FROM " + target +
              " t WHERE " + dj + ")) AS del,"
            "(SELECT count(*) FROM " + net_iu + " c WHERE NOT EXISTS (SELECT 1 FROM " + target +
              " t WHERE " + dj + ")) AS ins,"
            "(SELECT count(*) FROM " + net_iu + " c WHERE EXISTS (SELECT 1 FROM " + target +
              " t WHERE " + dj + ")) AS upd");
        if (cr->HasError()) throw std::runtime_error("CDC: count failed: " + cr->GetError());
        res.del = cr->GetValue(0, 0).GetValue<int64_t>();
        res.ins = cr->GetValue(1, 0).GetValue<int64_t>();
        res.upd = cr->GetValue(2, 0).GetValue<int64_t>();
    }

    // Atomic apply: delete net-deletes, upsert net-I/U, advance position + mark ACTIVE,
    // drop staging. Any error rolls all of it back (position stays where it was).
    const std::string del_sql =
        "DELETE FROM " + target + " t WHERE EXISTS (SELECT 1 FROM " + net_del +
        " c WHERE " + key_join("t", "c") + ")";

    // Upsert the net inserts/updates as DELETE-then-INSERT (not ON CONFLICT): DuckDB's
    // ON CONFLICT NULLs the leading column of a composite key when the source is a
    // SELECT, so the whole engine uses delete-then-insert (same as the delta MERGE).
    std::string del_iu_sql, ins_sql;
    if (!dcols.empty()) {
        std::string collist, sellist;
        for (size_t i = 0; i < dcols.size(); i++) {
            if (i) { collist += ","; sellist += ","; }
            collist += dcols[i];
            // The log delivers every value as SAP-raw text; cast it to the target
            // column type for the insert (keys included, harmless for delete-only).
            sellist += cast_to("c." + dcols[i], dtypes[i]) + " AS " + dcols[i];
        }
        del_iu_sql = "DELETE FROM " + target + " t WHERE EXISTS (SELECT 1 FROM " + net_iu +
                     " c WHERE " + key_join("t", "c") + ")";
        ins_sql = "INSERT INTO " + target + " (" + collist + ") SELECT " + sellist +
                  " FROM " + net_iu + " c";
    }

    Exec(con, "BEGIN");
    try {
        Exec(con, del_sql);
        if (!del_iu_sql.empty()) { Exec(con, del_iu_sql); Exec(con, ins_sql); }
        Exec(con, "UPDATE _erpl_rev_cdc SET position=" + std::to_string(max_seq) +
                  ", last_run_ts=now(), status='ACTIVE' WHERE target=" + SqlLit(target));
        Exec(con, "DROP TABLE " + staging);
        Exec(con, "COMMIT");
    } catch (...) {
        Exec(con, "ROLLBACK");
        throw;
    }
    res.prune_bound = max_seq;
    res.applied = true;
    return res;
}

} // namespace erpl_rev
