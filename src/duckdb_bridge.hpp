// DuckDbBridge: owns an in-process DuckDB and runs the two operations the RFC
// server exposes to ABAP — Query (SELECT → rows) and Ingest (rows → table,
// INSERT/UPSERT, optionally COPY to parquet).
//
// Payloads cross the RFC boundary as JSON strings, so the bridge speaks JSON too:
// QueryResult.rows is a vector of JSON object strings (one per row), and Ingest
// takes a JSON array string. This keeps the bridge schema-generic.
#pragma once

#include <string>
#include <vector>
#include <memory>

namespace duckdb { class DuckDB; class Connection; }

namespace erpl_rev {

struct QueryColumn {
    std::string name;
    std::string type;   // DuckDB LogicalType string, e.g. "BIGINT", "VARCHAR"
};

struct QueryResult {
    std::vector<QueryColumn> columns;
    std::vector<std::string> rows;   // each element is one JSON object (<= max_rows)
    long long row_count = 0;         // TOTAL rows the query produced (not capped)
    bool truncated = false;          // true if row_count > rows.size()
};

enum class IngestMode { Insert, Upsert, Merge };

// Result of a SnapshotMerge: how many rows the set-based diff inserted, updated
// (counted together as "upserted from staging"), and deleted (present in the
// target but absent from the fresh snapshot).
struct SnapshotResult {
    long long ins = 0;
    long long upd = 0;
    long long del = 0;
};

// Opened streaming cursor: a handle + the column metadata (so the caller can
// build its target structure once before fetching pages).
struct CursorOpen {
    std::string handle;
    std::vector<QueryColumn> columns;
};

// One streamed page: `bxml` is the binary-sXML encoding of up to page_rows rows
// (empty when fetched == 0); `done` is true once the stream is exhausted.
struct CursorPage {
    std::string bxml;
    long long fetched = 0;
    bool done = false;
};

// One row of the trigger-CDC state machine (_erpl_rev_cdc). `exists` is false when
// the target is not registered.
struct CdcState {
    bool exists = false;
    std::string target;
    std::string source;
    std::string keys;        // comma list
    std::string platform;    // HANA
    std::string mode;        // DELETE_ONLY | KEYS_IUD | IMAGE_IUD (FULL_IUD on read)
    std::string log_table;
    std::string status;      // PROVISIONED | SEEDED | ACTIVE | DISABLED
    long long position = 0;  // last consumed log sequence
};

// Result of a CdcApply: the I/U/D counts the coalesced log batch produced, and the
// prune bound — the confirmed log position ABAP may safely delete up to (`seq <=
// prune_bound`). `applied` is false for an empty batch (nothing read, prune_bound =
// the unchanged position).
struct CdcApplyResult {
    long long ins = 0;
    long long upd = 0;
    long long del = 0;
    long long prune_bound = 0;
    bool applied = false;
};

struct CursorStore;   // defined in the .cpp (owns DuckDB connections)

class DuckDbBridge {
public:
    // path empty => in-memory database. init_sql is arbitrary DuckDB SQL run ONCE
    // at startup on a GLOBAL connection (INSTALL/LOAD extensions, CREATE SECRET,
    // ATTACH external catalogs e.g. postgres/ducklake/bigquery) so replication can
    // publish to those targets. Empty => fall back to env ERPL_REV_DUCKDB_INIT.
    explicit DuckDbBridge(const std::string &path = "", const std::string &init_sql = "");
    ~DuckDbBridge();

    // Run a SELECT (or any query producing a result set) and return rows as JSON.
    // If max_rows > 0, at most that many row JSON objects are returned, but
    // row_count still reports the TOTAL produced and `truncated` is set — this
    // keeps multi-million-row results from overflowing the RFC string buffer /
    // ABAP JSON parser while still telling the caller the real size.
    // Streams the final statement and stops after max_rows (+1 to detect more) —
    // a capped query does NOT scan/drain the whole result. want_total=true drains
    // the rest to report the exact total (row_count); otherwise row_count is the
    // shipped count, or max_rows+1 when truncated. max_rows=0 = no cap (drain all).
    QueryResult Query(const std::string &sql, long long max_rows = 0,
                      bool want_total = false);

    // Run a statement with no result set (DDL, COPY, etc.).
    void Execute(const std::string &sql);

    // Start the DuckDB "quack" network server on `listen` (a quack URI such as
    // "quack:localhost" or "quack:0.0.0.0:9494"), exposing THIS in-process
    // database to remote DuckDB clients. INSTALL/LOAD the quack extension, then
    // CALL quack_serve(...). quack_serve returns immediately (the server runs on
    // background threads). `allow_other_host` must be true to bind a non-loopback
    // address. A non-empty `token` pins the client auth token (passed as
    // quack_serve's `token =>` parameter); empty lets quack generate a random
    // one. Returns quack_serve's result row(s) as a JSON array string — it
    // carries the listening URI, HTTP URL and auth token. Throws on failure
    // (e.g. engine < 1.5.3, or the extension cannot be installed).
    std::string StartQuack(const std::string &listen, bool allow_other_host,
                           const std::string &token = "");

    // Stop a quack server previously started on `listen` (CALL quack_stop(...)).
    void StopQuack(const std::string &listen);

    // Open an optional erpl-tunnel forward for the SAP gateway leg: INSTALL/LOAD
    // erpl_tunnel from the DuckDB community repository (signed, over HTTPS), then
    // run `import_sql` (see tunnel::ImportSql). The secret it names is the
    // operator's, defined in --init-file with erpl-tunnel's own CREATE SECRET, so
    // no credential passes through here.
    //
    // Throws with an actionable message rather than a DuckDB one when the
    // extension cannot be loaded -- the common cause is that no community build
    // exists for the exact DuckDB version this binary embeds, and "IO Error" does
    // not lead anyone to that.
    void StartTunnel(const std::string &import_sql);

    // The tunnels() row whose local_port matches, as a JSON object, or "" when
    // no such forward exists. Used to prove the forward came up before the RFC
    // registration is attempted, and to report liveness afterwards.
    std::string TunnelInfo(const std::string &local_port);

    // Ingest JSON rows into `target`:
    //  - Insert: append all rows.
    //  - Upsert: INSERT ... ON CONFLICT (key_cols) DO UPDATE (target needs a
    //    PRIMARY KEY / UNIQUE on key_cols).
    // If parquet_out is non-empty, COPY the whole target table to that parquet
    // file afterwards. Returns the number of rows ingested.
    long long Ingest(const std::string &target,
                     const std::string &json_rows,
                     IngestMode mode,
                     const std::vector<std::string> &key_cols,
                     const std::string &parquet_out,
                     const std::string &init_sql = "",
                     const std::string &ddl = "");

    // Ingest binary-sXML (BXML) rows — same semantics as the JSON overload, but
    // the rows come from `bxml` (as ABAP's cl_sxml_string_writer co_xt_binary +
    // CALL TRANSFORMATION id produce). Column names are the BXML element names;
    // every cell is inserted as a string literal and cast to the target column
    // type via the typed `ddl`. Returns the number of rows ingested.
    //
    // Delta (MERGE): when mode == Merge and op_col is non-empty, the staged
    // payload carries that control column with values I/U/D. The apply runs, in
    // one transaction, a DELETE of the 'D' keys followed by an UPSERT of the
    // 'I'/'U' rows; op_col is stripped from the target column list (it is control
    // data, not a target column). mode == Merge with an empty op_col degrades to
    // a plain key-based UPSERT.
    long long IngestBxml(const std::string &target,
                         const std::string &bxml,
                         IngestMode mode,
                         const std::vector<std::string> &key_cols,
                         const std::string &parquet_out,
                         const std::string &init_sql = "",
                         const std::string &ddl = "",
                         const std::string &op_col = "");

    // Snapshot diff/merge (physical-delete reconciliation): given a freshly
    // landed full snapshot in `staging`, upsert every snapshot row onto `target`
    // and DELETE the target keys absent from the snapshot, in one transaction,
    // then DROP the staging table. Used by the SNAPSHOT delta method to catch
    // hard deletes no change column can report. Returns the diff counts.
    SnapshotResult SnapshotMerge(const std::string &target,
                                 const std::string &staging,
                                 const std::vector<std::string> &keys);

    // --- Trigger-CDC state machine (_erpl_rev_cdc, created at boot) ----------
    // Guarded server-side so ABAP can't drive an illegal transition. Register
    // (re)creates the config in PROVISIONED with position 0; SetStatus enforces
    // PROVISIONED->SEEDED->ACTIVE->DISABLED (and DISABLED->PROVISIONED re-enable);
    // AdvancePosition is monotonic (a regression throws). State survives restart
    // (it's a normal DuckDB table in the same store).
    void CdcRegister(const std::string &target, const std::string &source,
                     const std::string &keys, const std::string &platform,
                     const std::string &mode, const std::string &log_table);
    void CdcSetStatus(const std::string &target, const std::string &status);
    void CdcAdvancePosition(const std::string &target, long long position);
    CdcState CdcGet(const std::string &target);

    // Apply one staged log batch to the target, atomically: coalesce the staging
    // rows to one net op per key (latest by sequence), DELETE the net-deletes,
    // upsert the net-inserts/updates (when staging carries the row image — full-IUD;
    // delete-only staging has none), advance the position to the max consumed
    // sequence, drop the staging table, and mark the run ACTIVE — all in one
    // transaction (an error rolls everything back and leaves the position untouched).
    // Returns the I/U/D counts and the prune bound. `keys` are the target key cols.
    // `images` (optional) is a second staging table holding the re-read row
    // images for a KEYS_IUD cycle, whose shadow log carries keys only. When it
    // is empty the row values come from the log itself, which is the IMAGE_IUD
    // path and is unchanged.
    CdcApplyResult CdcApply(const std::string &target, const std::string &staging,
                            const std::vector<std::string> &keys,
                            const std::string &images = "");

    // --- Streaming cursors (fixed-memory paging for large results) ----------
    // OpenCursor starts a streaming query on its OWN DuckDB connection (DuckDB
    // allows only one active stream per connection) and returns a handle + the
    // column metadata. FetchCursor pulls whole DataChunks until it has >=
    // page_rows rows (pass a multiple of 2048 to consume entire vectors) or the
    // stream ends, and returns them BXML-encoded. CloseCursor frees it. Memory
    // stays ~ one page regardless of total result size.
    CursorOpen OpenCursor(const std::string &sql);
    CursorPage FetchCursor(const std::string &handle, long long page_rows);
    void       CloseCursor(const std::string &handle);

private:
    // Each public operation opens its own duckdb::Connection on db_ (DuckDB allows
    // many concurrent connections on one database), so calls are thread-safe
    // without a global lock — enabling parallel ingest and reads. Cursors hold
    // their own connection too (see the .cpp).
    std::unique_ptr<duckdb::DuckDB> db_;
    std::unique_ptr<CursorStore> cursors_;
};

} // namespace erpl_rev
