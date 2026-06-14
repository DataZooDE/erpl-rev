#include <catch2/catch_test_macros.hpp>

#include "duckdb_bridge.hpp"
#include "json_util.hpp"
#include "sxml_binary.hpp"

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace erpl_rev;

// Portable temp path: under the OS temp dir, with forward slashes. DuckDB accepts
// '/' on every platform (and it sidesteps backslash escaping in SQL literals);
// std::remove accepts it too. Replaces hardcoded "/tmp/…" so the tests run on
// Windows as well as Linux/macOS.
static std::string TmpPath(const std::string &name) {
    return (std::filesystem::temp_directory_path() / name).generic_string();
}

// Absolute path to the sample parquet, injected by CMake (ERPL_REV_DATA_DIR),
// so the test is independent of the working directory it's launched from.
#ifndef ERPL_REV_DATA_DIR
#define ERPL_REV_DATA_DIR "."
#endif
static const std::string TAXI = std::string(ERPL_REV_DATA_DIR) + "/taxi.parquet";

TEST_CASE("Query aggregates NYC taxi parquet", "[bridge][query]") {
    DuckDbBridge db;  // in-memory engine; reads the parquet file directly
    // Cast the sum to DECIMAL so its textual form is deterministic.
    auto r = db.Query(
        "SELECT payment_type, count(*) AS c, "
        "CAST(sum(fare_amount) AS DECIMAL(10,2)) AS s "
        "FROM read_parquet('" + TAXI + "') "
        "GROUP BY 1 ORDER BY 1");

    REQUIRE(r.row_count == 2);
    REQUIRE(r.columns.size() == 3);
    REQUIRE(r.columns[0].name == "payment_type");
    REQUIRE(r.columns[1].name == "c");
    // CARD: 4 trips, 12.5+22.0+18.0+33.5 = 86.00 ; CASH: 2 trips, 7.0+5.5 = 12.50
    REQUIRE(r.rows[0] == R"({"payment_type":"CARD","c":4,"s":86.00})");
    REQUIRE(r.rows[1] == R"({"payment_type":"CASH","c":2,"s":12.50})");
}

TEST_CASE("Bridge turns on HTTP caching for fast multi-file remote reads", "[bridge][httpfs]") {
    // Regression guard: with DuckDB's default httpfs_connection_caching=false and no
    // HTTP metadata cache, a read_parquet over a long LIST of remote files opens a
    // fresh TLS connection per range request — a handshake storm that hangs the
    // server (and the synchronous SAP GUI) for minutes, while the duckdb CLI runs
    // the same query in seconds. The bridge must enable HTTP metadata caching at
    // construction (and, when httpfs is available, connection caching).
    DuckDbBridge db;  // in-memory; ctor runs the global HTTP-cache config

    auto meta = db.Query("SELECT CAST(current_setting('enable_http_metadata_cache') AS VARCHAR) AS v");
    REQUIRE(meta.row_count == 1);
    REQUIRE(meta.rows[0] == R"({"v":"true"})");

    // Connection caching only exists once httpfs is loaded; assert it only when the
    // ctor managed to load httpfs from the local extension cache (else skip — an
    // air-gapped box without a cached httpfs legitimately won't have it).
    auto loaded = db.Query(
        "SELECT count(*) AS c FROM duckdb_extensions() WHERE extension_name='httpfs' AND loaded");
    if (loaded.rows.size() == 1 && loaded.rows[0] == R"({"c":1})") {
        auto cc = db.Query("SELECT CAST(current_setting('httpfs_connection_caching') AS VARCHAR) AS v");
        REQUIRE(cc.rows[0] == R"({"v":"true"})");
    }
}

TEST_CASE("JSON row parser handles strings, numbers, bool, null", "[json]") {
    auto rows = json::ParseRows(
        R"([{"id":1,"name":"a","flag":true,"note":null},{"id":2,"name":"b\"x"}])");
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].size() == 4);
    REQUIRE(rows[0][0].key == "id");
    REQUIRE(rows[0][0].value == "1");
    REQUIRE(rows[0][0].is_string == false);
    REQUIRE(rows[0][1].value == "a");
    REQUIRE(rows[0][1].is_string == true);
    REQUIRE(rows[0][2].value == "true");
    REQUIRE(rows[0][3].is_null == true);
    REQUIRE(rows[1][1].value == "b\"x");   // unescaped
}

TEST_CASE("Ingest INSERT writes rows and parquet", "[bridge][ingest]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE sales(id INTEGER PRIMARY KEY, region VARCHAR, amount DOUBLE)");

    std::string out = TmpPath("erpl_rev_test_insert.parquet");
    auto n = db.Ingest("sales",
                       R"([{"id":1,"region":"EU","amount":10.0},
                           {"id":2,"region":"US","amount":20.5}])",
                       IngestMode::Insert, {}, out);
    REQUIRE(n == 2);

    auto r = db.Query("SELECT count(*) AS c, "
                      "CAST(sum(amount) AS DECIMAL(10,2)) AS s FROM sales");
    REQUIRE(r.rows[0] == R"({"c":2,"s":30.50})");

    // Parquet was written and is re-readable with the same content.
    auto p = db.Query("SELECT count(*) AS c FROM read_parquet('" + out + "')");
    REQUIRE(p.rows[0] == R"({"c":2})");
}

TEST_CASE("Ingest UPSERT updates existing key", "[bridge][ingest][upsert]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE inv(sku VARCHAR PRIMARY KEY, qty INTEGER)");

    // First load: two new rows.
    auto n1 = db.Ingest("inv",
                        R"([{"sku":"A","qty":5},{"sku":"B","qty":3}])",
                        IngestMode::Upsert, {"sku"}, "");
    REQUIRE(n1 == 2);

    // Second load: A updated, C inserted.
    auto n2 = db.Ingest("inv",
                        R"([{"sku":"A","qty":99},{"sku":"C","qty":7}])",
                        IngestMode::Upsert, {"sku"}, "");
    REQUIRE(n2 == 2);

    auto r = db.Query("SELECT sku, qty FROM inv ORDER BY sku");
    REQUIRE(r.row_count == 3);
    REQUIRE(r.rows[0] == R"({"sku":"A","qty":99})");  // updated, not duplicated
    REQUIRE(r.rows[1] == R"({"sku":"B","qty":3})");
    REQUIRE(r.rows[2] == R"({"sku":"C","qty":7})");
}

TEST_CASE("Query runs a multi-statement script, returns last SELECT", "[bridge][query][multi]") {
    DuckDbBridge db;
    // A ';'-separated script: DDL + INSERT (no result sets) then two SELECTs.
    // The bridge must execute every statement and return ONLY the last result.
    auto r = db.Query(
        "CREATE TABLE t(id INTEGER, v VARCHAR);"
        "INSERT INTO t VALUES (1,'a'),(2,'b');"
        "SELECT count(*) AS n FROM t;"
        "SELECT id, v FROM t ORDER BY id;");

    REQUIRE(r.row_count == 2);
    REQUIRE(r.columns.size() == 2);
    REQUIRE(r.columns[0].name == "id");
    REQUIRE(r.columns[1].name == "v");
    REQUIRE(r.rows[0] == R"({"id":1,"v":"a"})");
    REQUIRE(r.rows[1] == R"({"id":2,"v":"b"})");
}

TEST_CASE("Query script with no trailing result set returns empty", "[bridge][query][multi]") {
    DuckDbBridge db;
    auto r = db.Query("CREATE TABLE t2(id INTEGER); INSERT INTO t2 VALUES (1);");
    REQUIRE(r.row_count == 0);
    REQUIRE(r.columns.empty());
    REQUIRE(r.rows.empty());
}

TEST_CASE("Bridge applies bulk-load engine config (GLOBAL)", "[bridge][config]") {
    DuckDbBridge db;   // a fresh per-op connection must inherit the GLOBAL setting
    REQUIRE(db.Query("SELECT current_setting('preserve_insertion_order') AS v").rows[0]
            == R"({"v":false})");
}

TEST_CASE("Bridge runs boot init SQL (external-target setup hook)", "[bridge][config][init]") {
    // init_sql is where ATTACH/CREATE SECRET/INSTALL run at startup. Prove arbitrary
    // multi-statement boot SQL executes and is visible to later per-op connections.
    DuckDbBridge db("", "CREATE TABLE boot(i INTEGER); INSERT INTO boot VALUES (42),(7);");
    REQUIRE(db.Query("SELECT sum(i) AS s FROM boot").rows[0] == R"({"s":49})");
    // A failing boot init must surface as a startup error, not be swallowed.
    REQUIRE_THROWS(DuckDbBridge("", "SELECT * FROM no_such_table_at_boot"));
}

// The stage-then-publish sink: replication fills a local DuckDB table, then ONE
// DuckDB statement materializes it to the external target. These exercise the
// exact SQL shapes zcl_erpl_rev_util=>publish emits, on the real engine.
TEST_CASE("Publish: local table -> parquet (single file + partitioned)", "[bridge][publish][parquet]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE holding AS SELECT i AS id, 'v'||(i%3) AS grp FROM range(1000) t(i)");

    const std::string file = TmpPath("erpl_pub_single.parquet");
    db.Execute("COPY (SELECT * FROM holding) TO '" + file + "' (FORMAT parquet)");
    REQUIRE(db.Query("SELECT count(*) AS c FROM read_parquet('" + file + "')").rows[0]
            == R"({"c":1000})");

    // partitioned dataset: a directory of grp=… parts, re-readable as one relation.
    const std::string dir = TmpPath("erpl_pub_ds");
    db.Execute("COPY (SELECT * FROM holding) TO '" + dir +
               "' (FORMAT parquet, PARTITION_BY (grp), OVERWRITE_OR_IGNORE 1)");
    REQUIRE(db.Query("SELECT count(*) AS c FROM read_parquet('" + dir +
                     "/**/*.parquet')").rows[0] == R"({"c":1000})");
    REQUIRE(db.Query("SELECT count(DISTINCT grp) AS c FROM read_parquet('" + dir +
                     "/**/*.parquet')").rows[0] == R"({"c":3})");
}

TEST_CASE("Publish: local table -> attached catalog (CTAS)", "[bridge][publish][attach]") {
    // A second DuckDB file is a faithful proxy for postgres/ducklake/bigquery: the
    // CREATE TABLE <cat>.<schema>.<tbl> AS SELECT path is identical across catalogs.
    const std::string ext = TmpPath("erpl_pub_ext.duckdb");
    std::remove(ext.c_str()); std::remove((ext + ".wal").c_str());
    DuckDbBridge db("", "ATTACH '" + ext + "' AS extdb;");   // boot-time ATTACH, as in production
    db.Execute("CREATE TABLE holding AS SELECT i AS id, i*2 AS dbl FROM range(500) t(i)");

    // FULL publish: DROP + CTAS into the attached catalog.
    db.Execute("DROP TABLE IF EXISTS extdb.main.t; CREATE TABLE extdb.main.t AS SELECT * FROM holding");
    REQUIRE(db.Query("SELECT count(*) AS c, sum(dbl) AS s FROM extdb.main.t").rows[0]
            == R"({"c":500,"s":249500})");

    // APPEND publish: INSERT INTO … SELECT doubles the row count.
    db.Execute("INSERT INTO extdb.main.t SELECT * FROM holding");
    REQUIRE(db.Query("SELECT count(*) AS c FROM extdb.main.t").rows[0] == R"({"c":1000})");
}

TEST_CASE("Publish: DuckLake catalog if the extension is available", "[bridge][publish][ducklake]") {
    // DuckLake is the most write-complete lakehouse target. The extension may be
    // absent offline — SKIP loudly rather than fail.
    std::unique_ptr<DuckDbBridge> db;
    const std::string meta = TmpPath("erpl_pub_lake.ducklake");
    const std::string data = TmpPath("erpl_pub_lake_files");
    std::remove(meta.c_str());
    try {
        db = std::make_unique<DuckDbBridge>(
            "", "INSTALL ducklake; LOAD ducklake; ATTACH 'ducklake:" + meta +
                "' AS lake (DATA_PATH '" + data + "');");
    } catch (const std::exception &e) {
        SKIP(std::string("ducklake unavailable: ") + e.what());
    }
    db->Execute("CREATE TABLE holding AS SELECT i AS id FROM range(250) t(i)");
    db->Execute("CREATE TABLE lake.t AS SELECT * FROM holding");
    REQUIRE(db->Query("SELECT count(*) AS c FROM lake.t").rows[0] == R"({"c":250})");
}

TEST_CASE("Query streams + caps without draining the whole result", "[bridge][query][stream]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE big AS SELECT i AS id FROM range(1000000) t(i)");
    // Capped: ships exactly the cap, flags truncated, and reports a cap+1 sentinel
    // (NOT the true 1,000,000) — i.e. it stops streaming, it does not drain/count all.
    auto r = db.Query("SELECT id FROM big ORDER BY id", 10);
    REQUIRE(r.rows.size() == 10);
    REQUIRE(r.truncated);
    REQUIRE(r.row_count == 11);                       // cap+1, not 1e6
    REQUIRE(r.rows[0] == R"({"id":0})");
    // want_total: drain to the exact count.
    auto rt = db.Query("SELECT id FROM big", 10, /*want_total=*/true);
    REQUIRE(rt.rows.size() == 10);
    REQUIRE(rt.truncated);
    REQUIRE(rt.row_count == 1000000);
    // Uncapped small result: exact count, not truncated (unchanged behavior).
    auto s = db.Query("SELECT id FROM big WHERE id < 3 ORDER BY id");
    REQUIRE(s.rows.size() == 3);
    REQUIRE_FALSE(s.truncated);
    REQUIRE(s.row_count == 3);
}

TEST_CASE("Quack serves this in-process DuckDB to a remote client", "[bridge][quack]") {
    DuckDbBridge db;  // in-memory; quack exposes exactly this instance
    db.Execute("CREATE TABLE t(id INTEGER, v VARCHAR)");
    db.Execute("INSERT INTO t VALUES (1,'hello'),(2,'world')");

    // StartQuack must INSTALL/LOAD the extension from the public repo; if that
    // is unavailable (offline, or engine < 1.5.3) skip loudly rather than fail.
    std::string details;
    try {
        details = db.StartQuack("quack:localhost", /*allow_other_host=*/false);
    } catch (const std::exception &e) {
        SKIP(std::string("quack extension unavailable: ") + e.what());
    }

    // quack_serve returns one row: listen_uri / listen_url / auth_token.
    auto meta = json::ParseRows(details);
    REQUIRE(meta.size() == 1);
    std::string token;
    for (auto &cell : meta[0]) if (cell.key == "auth_token") token = cell.value;
    REQUIRE_FALSE(token.empty());

    // Query the running server back over the loopback HTTP transport: a remote
    // DuckDB client sees the rows ingested into this process.
    auto r = db.Query(
        "FROM quack_query('quack:localhost', "
        "'SELECT count(*) AS n, max(v) AS mv FROM t', token = '" + token + "')");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(r.rows[0] == R"({"n":2,"mv":"world"})");

    db.StopQuack("quack:localhost");
}

TEST_CASE("Quack honours a pinned auth token", "[bridge][quack]") {
    DuckDbBridge db;
    const std::string pinned = "MYFIXEDTOKEN0123456789ABCDEF";
    std::string details;
    try {
        details = db.StartQuack("quack:localhost", /*allow_other_host=*/false, pinned);
    } catch (const std::exception &e) {
        SKIP(std::string("quack extension unavailable: ") + e.what());
    }

    // quack_serve echoes back the token we pinned (not a random one).
    auto meta = json::ParseRows(details);
    REQUIRE(meta.size() == 1);
    std::string token;
    for (auto &cell : meta[0]) if (cell.key == "auth_token") token = cell.value;
    REQUIRE(token == pinned);

    // And that exact token authenticates a query.
    auto r = db.Query(
        "FROM quack_query('quack:localhost', 'SELECT 7 AS v', token = '" + pinned + "')");
    REQUIRE(r.rows[0] == R"({"v":7})");

    db.StopQuack("quack:localhost");
}

TEST_CASE("Typed ingest: init_sql + ddl + UPSERT round-trips typed values", "[bridge][ingest][typed]") {
    DuckDbBridge db;
    // init_sql runs first (a harmless SET stands in for a LOAD), then ddl creates
    // a typed table (DATE + DECIMAL + PK), then the rows are upserted.
    std::string init = "SET threads TO 1;";
    std::string ddl  = "CREATE TABLE IF NOT EXISTS sap_t("
                       "id INTEGER PRIMARY KEY, d DATE, amt DECIMAL(10,2), name VARCHAR);";
    auto n = db.Ingest("sap_t",
                       R"([{"id":1,"d":"2024-01-15","amt":12.50,"name":"a"},
                           {"id":2,"d":"2024-02-20","amt":7.00,"name":"b"}])",
                       IngestMode::Upsert, {"id"}, "", init, ddl);
    REQUIRE(n == 2);

    // Columns keep their real DuckDB types; values cast from JSON correctly.
    auto r = db.Query("SELECT id, d, amt FROM sap_t ORDER BY id");
    REQUIRE(r.row_count == 2);
    REQUIRE(r.columns[1].type == "DATE");
    REQUIRE(r.columns[2].type == "DECIMAL(10,2)");
    REQUIRE(r.rows[0] == R"({"id":1,"d":"2024-01-15","amt":12.50})");

    // UPSERT again: update id=1, insert id=3 (no init/ddl needed second time).
    auto n2 = db.Ingest("sap_t",
                        R"([{"id":1,"d":"2024-03-01","amt":99.99,"name":"a2"},
                            {"id":3,"d":"2024-04-04","amt":1.00,"name":"c"}])",
                        IngestMode::Upsert, {"id"}, "", "", "");
    REQUIRE(n2 == 2);
    auto r2 = db.Query("SELECT amt FROM sap_t WHERE id=1");
    REQUIRE(r2.rows[0] == R"({"amt":99.99})");
}

// ---- Streaming cursors (fixed-memory paging) -------------------------------

TEST_CASE("Cursor: pages a large result vector-aligned, true total", "[cursor]") {
    DuckDbBridge db;
    auto open = db.OpenCursor("SELECT i AS id FROM range(5000) t(i)");
    REQUIRE(open.columns.size() == 1);
    // Column names are uppercased to bind to ABAP's always-uppercase components.
    REQUIRE(open.columns[0].name == "ID");

    long long total = 0;
    int pages = 0;
    bool done = false;
    long long last_fetched = 0;
    while (!done) {
        auto p = db.FetchCursor(open.handle, 2048);
        done = p.done;
        last_fetched = p.fetched;
        total += p.fetched;
        if (p.fetched > 0) {
            // page decodes to exactly p.fetched rows of one column
            auto t = sxml::Decode(p.bxml);
            REQUIRE(t.columns == std::vector<std::string>{"ID"});
            REQUIRE((long long)t.rows.size() == p.fetched);
            REQUIRE(p.fetched <= 2048);            // never exceeds the page target
            if (pages == 0) REQUIRE(t.rows[0][0] == "0");
        }
        pages++;
    }
    REQUIRE(total == 5000);          // every row delivered, exactly once
    REQUIRE(last_fetched == 904);    // 5000 - 2*2048
    db.CloseCursor(open.handle);
}

TEST_CASE("Cursor: multi-statement streams the final SELECT", "[cursor]") {
    DuckDbBridge db;
    auto open = db.OpenCursor(
        "CREATE TABLE ct AS SELECT i AS x FROM range(3) t(i); "
        "SELECT x FROM ct ORDER BY x");
    REQUIRE(open.columns.size() == 1);
    auto p = db.FetchCursor(open.handle, 2048);
    REQUIRE(p.fetched == 3);
    REQUIRE(p.done);
    auto t = sxml::Decode(p.bxml);
    REQUIRE(t.rows[0][0] == "0");
    REQUIRE(t.rows[2][0] == "2");
    db.CloseCursor(open.handle);
}

TEST_CASE("Cursor: non-result statement yields an empty done page", "[cursor]") {
    DuckDbBridge db;
    auto open = db.OpenCursor("CREATE TABLE e2(a INTEGER)");
    auto p = db.FetchCursor(open.handle, 2048);
    REQUIRE(p.fetched == 0);
    REQUIRE(p.done);
    REQUIRE(p.bxml.empty());
    db.CloseCursor(open.handle);
}

TEST_CASE("Cursor: unknown / closed handle throws", "[cursor]") {
    DuckDbBridge db;
    auto open = db.OpenCursor("SELECT 1 AS a");
    db.CloseCursor(open.handle);
    REQUIRE_THROWS(db.FetchCursor(open.handle, 100));
    REQUIRE_THROWS(db.FetchCursor("cur_nonexistent", 100));
}

TEST_CASE("Cursor prefetch: pages are contiguous, no gaps or dupes", "[cursor][prefetch]") {
    DuckDbBridge db;
    auto open = db.OpenCursor("SELECT i AS id FROM range(10000) t(i)");
    long long expect = 0;
    bool done = false;
    while (!done) {
        auto p = db.FetchCursor(open.handle, 1000);   // small pages -> many prefetches
        done = p.done;
        if (p.fetched > 0) {
            auto t = sxml::Decode(p.bxml);
            // every value in this page must continue the global sequence exactly
            for (auto &row : t.rows) {
                REQUIRE(row[0] == std::to_string(expect));
                expect++;
            }
        }
    }
    REQUIRE(expect == 10000);   // 0..9999 delivered once each, in order
    db.CloseCursor(open.handle);
}

TEST_CASE("Cursor prefetch: two interleaved cursors don't cross-talk", "[cursor][prefetch]") {
    DuckDbBridge db;
    auto a = db.OpenCursor("SELECT i AS id FROM range(3000) t(i)");
    auto b = db.OpenCursor("SELECT i*10 AS id FROM range(3000) t(i)");
    long long ea = 0, eb = 0;
    bool da = false, db_ = false;
    while (!da || !db_) {
        if (!da) {
            auto p = db.FetchCursor(a.handle, 512); da = p.done;
            if (p.fetched > 0)
                for (auto &r : sxml::Decode(p.bxml).rows) { REQUIRE(r[0] == std::to_string(ea)); ea++; }
        }
        if (!db_) {
            auto p = db.FetchCursor(b.handle, 512); db_ = p.done;
            if (p.fetched > 0)
                for (auto &r : sxml::Decode(p.bxml).rows) { REQUIRE(r[0] == std::to_string(eb*10)); eb++; }
        }
    }
    REQUIRE(ea == 3000);
    REQUIRE(eb == 3000);
    db.CloseCursor(a.handle);
    db.CloseCursor(b.handle);
}

TEST_CASE("Cursor prefetch: close mid-stream is clean", "[cursor][prefetch]") {
    DuckDbBridge db;
    auto open = db.OpenCursor("SELECT i AS id FROM range(100000) t(i)");
    auto p = db.FetchCursor(open.handle, 2048);   // leaves a prefetch in flight
    REQUIRE(p.fetched == 2048);
    REQUIRE_FALSE(p.done);
    db.CloseCursor(open.handle);                  // must join the in-flight prefetch safely
    SUCCEED();
}

TEST_CASE("IngestBxml: typed insert + UPSERT from binary sXML", "[bridge][ingest][bxml]") {
    DuckDbBridge db;
    // Rows arrive as a BXML page (as ABAP's cl_sxml_string_writer would produce);
    // all cells are strings and must cast to the typed DDL columns.
    sxml::Table t;
    t.columns = {"ID", "D", "AMT", "NAME"};
    t.rows = {{"1", "2024-01-15", "12.50", "alpha"},
              {"2", "2024-02-20", "7.00",  "bravo"}};
    std::string ddl = "CREATE TABLE IF NOT EXISTS r("
                      "id INTEGER PRIMARY KEY, d DATE, amt DECIMAL(10,2), name VARCHAR);";

    auto n = db.IngestBxml("r", sxml::Encode("DATA", t), IngestMode::Upsert,
                           {"id"}, "", "SET threads TO 1;", ddl);
    REQUIRE(n == 2);

    // Typed casts landed: DATE and DECIMAL are real types, not strings.
    auto q = db.Query("SELECT typeof(d) AS td, typeof(amt) AS ta FROM r LIMIT 1");
    // custom delimiter: the JSON contains )" which would close a default R"(...)"
    REQUIRE(q.rows[0] == R"json({"td":"DATE","ta":"DECIMAL(10,2)"})json");
    auto qs = db.Query("SELECT CAST(sum(amt) AS DECIMAL(10,2)) AS s FROM r");
    REQUIRE(qs.rows[0] == R"({"s":19.50})");

    // UPSERT updates the existing key from a second BXML page.
    sxml::Table u;
    u.columns = {"ID", "D", "AMT", "NAME"};
    u.rows = {{"1", "2024-01-15", "99.99", "ALPHA2"}};
    auto n2 = db.IngestBxml("r", sxml::Encode("DATA", u), IngestMode::Upsert,
                            {"id"}, "", "", "");
    REQUIRE(n2 == 1);
    auto r2 = db.Query("SELECT name FROM r WHERE id=1");
    REQUIRE(r2.rows[0] == R"({"name":"ALPHA2"})");
    auto r3 = db.Query("SELECT count(*) AS c FROM r");
    REQUIRE(r3.rows[0] == R"({"c":2})");   // upsert, not duplicate
}

TEST_CASE("IngestBxml: binary/NUL cells land in a BLOB column", "[bridge][ingest][bxml][blob]") {
    DuckDbBridge db;
    // A RAW column (DDIC RAW/xstring -> DuckDB BLOB). The cell carries raw bytes
    // including embedded NUL and a stray 0xFF (invalid UTF-8) — exactly what an
    // initial/binary ABAP RAW field produces. SqlQuote would yield an
    // "unterminated quoted string"; the bridge must route it through from_hex.
    std::string raw16(16, '\0');                            // 16 NUL bytes (initial RAW(16))
    std::string mixed = std::string("\x01\x00\xFF\x41", 4); // 01 00 FF 41
    sxml::Table t;
    t.columns = {"ID", "B"};
    t.rows = {{"1", raw16}, {"2", mixed}};
    std::string ddl = "CREATE TABLE IF NOT EXISTS rb("
                      "id INTEGER PRIMARY KEY, b BLOB);";

    auto n = db.IngestBxml("rb", sxml::Encode("DATA", t), IngestMode::Upsert,
                           {"id"}, "", "SET threads TO 1;", ddl);
    REQUIRE(n == 2);

    // Stored as a real BLOB, with the exact bytes (length + hex round-trip).
    auto ty = db.Query("SELECT typeof(b) AS t FROM rb WHERE id=1");
    REQUIRE(ty.rows[0] == R"({"t":"BLOB"})");
    auto l1 = db.Query("SELECT octet_length(b) AS n FROM rb WHERE id=1");
    REQUIRE(l1.rows[0] == R"({"n":16})");
    auto h2 = db.Query("SELECT hex(b) AS h FROM rb WHERE id=2");
    REQUIRE(h2.rows[0] == R"({"h":"0100FF41"})");
}

TEST_CASE("IngestBxml: empty SAP date (0000-00-00) becomes NULL", "[bridge][ingest][bxml][date]") {
    DuckDbBridge db;
    // `CALL TRANSFORMATION id` renders an initial DATS as "0000-00-00", which
    // DuckDB rejects as a DATE. It must land as NULL, while a real date casts.
    sxml::Table t;
    t.columns = {"ID", "D"};
    t.rows = {{"1", "0000-00-00"}, {"2", "2026-05-31"}};
    std::string ddl = "CREATE TABLE IF NOT EXISTS rd(id INTEGER PRIMARY KEY, d DATE);";
    auto n = db.IngestBxml("rd", sxml::Encode("DATA", t), IngestMode::Upsert,
                           {"id"}, "", "SET threads TO 1;", ddl);
    REQUIRE(n == 2);
    auto a = db.Query("SELECT count(*) AS c FROM rd WHERE d IS NULL");
    REQUIRE(a.rows[0] == R"({"c":1})");
    auto b = db.Query("SELECT CAST(d AS VARCHAR) AS d FROM rd WHERE id=2");
    REQUIRE(b.rows[0] == R"({"d":"2026-05-31"})");
}

TEST_CASE("IngestBxml: blank SAP date (spaces) becomes NULL", "[bridge][ingest][bxml][date]") {
    DuckDbBridge db;
    // Some tables (e.g. REPOSRC) hold a blank DATS — 8 spaces — which `CALL
    // TRANSFORMATION id` renders as "    -  -  " (dashes, space digits), NOT
    // "0000-00-00". That also has no valid DATE; it must land as NULL, not crash
    // the package with "invalid date field format".
    sxml::Table t;
    t.columns = {"ID", "D"};
    t.rows = {{"1", "    -  -  "}, {"2", ""}, {"3", "2026-05-31"}};
    std::string ddl = "CREATE TABLE IF NOT EXISTS rdb(id INTEGER PRIMARY KEY, d DATE);";
    auto n = db.IngestBxml("rdb", sxml::Encode("DATA", t), IngestMode::Upsert,
                           {"id"}, "", "SET threads TO 1;", ddl);
    REQUIRE(n == 3);
    auto a = db.Query("SELECT count(*) AS c FROM rdb WHERE d IS NULL");
    REQUIRE(a.rows[0] == R"({"c":2})");          // blank + empty both NULL
    auto b = db.Query("SELECT CAST(d AS VARCHAR) AS d FROM rdb WHERE id=3");
    REQUIRE(b.rows[0] == R"({"d":"2026-05-31"})");
}

TEST_CASE("IngestBxml: large batch loads fast and correct", "[bridge][ingest][bxml][bulk]") {
    DuckDbBridge db;
    // A realistic-sized package through the bulk path: typed casts still hold and
    // every row lands. (The bulk loader must not regress correctness vs per-row.)
    const int N = 50000;
    sxml::Table t;
    t.columns = {"ID", "D", "AMT", "NAME"};
    t.rows.reserve(N);
    for (int i = 0; i < N; i++)
        t.rows.push_back({std::to_string(i), "2026-06-01",
                          std::to_string(i % 1000) + ".50", "row_" + std::to_string(i)});
    std::string ddl = "CREATE TABLE IF NOT EXISTS big("
                      "id INTEGER PRIMARY KEY, d DATE, amt DECIMAL(13,2), name VARCHAR);";
    auto n = db.IngestBxml("big", sxml::Encode("DATA", t), IngestMode::Insert,
                           {}, "", "SET threads TO 1;", ddl);
    REQUIRE(n == N);
    auto c = db.Query("SELECT count(*) AS c FROM big");
    REQUIRE(c.rows[0] == R"({"c":50000})");
    auto ty = db.Query("SELECT typeof(d) AS d, typeof(amt) AS a FROM big LIMIT 1");
    REQUIRE(ty.rows[0] == R"json({"d":"DATE","a":"DECIMAL(13,2)"})json");
    auto last = db.Query("SELECT name FROM big WHERE id=49999");
    REQUIRE(last.rows[0] == R"({"name":"row_49999"})");
}

TEST_CASE("IngestBxml: a bad cast rolls back the whole package", "[bridge][ingest][bxml][atomic]") {
    DuckDbBridge db;
    // One un-castable date in the package must abort the package atomically and
    // leave the target empty — NOT commit the rows before it (the per-row loop
    // would have left row 1 behind). This is the atomicity guarantee.
    sxml::Table t;
    t.columns = {"ID", "D"};
    t.rows = {{"1", "2024-01-01"}, {"2", "not-a-date"}, {"3", "2024-03-03"}};
    std::string ddl = "CREATE TABLE IF NOT EXISTS atom(id INTEGER, d DATE);";
    REQUIRE_THROWS(db.IngestBxml("atom", sxml::Encode("DATA", t), IngestMode::Insert,
                                 {}, "", "SET threads TO 1;", ddl));
    auto c = db.Query("SELECT count(*) AS c FROM atom");
    REQUIRE(c.rows[0] == R"({"c":0})");   // nothing committed
}

// ---- Defer-PK full-load: load a heap table, then ADD PRIMARY KEY once ---------
// Validates the DuckDB mechanic behind the replication optimization: building the
// ART index ONCE at the end (vs per-package) — and what NOT-NULL/uniqueness it
// requires. (composite key, nullable heap columns up front.)
TEST_CASE("Defer PK: bulk-load heap then ALTER ADD PRIMARY KEY", "[bridge][ddl][pk]") {
    DuckDbBridge db;
    // heap: NO primary key, columns nullable (as a deferred-PK full-load target is)
    db.Execute("CREATE TABLE t (bukrs VARCHAR, belnr VARCHAR, v INTEGER)");
    db.Execute("INSERT INTO t VALUES ('1000','0001',10),('1000','0002',20),('2000','0001',30)");
    // Build the index once over the full, unique-on-key data.
    db.Execute("ALTER TABLE t ADD PRIMARY KEY (bukrs, belnr)");
    // PK is now enforced: a duplicate key is rejected.
    REQUIRE_THROWS(db.Execute("INSERT INTO t VALUES ('1000','0001',99)"));
    auto c = db.Query("SELECT count(*) AS c FROM t");
    REQUIRE(c.rows[0] == R"({"c":3})");
    // typeof/constraint sanity: a fresh non-dup insert still works.
    db.Execute("INSERT INTO t VALUES ('3000','0001',40)");
    REQUIRE(db.Query("SELECT count(*) AS c FROM t").rows[0] == R"({"c":4})");
}

// M2: concurrency. Many threads ingest disjoint key ranges into ONE target while
// other threads query it — all on a single bridge with NO external lock (each op
// opens its own connection; TEMP staging is connection-local). Asserts every row
// lands exactly once and nothing crashes.
TEST_CASE("Concurrent ingest + query on one bridge (per-connection, no lock)", "[bridge][concurrency]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE t (id INTEGER, v VARCHAR)");   // heap (full-load style)
    const int kWorkers = 8, kPer = 5000;                    // 40,000 disjoint rows
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int w = 0; w < kWorkers; w++) {
        ts.emplace_back([&, w] {
            try {
                sxml::Table tbl;
                tbl.columns = {"ID", "V"};
                for (int i = 0; i < kPer; i++) {
                    int id = w * kPer + i;
                    tbl.rows.push_back({std::to_string(id), "v" + std::to_string(id)});
                }
                db.IngestBxml("t", sxml::Encode("DATA", tbl), IngestMode::Insert, {}, "");
            } catch (...) { errors++; }
        });
    }
    for (int q = 0; q < 4; q++)   // concurrent readers (must not block / crash)
        ts.emplace_back([&] {
            try { for (int k = 0; k < 25; k++) (void)db.Query("SELECT count(*) AS c FROM t"); }
            catch (...) { errors++; }
        });
    for (auto &th : ts) th.join();

    REQUIRE(errors.load() == 0);
    REQUIRE(db.Query("SELECT count(*) AS c FROM t").rows[0] == R"({"c":40000})");          // none lost
    REQUIRE(db.Query("SELECT count(DISTINCT id) AS d FROM t").rows[0] == R"({"d":40000})"); // none doubled
}

TEST_CASE("Cursor: expression column names sanitized for ABAP", "[cursor][sanitize]") {
    DuckDbBridge db;
    // count(*) -> DuckDB "count_star()"; the '(' ')' '*' are invalid ABAP
    // component-name chars and crashed cl_abap_structdescr. Must be sanitized.
    auto open = db.OpenCursor("SELECT count(*) FROM range(5)");
    REQUIRE(open.columns.size() == 1);
    const std::string &nm = open.columns[0].name;
    for (char c : nm)
        REQUIRE(((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'));
    REQUIRE(nm[0] != '_');                       // valid identifier start
    auto p = db.FetchCursor(open.handle, 2048);
    auto t = sxml::Decode(p.bxml);
    REQUIRE(t.columns[0] == nm);                 // BXML element matches EV_COLUMNS
    REQUIRE(t.rows[0][0] == "5");
    db.CloseCursor(open.handle);
}

TEST_CASE("Cursor: duplicate sanitized names are de-duplicated", "[cursor][sanitize]") {
    DuckDbBridge db;
    // both expressions sanitize toward the same base -> must stay distinct.
    auto open = db.OpenCursor("SELECT 1 AS \"a(*)\", 2 AS \"a[*]\"");
    REQUIRE(open.columns.size() == 2);
    REQUIRE(open.columns[0].name != open.columns[1].name);
    db.CloseCursor(open.handle);
}

TEST_CASE("Cursor: realistic expression column names all sanitize valid", "[cursor][sanitize]") {
    DuckDbBridge db;
    // A spread of the column names a console user actually generates.
    auto open = db.OpenCursor(
        "SELECT count(*), 1+2, upper('a'), "
        "length('the quick brown fox jumps over the lazy dog')*1000 AS \"x\", "
        "CASE WHEN 1=1 THEN 'y' END");
    REQUIRE(open.columns.size() == 5);
    for (auto &c : open.columns) {
        REQUIRE_FALSE(c.name.empty());
        REQUIRE(c.name.size() <= 30);                 // ABAP component-name limit
        REQUIRE(((c.name[0] >= 'A' && c.name[0] <= 'Z') || c.name[0] == '_'));
        for (char ch : c.name)
            REQUIRE(((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'));
    }
    // names are unique (so the ABAP structure build won't collide)
    for (size_t i = 0; i < open.columns.size(); i++)
        for (size_t j = i + 1; j < open.columns.size(); j++)
            REQUIRE(open.columns[i].name != open.columns[j].name);
    auto p = db.FetchCursor(open.handle, 2048);
    REQUIRE(p.fetched == 1);
    db.CloseCursor(open.handle);
}

// ---------------------------------------------------------------------------
// Delta (incremental) extraction — server merge engine + state table.
// The ABAP delta readers stream a change package whose payload carries an op
// column (I/U/D) and call IngestBxml in MERGE mode; deletes-without-a-change-
// column use SnapshotMerge. Delta config + runtime state live in the DuckDB
// table _erpl_rev_delta_state, created once at boot. (See docs/delta.md.)
// ---------------------------------------------------------------------------

TEST_CASE("Delta: _erpl_rev_delta_state registry exists at boot", "[bridge][delta][state]") {
    DuckDbBridge db;
    // Present, empty, queryable through the same path ABAP uses (Z_DUCKDB_QUERY).
    auto q = db.Query("SELECT count(*) AS c FROM _erpl_rev_delta_state");
    REQUIRE(q.rows[0] == R"({"c":0})");
    // The column set + defaults match the HLD schema: seed a target like ABAP would.
    db.Execute("INSERT INTO _erpl_rev_delta_state(target,method,source_from,keys) "
               "VALUES ('delta_wm','WATERMARK','ZDELTA_WM','id')");
    auto r = db.Query("SELECT method, safety_secs, cadence, status "
                      "FROM _erpl_rev_delta_state WHERE target='delta_wm'");
    REQUIRE(r.rows[0] ==
            R"({"method":"WATERMARK","safety_secs":120,"cadence":"nightly","status":"IDLE"})");
}

// ---------------------------------------------------------------------------
// Replication run statistics: _erpl_rev_run_stats (+ erpl_rev_run_stats view),
// created at boot, one row per full/incremental run. (See docs/stats.md.)
// ---------------------------------------------------------------------------

TEST_CASE("Stats: _erpl_rev_run_stats table + view exist at boot", "[bridge][stats]") {
    DuckDbBridge db;
    // Present + empty, queryable through the same path ABAP uses (Z_DUCKDB_QUERY).
    REQUIRE(db.Query("SELECT count(*) AS c FROM _erpl_rev_run_stats").rows[0] == R"({"c":0})");
    REQUIRE(db.Query("SELECT count(*) AS c FROM erpl_rev_run_stats").rows[0] == R"({"c":0})");
}

TEST_CASE("Stats: recorded run surfaces derived columns in the view", "[bridge][stats][view]") {
    DuckDbBridge db;
    // A full-load run as ABAP's record_run would write it (run_id + ts default).
    db.Execute("INSERT INTO _erpl_rev_run_stats"
               "(target,source,run_type,method,status,duration_ms,rows_read,rows_ins,rows_upd,rows_del,jobs) "
               "VALUES ('mara','MARA','FULL','FULL','SUCCESS',2000,1000,1000,0,0,1)");
    // A delta SNAPSHOT cycle with a physical delete.
    db.Execute("INSERT INTO _erpl_rev_run_stats"
               "(target,source,run_type,method,status,duration_ms,rows_read,rows_ins,rows_upd,rows_del,jobs) "
               "VALUES ('mara','MARA','DELTA','SNAPSHOT','SUCCESS',500,3,1,1,1,1)");
    // The view derives rows_applied, rows_per_sec, is_success — dashboard-ready.
    auto r = db.Query("SELECT run_type, rows_applied, CAST(rows_per_sec AS BIGINT) AS rps, is_success "
                      "FROM erpl_rev_run_stats ORDER BY run_id");
    REQUIRE(r.rows[0] == R"({"run_type":"FULL","rows_applied":1000,"rps":500,"is_success":true})");
    REQUIRE(r.rows[1] == R"({"run_type":"DELTA","rows_applied":3,"rps":6,"is_success":true})");
    // run_id is sequence-assigned and monotonic; started_at = finished_at - duration.
    auto m = db.Query("SELECT count(*) AS c FROM erpl_rev_run_stats "
                      "WHERE started_at <= finished_at AND run_id >= 1");
    REQUIRE(m.rows[0] == R"({"c":2})");
}

TEST_CASE("Stats: an ERROR run is recorded as not-successful", "[bridge][stats][error]") {
    DuckDbBridge db;
    db.Execute("INSERT INTO _erpl_rev_run_stats"
               "(target,source,run_type,method,status,duration_ms,error_text) "
               "VALUES ('mara','MARA','DELTA','WATERMARK','ERROR',10,'cast failed')");
    auto r = db.Query("SELECT status, is_success, error_text FROM erpl_rev_run_stats");
    REQUIRE(r.rows[0] == R"({"status":"ERROR","is_success":false,"error_text":"cast failed"})");
}

// ---------------------------------------------------------------------------
// Trigger-CDC state machine: _erpl_rev_cdc + guarded transitions. (Epic #17.)
// ---------------------------------------------------------------------------

TEST_CASE("CDC state: table at boot, register, get", "[bridge][cdc][state]") {
    DuckDbBridge db;
    REQUIRE(db.Query("SELECT count(*) AS c FROM _erpl_rev_cdc").rows[0] == R"({"c":0})");
    REQUIRE_FALSE(db.CdcGet("sflight").exists);

    db.CdcRegister("sflight", "SFLIGHT", "MANDT,CARRID,CONNID,FLDATE", "HANA",
                   "DELETE_ONLY", "ZCDC_SFLIGHT_LOG");
    CdcState s = db.CdcGet("sflight");
    REQUIRE(s.exists);
    REQUIRE(s.source == "SFLIGHT");
    REQUIRE(s.keys == "MANDT,CARRID,CONNID,FLDATE");
    REQUIRE(s.platform == "HANA");
    REQUIRE(s.mode == "DELETE_ONLY");
    REQUIRE(s.log_table == "ZCDC_SFLIGHT_LOG");
    REQUIRE(s.status == "PROVISIONED");
    REQUIRE(s.position == 0);
}

TEST_CASE("CDC state: legal transitions + re-enable", "[bridge][cdc][state]") {
    DuckDbBridge db;
    db.CdcRegister("t", "T", "K", "HANA", "DELETE_ONLY", "ZCDC_T_LOG");
    db.CdcSetStatus("t", "SEEDED");
    REQUIRE(db.CdcGet("t").status == "SEEDED");
    db.CdcSetStatus("t", "ACTIVE");
    db.CdcSetStatus("t", "ACTIVE");   // re-run, idempotent-ish
    REQUIRE(db.CdcGet("t").status == "ACTIVE");
    db.CdcSetStatus("t", "DISABLED");
    REQUIRE(db.CdcGet("t").status == "DISABLED");
    // re-enable goes back through register -> PROVISIONED.
    db.CdcRegister("t", "T", "K", "HANA", "DELETE_ONLY", "ZCDC_T_LOG");
    REQUIRE(db.CdcGet("t").status == "PROVISIONED");
}

TEST_CASE("CDC state: illegal transitions are rejected", "[bridge][cdc][state]") {
    DuckDbBridge db;
    db.CdcRegister("t", "T", "K", "HANA", "DELETE_ONLY", "ZCDC_T_LOG");
    REQUIRE_THROWS(db.CdcSetStatus("t", "ACTIVE"));      // must SEED first
    db.CdcSetStatus("t", "SEEDED");
    REQUIRE_THROWS(db.CdcSetStatus("t", "PROVISIONED")); // only DISABLED->PROVISIONED
    REQUIRE_THROWS(db.CdcSetStatus("nope", "SEEDED"));   // unknown target
}

TEST_CASE("CDC state: position is monotonic", "[bridge][cdc][state]") {
    DuckDbBridge db;
    db.CdcRegister("t", "T", "K", "HANA", "DELETE_ONLY", "ZCDC_T_LOG");
    db.CdcAdvancePosition("t", 5);
    db.CdcAdvancePosition("t", 10);
    db.CdcAdvancePosition("t", 10);   // equal is fine (idempotent re-apply)
    REQUIRE(db.CdcGet("t").position == 10);
    REQUIRE_THROWS(db.CdcAdvancePosition("t", 3));   // regression rejected
    REQUIRE(db.CdcGet("t").position == 10);
}

TEST_CASE("CDC state: survives a restart (file-backed)", "[bridge][cdc][state]") {
    const std::string path = TmpPath("erpl_cdc_state_test.duckdb");
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
    {
        DuckDbBridge db(path);
        db.CdcRegister("t", "T", "K", "HANA", "FULL_IUD", "ZCDC_T_LOG");
        db.CdcSetStatus("t", "SEEDED");
        db.CdcAdvancePosition("t", 42);
    }
    {
        DuckDbBridge db(path);   // reopen the same store
        CdcState s = db.CdcGet("t");
        REQUIRE(s.exists);
        REQUIRE(s.status == "SEEDED");
        REQUIRE(s.position == 42);
        REQUIRE(s.mode == "FULL_IUD");
    }
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
}

TEST_CASE("CDC apply: delete-only batch removes keys + advances position", "[bridge][cdc][apply]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE t1(id INTEGER PRIMARY KEY, v VARCHAR)");
    db.Execute("INSERT INTO t1 VALUES (1,'a'),(2,'b'),(3,'c')");
    db.CdcRegister("t1", "T1", "id", "HANA", "DELETE_ONLY", "ZCDC_T1_LOG");
    db.CdcSetStatus("t1", "SEEDED");
    // delete-only staging carries keys + op + seq only.
    db.Execute("CREATE TABLE log1(id INTEGER, \"_op\" VARCHAR, \"_seq\" BIGINT)");
    db.Execute("INSERT INTO log1 VALUES (1,'D',1),(3,'D',2)");

    CdcApplyResult r = db.CdcApply("t1", "log1", {"id"});
    REQUIRE(r.applied);
    REQUIRE(r.del == 2);
    REQUIRE(r.ins == 0);
    REQUIRE(r.upd == 0);
    REQUIRE(r.prune_bound == 2);
    REQUIRE(db.Query("SELECT count(*) AS c FROM t1").rows[0] == R"({"c":1})");
    REQUIRE(db.Query("SELECT v FROM t1").rows[0] == R"({"v":"b"})");
    REQUIRE(db.CdcGet("t1").position == 2);
    REQUIRE(db.CdcGet("t1").status == "ACTIVE");
    REQUIRE(db.Query("SELECT count(*) AS c FROM duckdb_tables() WHERE table_name='log1'").rows[0]
            == R"({"c":0})");   // staging dropped
}

TEST_CASE("CDC apply: coalesces interleaved I/U/D per key to the net op", "[bridge][cdc][apply]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE t2(id INTEGER PRIMARY KEY, v VARCHAR)");
    db.Execute("INSERT INTO t2 VALUES (2,'b'),(3,'c')");   // id1 absent; id2,id3 present
    db.CdcRegister("t2", "T2", "id", "HANA", "FULL_IUD", "ZCDC_T2_LOG");
    db.CdcSetStatus("t2", "SEEDED");
    // full-IUD staging carries the row image. id1: D then I -> net I; id2: U; id3: I then D -> net D.
    db.Execute("CREATE TABLE log2(id INTEGER, v VARCHAR, \"_op\" VARCHAR, \"_seq\" BIGINT)");
    db.Execute("INSERT INTO log2 VALUES "
               "(1,NULL,'D',1),(1,'x','I',2),"
               "(2,'B','U',3),"
               "(3,'c','I',4),(3,NULL,'D',5)");

    CdcApplyResult r = db.CdcApply("t2", "log2", {"id"});
    REQUIRE(r.ins == 1);   // id1 new
    REQUIRE(r.upd == 1);   // id2 existing
    REQUIRE(r.del == 1);   // id3 net delete (was present)
    REQUIRE(r.prune_bound == 5);
    REQUIRE(db.Query("SELECT count(*) AS c FROM t2").rows[0] == R"({"c":2})");
    REQUIRE(db.Query("SELECT v FROM t2 WHERE id=1").rows[0] == R"({"v":"x"})");
    REQUIRE(db.Query("SELECT v FROM t2 WHERE id=2").rows[0] == R"({"v":"B"})");
    REQUIRE(db.Query("SELECT count(*) AS c FROM t2 WHERE id=3").rows[0] == R"({"c":0})");
}

TEST_CASE("CDC apply: SAP-typed keys (NUMC + DATE) match via cast", "[bridge][cdc][apply]") {
    DuckDbBridge db;
    // SFLIGHT-shaped target: CONNID numeric, FLDATE a real DATE.
    db.Execute("CREATE TABLE sf(mandt VARCHAR, carrid VARCHAR, connid INTEGER, fldate DATE, "
               "price INTEGER, PRIMARY KEY(mandt,carrid,connid,fldate))");
    db.Execute("INSERT INTO sf VALUES "
               "('001','AA',17,DATE '2099-12-31',100),('001','LH',400,DATE '2099-12-30',200)");
    db.CdcRegister("sf", "SFLIGHT", "mandt,carrid,connid,fldate", "HANA", "DELETE_ONLY", "L");
    db.CdcSetStatus("sf", "SEEDED");
    // the log delivers SAP-raw key text: NUMC '0017', DATS '20991231'.
    db.Execute("CREATE TABLE sflog(mandt VARCHAR, carrid VARCHAR, connid VARCHAR, fldate VARCHAR, "
               "\"_op\" VARCHAR, \"_seq\" BIGINT)");
    db.Execute("INSERT INTO sflog VALUES ('001','AA','0017','20991231','D',1)");
    CdcApplyResult r = db.CdcApply("sf", "sflog", {"mandt", "carrid", "connid", "fldate"});
    REQUIRE(r.del == 1);
    REQUIRE(db.Query("SELECT count(*) AS c FROM sf WHERE carrid='AA'").rows[0] == R"({"c":0})");
    REQUIRE(db.Query("SELECT count(*) AS c FROM sf").rows[0] == R"({"c":1})");   // LH untouched
}

TEST_CASE("CDC apply: empty batch is a no-op (position unchanged)", "[bridge][cdc][apply]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE t3(id INTEGER PRIMARY KEY)");
    db.CdcRegister("t3", "T3", "id", "HANA", "DELETE_ONLY", "L");
    db.CdcAdvancePosition("t3", 7);
    db.Execute("CREATE TABLE log3(id INTEGER, \"_op\" VARCHAR, \"_seq\" BIGINT)");   // empty
    CdcApplyResult r = db.CdcApply("t3", "log3", {"id"});
    REQUIRE_FALSE(r.applied);
    REQUIRE(r.prune_bound == 7);
    REQUIRE(db.CdcGet("t3").position == 7);
}

TEST_CASE("CDC apply: an error rolls back, position untouched", "[bridge][cdc][apply]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE t4(id INTEGER PRIMARY KEY, v INTEGER NOT NULL)");
    db.Execute("INSERT INTO t4 VALUES (1,10)");
    db.CdcRegister("t4", "T4", "id", "HANA", "FULL_IUD", "L");
    db.CdcSetStatus("t4", "SEEDED");
    // a net insert with NULL in a NOT NULL column -> the upsert fails mid-transaction.
    db.Execute("CREATE TABLE log4(id INTEGER, v INTEGER, \"_op\" VARCHAR, \"_seq\" BIGINT)");
    db.Execute("INSERT INTO log4 VALUES (2,NULL,'I',5)");
    REQUIRE_THROWS(db.CdcApply("t4", "log4", {"id"}));
    REQUIRE(db.CdcGet("t4").position == 0);                                   // not advanced
    REQUIRE(db.Query("SELECT count(*) AS c FROM t4").rows[0] == R"({"c":1})"); // id2 not inserted
}

TEST_CASE("IngestBxml MERGE: op_col drives insert/update/delete", "[bridge][ingest][merge]") {
    DuckDbBridge db;
    std::string ddl = "CREATE TABLE IF NOT EXISTS m(id INTEGER PRIMARY KEY, v VARCHAR);";
    sxml::Table s; s.columns = {"ID", "V"};
    s.rows = {{"1", "a"}, {"2", "b"}, {"3", "c"}};
    db.IngestBxml("m", sxml::Encode("DATA", s), IngestMode::Upsert, {"id"}, "",
                  "SET threads TO 1;", ddl);

    // A change package: update id=1, insert id=4, delete id=2 (id=3 untouched).
    sxml::Table d; d.columns = {"ID", "V", "OP"};
    d.rows = {{"1", "a2", "U"}, {"4", "d", "I"}, {"2", "", "D"}};
    auto n = db.IngestBxml("m", sxml::Encode("DATA", d), IngestMode::Merge, {"id"},
                           "", "", "", "OP");
    REQUIRE(n == 3);
    REQUIRE(db.Query("SELECT v FROM m WHERE id=1").rows[0] == R"({"v":"a2"})");      // updated
    REQUIRE(db.Query("SELECT v FROM m WHERE id=4").rows[0] == R"({"v":"d"})");       // inserted
    REQUIRE(db.Query("SELECT count(*) AS c FROM m WHERE id=2").rows[0] == R"({"c":0})"); // deleted
    REQUIRE(db.Query("SELECT count(*) AS c FROM m").rows[0] == R"({"c":3})");        // 1,3,4
    // op_col is control data, never a target column.
    auto cols = db.Query("SELECT count(*) AS c FROM information_schema.columns "
                         "WHERE lower(table_name)='m' AND lower(column_name)='op'");
    REQUIRE(cols.rows[0] == R"({"c":0})");
}

TEST_CASE("IngestBxml MERGE: empty op_col behaves like UPSERT", "[bridge][ingest][merge]") {
    DuckDbBridge db;
    std::string ddl = "CREATE TABLE IF NOT EXISTS m2(id INTEGER PRIMARY KEY, v VARCHAR);";
    sxml::Table s; s.columns = {"ID", "V"}; s.rows = {{"1", "a"}, {"2", "b"}};
    db.IngestBxml("m2", sxml::Encode("DATA", s), IngestMode::Merge, {"id"}, "",
                  "SET threads TO 1;", ddl, "");          // empty op_col
    sxml::Table u; u.columns = {"ID", "V"}; u.rows = {{"2", "B"}, {"3", "c"}};
    db.IngestBxml("m2", sxml::Encode("DATA", u), IngestMode::Merge, {"id"}, "", "", "", "");
    REQUIRE(db.Query("SELECT count(*) AS c FROM m2").rows[0] == R"({"c":3})");   // upsert, no delete
    REQUIRE(db.Query("SELECT v FROM m2 WHERE id=2").rows[0] == R"({"v":"B"})");
}

TEST_CASE("IngestBxml MERGE: bad cast aborts the whole package atomically",
          "[bridge][ingest][merge][atomic]") {
    DuckDbBridge db;
    std::string ddl = "CREATE TABLE IF NOT EXISTS m3(id INTEGER PRIMARY KEY, n INTEGER);";
    sxml::Table s; s.columns = {"ID", "N"}; s.rows = {{"1", "10"}, {"2", "20"}};
    db.IngestBxml("m3", sxml::Encode("DATA", s), IngestMode::Upsert, {"id"}, "",
                  "SET threads TO 1;", ddl);
    // A package that DELETEs id=2 and then fails to cast id=3's N: the whole
    // transaction (delete + insert) must roll back, leaving the target intact.
    sxml::Table d; d.columns = {"ID", "N", "OP"};
    d.rows = {{"2", "0", "D"}, {"3", "notanumber", "I"}};
    REQUIRE_THROWS(db.IngestBxml("m3", sxml::Encode("DATA", d), IngestMode::Merge,
                                 {"id"}, "", "", "", "OP"));
    REQUIRE(db.Query("SELECT count(*) AS c FROM m3").rows[0] == R"({"c":2})");        // delete rolled back
    REQUIRE(db.Query("SELECT count(*) AS c FROM m3 WHERE id=2").rows[0] == R"({"c":1})");
    REQUIRE(db.Query("SELECT n FROM m3 WHERE id=1").rows[0] == R"({"n":10})");        // unchanged
}

TEST_CASE("SnapshotMerge: insert+update+delete diff in one transaction",
          "[bridge][snapshot]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE t(id INTEGER PRIMARY KEY, v VARCHAR)");
    db.Execute("INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c')");
    // A fresh full snapshot: id=1 unchanged, id=2 changed, id=4 new, id=3 gone.
    db.Execute("CREATE TABLE t__snap(id INTEGER PRIMARY KEY, v VARCHAR)");
    db.Execute("INSERT INTO t__snap VALUES (1,'a'),(2,'B'),(4,'d')");
    auto res = db.SnapshotMerge("t", "t__snap", {"id"});
    REQUIRE(res.ins == 1);   // id=4
    REQUIRE(res.upd == 2);   // id=1,2 present in both (upserted)
    REQUIRE(res.del == 1);   // id=3 absent from snapshot
    REQUIRE(db.Query("SELECT count(*) AS c FROM t").rows[0] == R"({"c":3})");
    REQUIRE(db.Query("SELECT v FROM t WHERE id=2").rows[0] == R"({"v":"B"})");
    REQUIRE(db.Query("SELECT count(*) AS c FROM t WHERE id=3").rows[0] == R"({"c":0})");
    REQUIRE(db.Query("SELECT v FROM t WHERE id=4").rows[0] == R"({"v":"d"})");
    // staging table consumed (dropped) by the merge.
    auto sg = db.Query("SELECT count(*) AS c FROM information_schema.tables "
                       "WHERE lower(table_name)='t__snap'");
    REQUIRE(sg.rows[0] == R"({"c":0})");
}

TEST_CASE("IngestBxml UPSERT: composite key with client-first column", "[bridge][ingest][upsert][composite]") {
    DuckDbBridge db;
    std::string ddl = "CREATE TABLE IF NOT EXISTS mt(client VARCHAR, id VARCHAR, v VARCHAR, PRIMARY KEY(client,id));";
    sxml::Table s; s.columns = {"CLIENT","ID","V"};
    s.rows = {{"001","1","a"},{"001","2","b"},{"001","3","c"}};
    auto n = db.IngestBxml("mt", sxml::Encode("DATA", s), IngestMode::Insert, {"CLIENT","ID"}, "", "SET threads TO 1;", ddl);
    REQUIRE(n == 3);
    REQUIRE(db.Query("SELECT count(*) AS c FROM mt").rows[0] == R"({"c":3})");
    // re-upsert the SAME 3 rows (ddl carries CREATE IF NOT EXISTS, like a delta cycle)
    auto n2 = db.IngestBxml("mt", sxml::Encode("DATA", s), IngestMode::Upsert, {"CLIENT","ID"}, "", "", ddl);
    REQUIRE(n2 == 3);
    REQUIRE(db.Query("SELECT count(*) AS c FROM mt").rows[0] == R"({"c":3})");
    REQUIRE(db.Query("SELECT client FROM mt WHERE id='1'").rows[0] == R"({"client":"001"})");
}

TEST_CASE("IngestBxml UPSERT: exact A4H delta shape (client PK + DECIMAL ts)", "[bridge][ingest][upsert][a4h]") {
    DuckDbBridge db;
    std::string ddl = "CREATE TABLE IF NOT EXISTS mt2 (CLIENT VARCHAR, ID VARCHAR, NAME VARCHAR, VAL INTEGER, CHANGED_AT DECIMAL(21,7), PRIMARY KEY(CLIENT,ID));";
    sxml::Table s; s.columns = {"CLIENT","ID","NAME","VAL","CHANGED_AT"};
    s.rows = {{"001","0000000001","row 1","1","20260607135828.5250730"},
              {"001","0000000002","row 2","2","20260607135828.5250730"},
              {"001","0000000003","row 3","3","20260607135828.5250730"}};
    // baseline: heap insert then PK already in ddl
    auto n = db.IngestBxml("mt2", sxml::Encode("DATA", s), IngestMode::Insert, {"CLIENT","ID"}, "", "SET threads TO 1;", ddl);
    REQUIRE(n == 3);
    // delta upsert: same rows, ddl carries CREATE IF NOT EXISTS (as on A4H)
    auto n2 = db.IngestBxml("mt2", sxml::Encode("DATA", s), IngestMode::Upsert, {"CLIENT","ID"}, "", "", ddl);
    REQUIRE(n2 == 3);
    REQUIRE(db.Query("SELECT count(*) AS c FROM mt2").rows[0] == R"({"c":3})");
    REQUIRE(db.Query("SELECT client AS c FROM mt2 WHERE id='0000000001'").rows[0] == R"({"c":"001"})");
    REQUIRE(db.Query("SELECT count(*) AS c FROM mt2 WHERE client IS NULL").rows[0] == R"({"c":0})");
}
