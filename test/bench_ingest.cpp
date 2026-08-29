// Performance experiment: which insert mechanism is fastest for our workload?
//
// Compares, on a synthetic 50-column mixed-type table (BIGINT / VARCHAR /
// DECIMAL / DATE — the families ZWIDE_BSEG exercises), the candidate bulk-load
// strategies for IngestBxml. Hidden by default ([.]); run with:
//   ./erpl_rev_tests "[bench]" --success     (numbers are emitted via WARN)
//
// Rows-per-second is the comparable metric; each strategy uses a row count sized
// to finish in a sane time (per-row is ~3 orders slower, so it runs fewer rows).
//
// Observed (50 cols, in-memory, default threads; rates vary with load):
//   A per-row INSERT        ~0.2k rows/s   (baseline — the old IngestBxml)
//   B multi-row INSERT      ~3k   rows/s
//   C appender (direct)     ~20-30k rows/s (cast-bound: per-cell DefaultCastAs)
//   D appender+typed-stage   ~23k rows/s
//   E varchar-stage + cast  ~50k  rows/s   <-- WINNER: append raw strings, then
//                                              ONE vectorized INSERT…SELECT cast.
// => IngestBxml uses E: Appender into a VARCHAR/BLOB staging clone, then a
//    set-based INSERT … SELECT <casts> [ON CONFLICT …]. ~250x over per-row.
#include <catch2/catch_test_macros.hpp>

#include "duckdb.hpp"
#include "duckdb_bridge.hpp"
#include "sxml_binary.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace duckdb;
using clk = std::chrono::steady_clock;

namespace {

constexpr int NCOL = 50;

LogicalType ColType(int c) {
    switch (c % 4) {
        case 0:  return LogicalType::BIGINT;
        case 1:  return LogicalType::VARCHAR;
        case 2:  return LogicalType::DECIMAL(13, 2);
        default: return LogicalType::DATE;
    }
}

std::string Ddl(const std::string &t) {
    std::string s = "CREATE OR REPLACE TABLE " + t + " (";
    for (int c = 0; c < NCOL; c++) {
        if (c) s += ",";
        s += "c" + std::to_string(c) + " ";
        switch (c % 4) {
            case 0:  s += "BIGINT"; break;
            case 1:  s += "VARCHAR"; break;
            case 2:  s += "DECIMAL(13,2)"; break;
            default: s += "DATE"; break;
        }
    }
    s += ")";
    return s;
}

// Cell value as the string a decoded BXML cell would carry.
std::string Cell(int c, long long r) {
    switch (c % 4) {
        case 0:  return std::to_string(r * NCOL + c);
        case 1:  return "v" + std::to_string(r) + "_" + std::to_string(c);
        case 2:  return std::to_string(r % 100000) + ".50";
        default: return "2026-06-01";
    }
}

std::string SqlQuote(const std::string &s) {
    std::string o = "'";
    for (char ch : s) { if (ch == '\'') o += "''"; else o += ch; }
    o += "'";
    return o;
}

// SQL literal for the text-based strategies (A/B): quote varchar+date, numbers raw.
std::string Lit(int c, long long r) {
    return (c % 4 == 1 || c % 4 == 3) ? SqlQuote(Cell(c, r)) : Cell(c, r);
}

void Check(Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    if (r->HasError()) throw std::runtime_error(r->GetError());
}

// A — one INSERT statement per row (the current IngestBxml behavior).
void StratA(Connection &con, long long n) {
    Check(con, Ddl("t_a"));
    for (long long r = 0; r < n; r++) {
        std::string s = "INSERT INTO t_a VALUES (";
        for (int c = 0; c < NCOL; c++) { if (c) s += ","; s += Lit(c, r); }
        s += ")";
        Check(con, s);
    }
}

// B — multi-row INSERT, `chunk` rows of VALUES per statement.
void StratB(Connection &con, long long n, long long chunk) {
    Check(con, Ddl("t_b"));
    long long r = 0;
    while (r < n) {
        long long end = (r + chunk < n) ? r + chunk : n;
        std::string s = "INSERT INTO t_b VALUES ";
        for (long long i = r; i < end; i++) {
            if (i > r) s += ",";
            s += "(";
            for (int c = 0; c < NCOL; c++) { if (c) s += ","; s += Lit(c, i); }
            s += ")";
        }
        Check(con, s);
        r = end;
    }
}

// C — DuckDB Appender straight into the target (typed Value per cell).
void StratC(Connection &con, long long n) {
    Check(con, Ddl("t_c"));
    std::vector<LogicalType> types;
    for (int c = 0; c < NCOL; c++) types.push_back(ColType(c));
    Appender app(con, "t_c");
    for (long long r = 0; r < n; r++) {
        app.BeginRow();
        for (int c = 0; c < NCOL; c++)
            app.Append(Value(Cell(c, r)).DefaultCastAs(types[c]));
        app.EndRow();
    }
    app.Close();
}

// D — Appender into a temp staging clone, then set-based INSERT…SELECT per
// package (the shape the UPSERT/delta path needs). pkg = rows per merge.
void StratD(Connection &con, long long n, long long pkg) {
    Check(con, Ddl("t_d"));
    Check(con, "CREATE OR REPLACE TEMP TABLE t_d__stg AS SELECT * FROM t_d LIMIT 0");
    std::vector<LogicalType> types;
    for (int c = 0; c < NCOL; c++) types.push_back(ColType(c));
    long long r = 0;
    while (r < n) {
        long long end = (r + pkg < n) ? r + pkg : n;
        {
            Appender app(con, "t_d__stg");
            for (long long i = r; i < end; i++) {
                app.BeginRow();
                for (int c = 0; c < NCOL; c++)
                    app.Append(Value(Cell(c, i)).DefaultCastAs(types[c]));
                app.EndRow();
            }
            app.Close();
        }
        Check(con, "INSERT INTO t_d SELECT * FROM t_d__stg");
        Check(con, "DELETE FROM t_d__stg");
        r = end;
    }
}

// E — Appender into an ALL-VARCHAR staging (no per-cell cast — just append the
// raw strings), then ONE vectorized set-based INSERT…SELECT that casts each
// column. Hypothesis: moves the cast off the hot per-cell path. (Binary columns
// would be BLOB in staging; this synthetic set has none.)
void StratE(Connection &con, long long n, long long pkg) {
    Check(con, Ddl("t_e"));
    std::string stg = "CREATE OR REPLACE TEMP TABLE t_e__stg (";
    for (int c = 0; c < NCOL; c++) { if (c) stg += ","; stg += "c" + std::to_string(c) + " VARCHAR"; }
    stg += ")";
    Check(con, stg);
    // cast projection: col::<targettype> (varchar passes through)
    std::string sel = "INSERT INTO t_e SELECT ";
    for (int c = 0; c < NCOL; c++) {
        if (c) sel += ",";
        std::string col = "c" + std::to_string(c);
        switch (c % 4) {
            case 0:  sel += col + "::BIGINT"; break;
            case 1:  sel += col; break;
            case 2:  sel += col + "::DECIMAL(13,2)"; break;
            default: sel += col + "::DATE"; break;
        }
    }
    sel += " FROM t_e__stg";

    long long r = 0;
    while (r < n) {
        long long end = (r + pkg < n) ? r + pkg : n;
        {
            Appender app(con, "t_e__stg");
            for (long long i = r; i < end; i++) {
                app.BeginRow();
                for (int c = 0; c < NCOL; c++) app.Append(Value(Cell(c, i)));  // VARCHAR, no cast
                app.EndRow();
            }
            app.Close();
        }
        Check(con, sel);
        Check(con, "DELETE FROM t_e__stg");
        r = end;
    }
}

// Columnar feed (the IngestBxml fill path), parameterized by column count. Both
// keep the proven all-VARCHAR staging + ONE vectorized INSERT…SELECT cast; they
// differ ONLY in how staging is filled: E = per-cell Appender::Append (the old
// way), F = DataChunk VARCHAR vectors + AppendDataChunk (the new way).
std::string DdlN(const std::string &t, int ncol) {
    std::string s = "CREATE OR REPLACE TABLE " + t + " (";
    for (int c = 0; c < ncol; c++) {
        if (c) s += ",";
        s += "c" + std::to_string(c) + " ";
        switch (c % 4) { case 0: s += "BIGINT"; break; case 1: s += "VARCHAR"; break;
                         case 2: s += "DECIMAL(13,2)"; break; default: s += "DATE"; break; }
    }
    return s + ")";
}
std::string StagingDdl(const std::string &stg, int ncol) {
    std::string s = "CREATE OR REPLACE TEMP TABLE " + stg + " (";
    for (int c = 0; c < ncol; c++) { if (c) s += ","; s += "c" + std::to_string(c) + " VARCHAR"; }
    return s + ")";
}
std::string CastInsert(const std::string &tgt, const std::string &stg, int ncol) {
    std::string sel = "INSERT INTO " + tgt + " SELECT ";
    for (int c = 0; c < ncol; c++) {
        if (c) sel += ",";
        std::string col = "c" + std::to_string(c);
        switch (c % 4) { case 0: sel += col + "::BIGINT"; break; case 1: sel += col; break;
                         case 2: sel += col + "::DECIMAL(13,2)"; break; default: sel += col + "::DATE"; break; }
    }
    return sel + " FROM " + stg;
}
void StratEg(Connection &con, long long n, long long pkg, int ncol, const std::string &tgt) {
    Check(con, DdlN(tgt, ncol));
    std::string stg = tgt + "__stg";
    Check(con, StagingDdl(stg, ncol));
    std::string sel = CastInsert(tgt, stg, ncol);
    for (long long r = 0; r < n;) {
        long long end = (r + pkg < n) ? r + pkg : n;
        { Appender app(con, stg);
          for (long long i = r; i < end; i++) {
              app.BeginRow();
              for (int c = 0; c < ncol; c++) app.Append(Value(Cell(c, i)));
              app.EndRow();
          }
          app.Close(); }
        Check(con, sel);
        Check(con, "DELETE FROM " + stg);
        r = end;
    }
}
void StratFg(Connection &con, long long n, long long pkg, int ncol, const std::string &tgt) {
    Check(con, DdlN(tgt, ncol));
    std::string stg = tgt + "__stg";
    Check(con, StagingDdl(stg, ncol));
    std::string sel = CastInsert(tgt, stg, ncol);
    duckdb::vector<LogicalType> vtypes(ncol, LogicalType::VARCHAR);
    for (long long r = 0; r < n;) {
        long long end = (r + pkg < n) ? r + pkg : n;
        { Appender app(con, stg);
          DataChunk chunk; chunk.Initialize(Allocator::DefaultAllocator(), vtypes);
          for (long long i = r; i < end;) {
              long long blk = (end - i < STANDARD_VECTOR_SIZE) ? (end - i) : STANDARD_VECTOR_SIZE;
              chunk.Reset();
              for (int c = 0; c < ncol; c++) {
                  Vector &v = chunk.data[c];
                  auto out = FlatVector::GetData<string_t>(v);
                  for (long long k = 0; k < blk; k++)
                      out[k] = StringVector::AddString(v, Cell(c, i + k));
              }
              chunk.SetCardinality(blk);
              app.AppendDataChunk(chunk);
              i += blk;
          }
          app.Close(); }
        Check(con, sel);
        Check(con, "DELETE FROM " + stg);
        r = end;
    }
}

double Timed(const std::function<void()> &f) {
    auto t0 = clk::now();
    f();
    auto t1 = clk::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

long long CountOf(Connection &con, const std::string &t) {
    auto r = con.Query("SELECT count(*) FROM " + t);
    return r->GetValue(0, 0).GetValue<int64_t>();
}

} // namespace

// Defer-PK: chunked inserts into a PK table (index maintained per package) vs
// chunked inserts into a heap then ONE ALTER ADD PRIMARY KEY. Models the
// replication choice; the win grows with row count / package count (a large ART
// index re-balanced per package). Pure DuckDB — no SAP.
TEST_CASE("defer-PK bench: per-package PK vs heap+ADD PRIMARY KEY", "[bench][.]") {
    DuckDB db(nullptr);
    Connection con(db);
    const long long N = 2000000, CH = 100000;   // 2M rows in 20 chunks of 100k
    Check(con, "CREATE OR REPLACE TABLE src AS "
               "SELECT i AS k, (i%1000) AS k2, 'v'||i AS v FROM range(" + std::to_string(N) + ") t(i)");

    auto chunked = [&](const std::string &t) {
        for (long long off = 0; off < N; off += CH)
            Check(con, "INSERT INTO " + t + " SELECT * FROM src WHERE k>=" +
                       std::to_string(off) + " AND k<" + std::to_string(off + CH));
    };

    Check(con, "CREATE OR REPLACE TABLE a (k BIGINT, k2 BIGINT, v VARCHAR, PRIMARY KEY(k))");
    double ta = Timed([&] { chunked("a"); });                       // PK maintained per chunk

    Check(con, "CREATE OR REPLACE TABLE b (k BIGINT, k2 BIGINT, v VARCHAR)");
    double tb = Timed([&] { chunked("b"); Check(con, "ALTER TABLE b ADD PRIMARY KEY(k)"); });

    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "\n=== defer-PK (%lld rows, 20 pkgs) ===\n"
                  "  A per-package PK     %8.3f s\n"
                  "  B heap + ADD PK once %8.3f s   (%.2fx)\n",
                  N, ta, tb, ta > 0 ? ta / tb : 0.0);
    WARN(buf);
    REQUIRE(CountOf(con, "a") == N);
    REQUIRE(CountOf(con, "b") == N);
}

// Query: a capped SELECT should STREAM (stop after cap+1) rather than materialize
// + drain the whole result. Compare streaming (fast) vs want_total=true (drains
// all, the old behavior) over a 5M-row table with a 10k cap.
TEST_CASE("query bench: streaming cap vs drain-all", "[bench][.]") {
    erpl_rev::DuckDbBridge bridge;
    bridge.Execute("CREATE TABLE qbig AS SELECT i AS id, 'v'||i AS v FROM range(5000000) t(i)");
    double fast = Timed([&] {
        auto r = bridge.Query("SELECT * FROM qbig", 10000);            // streaming, stops ~10001
        if (r.rows.size() != 10000) throw std::runtime_error("cap");
    });
    double drain = Timed([&] {
        auto r = bridge.Query("SELECT * FROM qbig", 10000, true);      // drains all 5M to count
        if (r.row_count != 5000000) throw std::runtime_error("total");
    });
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "\n=== query cap=10k over 5M rows ===\n"
                  "  streaming (stop at cap+1) %8.3f s\n"
                  "  drain-all (exact total)   %8.3f s   (%.1fx)\n",
                  fast, drain, fast > 0 ? drain / fast : 0.0);
    WARN(buf);
}

TEST_CASE("columnar feed bench: per-cell Appender (E) vs DataChunk (F)", "[bench][.]") {
    DuckDB db(nullptr);
    Connection con(db);
    std::string rep = "\n=== columnar feed: E per-cell Append vs F DataChunk ===\n";
    char buf[160];
    auto report = [&](const char *name, int ncol, long long n, double s) {
        std::snprintf(buf, sizeof buf, "  %-18s cols=%3d %9lld rows %8.3f s %12.0f rows/s\n",
                      name, ncol, n, s, s > 0 ? n / s : 0.0);
        rep += buf;
    };
    report("E narrow x50k", 50,  1000000, Timed([&] { StratEg(con, 1000000, 50000,  50,  "e50"); }));
    report("F narrow x50k", 50,  1000000, Timed([&] { StratFg(con, 1000000, 50000,  50,  "f50"); }));
    report("E wide   x20k", 390, 200000,  Timed([&] { StratEg(con, 200000,  20000, 390, "e390"); }));
    report("F wide   x20k", 390, 200000,  Timed([&] { StratFg(con, 200000,  20000, 390, "f390"); }));
    WARN(rep);
    REQUIRE(CountOf(con, "e50") == 1000000);
    REQUIRE(CountOf(con, "f50") == 1000000);
    REQUIRE(CountOf(con, "e390") == 200000);
    REQUIRE(CountOf(con, "f390") == 200000);
}

TEST_CASE("ingest strategy benchmark", "[bench][.]") {
    DuckDB db(nullptr);   // in-memory
    Connection con(db);

    std::string rep = "\n=== ingest strategy bench (50 cols: BIGINT/VARCHAR/DECIMAL/DATE) ===\n";
    char buf[256];
    auto report = [&](const char *name, long long n, double s) {
        std::snprintf(buf, sizeof buf, "  %-24s %10lld rows  %8.3f s  %12.0f rows/s\n",
                      name, n, s, s > 0 ? n / s : 0.0);
        rep += buf;
    };

    report("A per-row INSERT",        5000,    Timed([&] { StratA(con, 5000); }));
    report("B multi-row INSERT x2000", 200000, Timed([&] { StratB(con, 200000, 2000); }));
    report("C appender (direct)",     1000000, Timed([&] { StratC(con, 1000000); }));
    report("D appender+staging x50k", 1000000, Timed([&] { StratD(con, 1000000, 50000); }));
    report("E varchar-stg+cast x50k", 1000000, Timed([&] { StratE(con, 1000000, 50000); }));

    WARN(rep);

    // correctness sanity for the candidates that ran the full set
    REQUIRE(CountOf(con, "t_c") == 1000000);
    REQUIRE(CountOf(con, "t_d") == 1000000);
    REQUIRE(CountOf(con, "t_e") == 1000000);
    REQUIRE(CountOf(con, "t_b") == 200000);
}

// Q10 cursor-encode: the live cursor stores all SAP columns as VARCHAR, so the
// page path is bottlenecked on (a) a heap Value per cell (GetValue().ToString())
// and (b) a full vector<vector<string>> row matrix before Encode walks it again.
// New path: Flatten the chunk, read string_t bytes flat, stream straight to BXML.
// This bench runs BOTH over identical fresh streams and asserts byte-equality.
namespace {
// OLD page producer (pre-Q10): GetValue().ToString() per cell -> Table -> Encode.
std::string OldEncodePages(Connection &con, const std::string &sql,
                           const std::vector<std::string> &cols) {
    auto r = con.SendQuery(sql);
    erpl_rev::sxml::Table tbl;
    tbl.columns = cols;
    const idx_t ncol = cols.size();
    while (true) {
        auto chunk = r->Fetch();
        if (!chunk || chunk->size() == 0) break;
        const idx_t n = chunk->size();
        for (idx_t row = 0; row < n; row++) {
            std::vector<std::string> cells;
            cells.reserve(ncol);
            for (idx_t c = 0; c < ncol; c++) {
                auto v = chunk->GetValue(c, row);
                cells.push_back(v.IsNull() ? std::string() : v.ToString());
            }
            tbl.rows.push_back(std::move(cells));
        }
    }
    return erpl_rev::sxml::Encode("DATA", tbl);
}
// NEW page producer (Q10): mirrors ProducePage's flat VARCHAR fast path.
std::string NewEncodePages(Connection &con, const std::string &sql,
                           const std::vector<std::string> &cols) {
    auto r = con.SendQuery(sql);
    const idx_t ncol = cols.size();
    erpl_rev::sxml::StreamEncoder enc(cols);
    while (true) {
        auto chunk = r->Fetch();
        if (!chunk || chunk->size() == 0) break;
        chunk->Flatten();
        const idx_t n = chunk->size();
        for (idx_t row = 0; row < n; row++) {
            enc.StartRow();
            for (idx_t c = 0; c < ncol; c++) {
                auto &vec = chunk->data[c];
                if (vec.GetType().id() == LogicalTypeId::VARCHAR) {
                    if (!FlatVector::Validity(vec).RowIsValid(row)) { enc.Cell(c, nullptr, 0); }
                    else { auto &s = FlatVector::GetData<string_t>(vec)[row];
                           enc.Cell(c, s.GetData(), s.GetSize()); }
                } else {
                    auto v = chunk->GetValue(c, row);
                    std::string s = v.IsNull() ? std::string() : v.ToString();
                    enc.Cell(c, s.data(), s.size());
                }
            }
            enc.EndRow();
        }
    }
    return enc.Finish();
}
} // namespace

TEST_CASE("cursor-encode bench: GetValue+Table vs flat+StreamEncoder", "[bench][.]") {
    DuckDB db(nullptr);
    Connection con(db);
    // wide all-VARCHAR table — the shape of a replicated SAP table read back.
    const int ncol = 40;
    const long long nrow = 300000;
    std::string sel;
    std::vector<std::string> cols;
    for (int c = 0; c < ncol; c++) {
        if (c) sel += ", ";
        std::string cn = "C" + std::to_string(c);
        cols.push_back(cn);
        sel += "('val_'||i||'_" + std::to_string(c) + "')::VARCHAR AS " + cn;
    }
    con.Query("CREATE TABLE cbig AS SELECT " + sel + " FROM range(" +
              std::to_string(nrow) + ") t(i)");
    const std::string q = "SELECT * FROM cbig";

    std::string a, b;
    double told = Timed([&] { a = OldEncodePages(con, q, cols); });
    double tnew = Timed([&] { b = NewEncodePages(con, q, cols); });
    REQUIRE(a == b);   // byte-identical: the optimization must not change the wire

    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "\n=== cursor encode: %d cols x %lld VARCHAR rows ===\n"
                  "  OLD GetValue+Table+Encode %8.3f s %12.0f rows/s\n"
                  "  NEW flat+StreamEncoder    %8.3f s %12.0f rows/s   (%.2fx)\n",
                  ncol, nrow, told, told > 0 ? nrow / told : 0.0,
                  tnew, tnew > 0 ? nrow / tnew : 0.0, tnew > 0 ? told / tnew : 0.0);
    WARN(buf);
}

// Where does an ingest actually spend its time?
//
// The strategy comparison above answers "which insert mechanism", but the
// handler does three things per package and only one of them is the insert:
// decode the BXML, append the cells into the staging clone, then one
// INSERT…SELECT with casts. A live 100k-row ZWIDE_BSEG replication spends 60%
// of its wall clock inside this process, so knowing which third that is decides
// what is worth optimising.
//
// Shaped like the real thing: 420 columns, the width of ZWIDE_BSEG.
TEST_CASE("ingest phase split: decode / append / apply", "[bench][.]") {
    constexpr int NCOL = 420;
    constexpr int NROW = 50000;   // one package, as the replicator sends

    // Column names and a DDL matching the live shape: mostly VARCHAR with a
    // scattering of the other families ZWIDE_BSEG carries.
    erpl_rev::sxml::Table t;
    std::string ddl = "CREATE TABLE IF NOT EXISTS phz (";
    for (int c = 0; c < NCOL; c++) {
        char nm[32];
        std::snprintf(nm, sizeof nm, "COL%04d", c);
        t.columns.emplace_back(nm);
        if (c) ddl += ",";
        ddl += std::string(nm) + (c % 7 == 0   ? " BIGINT"
                                  : c % 7 == 1 ? " DECIMAL(15,2)"
                                  : c % 7 == 2 ? " DATE"
                                               : " VARCHAR");
    }
    ddl += ");";

    t.rows.reserve(NROW);
    for (int r = 0; r < NROW; r++) {
        std::vector<std::string> row;
        row.reserve(NCOL);
        for (int c = 0; c < NCOL; c++) {
            if (c % 7 == 0)      row.emplace_back(std::to_string(r * 31 + c));
            else if (c % 7 == 1) row.emplace_back("12.50");
            else if (c % 7 == 2) row.emplace_back("2024-01-15");
            else                 row.emplace_back("abcdefghij");
        }
        t.rows.push_back(std::move(row));
    }

    const std::string bxml = erpl_rev::sxml::Encode("DATA", t);
    WARN("payload: " << NROW << " rows x " << NCOL << " cols, "
                     << bxml.size() / (1024 * 1024) << " MiB BXML");

    // Decode alone.
    auto t0 = clk::now();
    erpl_rev::sxml::Table decoded = erpl_rev::sxml::Decode(bxml);
    auto t1 = clk::now();
    REQUIRE(decoded.rows.size() == NROW);

    // Decode + stage + insert, as IngestBxml does it end to end.
    erpl_rev::DuckDbBridge db;
    auto t2 = clk::now();
    auto n = db.IngestBxml("phz", bxml, erpl_rev::IngestMode::Insert, {}, "", "SET threads TO 1;", ddl);
    auto t3 = clk::now();
    REQUIRE(n == NROW);

    auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    const long dec = ms(t0, t1), all = ms(t2, t3);
    WARN("decode        " << dec << " ms");
    WARN("ingest total  " << all << " ms  (" << (NROW * 1000L / (all ? all : 1)) << " rows/s)");
    WARN("append+apply  " << (all - dec) << " ms  (decode is "
                          << (all ? 100 * dec / all : 0) << "% of ingest)");
}
