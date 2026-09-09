// The shipped transformation macros.
//
// The competitor blueprint does this with per-field transfer rules in ABAP. Here
// they are DuckDB macros, which means no new SAP object, no ABAP CPU per row,
// and they are testable without SAP at all.
//
// Crucially they are CREATE OR REPLACE in the bridge constructor, beside the
// run-stats view -- NOT in the migration list. A macro that lived in the
// customer's file as versioned DDL could never be fixed: a currency bug found in
// v2 would stay broken on every file already created.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>

#include "duckdb_bridge.hpp"
#include "temp_path.hpp"

using namespace erpl_rev;

namespace {
std::string One(DuckDbBridge &db, const std::string &expr) {
    const auto r = db.Query("SELECT " + expr + " AS v");
    REQUIRE(r.rows.size() == 1);
    return r.rows[0];
}
}  // namespace

TEST_CASE("macros: ALPHA pads and strips leading zeros", "[macros]") {
    DuckDbBridge db;
    // The SAP convention: a numeric-looking key is stored zero-padded.
    CHECK(One(db, "erpl_rev_alpha_in('4711', 10)") == R"({"v":"0000004711"})");
    CHECK(One(db, "erpl_rev_alpha_out('0000004711')") == R"({"v":"4711"})");
    // Round trip.
    CHECK(One(db, "erpl_rev_alpha_out(erpl_rev_alpha_in('4711', 18))") == R"({"v":"4711"})");
}

TEST_CASE("macros: ALPHA leaves a non-numeric value alone", "[macros]") {
    // A material number like 'ABC-1' is not zero-padded by SAP either; padding
    // it would silently corrupt the key.
    DuckDbBridge db;
    CHECK(One(db, "erpl_rev_alpha_in('ABC-1', 10)") == R"({"v":"ABC-1"})");
    CHECK(One(db, "erpl_rev_alpha_out('ABC-1')") == R"({"v":"ABC-1"})");
}

TEST_CASE("macros: XFELD becomes a real boolean", "[macros]") {
    DuckDbBridge db;
    CHECK(One(db, "erpl_rev_xfeld('X')") == R"({"v":true})");
    CHECK(One(db, "erpl_rev_xfeld('')") == R"({"v":false})");
    CHECK(One(db, "erpl_rev_xfeld(NULL)") == R"({"v":false})");
}

TEST_CASE("macros: SAP dates and times parse, including the blank ones", "[macros]") {
    DuckDbBridge db;
    CHECK(One(db, "erpl_rev_dats('20260905')") == R"({"v":"2026-09-05"})");
    // '00000000' is SAP's empty date. Parsing it as a date would either fail or
    // invent the year zero; it has to come back NULL.
    CHECK(One(db, "erpl_rev_dats('00000000')") == R"({"v":null})");
    CHECK(One(db, "erpl_rev_dats('')") == R"({"v":null})");
    CHECK(One(db, "erpl_rev_tims('143000')") == R"({"v":"14:30:00"})");
}

TEST_CASE("macros: currency amounts are corrected by TCURX", "[macros][currency]") {
    // The problem this exists for: SAP stores every currency amount with two
    // implied decimals regardless of the currency. For JPY, which has none, the
    // stored 1234 means 1234 yen -- not 12.34. Reading it without correcting is
    // wrong by a factor of 100, silently, on real money.
    // TCURX has to exist when the macros are bound, which is at boot. That is
    // the real sequence too: replicate TCURX, then restart the server.
    const std::string path = erpl_rev_test::TmpDbPath("curr");
    {
        DuckDbBridge seed(path);
        seed.Execute("CREATE TABLE tcurx(currkey VARCHAR, currdec INTEGER)");
        seed.Execute("INSERT INTO tcurx VALUES ('JPY',0),('TND',3)");
    }
    DuckDbBridge db(path);

    // A currency absent from TCURX has the default two decimals and needs no
    // correction.
    CHECK(One(db, "erpl_rev_curr_amount(1234.00, 'EUR')") == R"({"v":1234.0})");
    // JPY: zero decimals, so the stored value is scaled up by 100.
    CHECK(One(db, "erpl_rev_curr_amount(12.34, 'JPY')") == R"({"v":1234.0})");
    // TND: three decimals, so it is scaled down by ten.
    CHECK(One(db, "erpl_rev_curr_amount(12.34, 'TND')") == R"({"v":1.234})");

    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
}

TEST_CASE("macros: a currency correction without TCURX fails loudly", "[macros][currency]") {
    // Returning an uncorrected number would be a wrong amount presented as a
    // right one. Better to refuse and say what is missing.
    DuckDbBridge db;
    bool threw = false;
    std::string msg;
    try {
        const auto r = db.Query("SELECT erpl_rev_curr_amount(12.34, 'JPY') AS v");
        msg = r.rows.empty() ? std::string("<no rows>") : r.rows[0];
    } catch (const std::exception &e) {
        threw = true;
        msg = e.what();
    }
    INFO("result was: " << msg);
    CHECK(threw);
    CHECK(msg.find("TCURX") != std::string::npos);
}

TEST_CASE("macros: they are recreated on every open, so a fix reaches old files",
          "[macros]") {
    // The reason they are not migrations. Simulate a file carrying an older,
    // wrong definition and confirm reopening replaces it.
    const std::string path = erpl_rev_test::TmpDbPath("macro");
    {
        DuckDbBridge db(path);
        db.Execute("CREATE OR REPLACE MACRO erpl_rev_xfeld(v) AS 'definitely wrong'");
        CHECK(One(db, "erpl_rev_xfeld('X')") == R"({"v":"definitely wrong"})");
    }
    {
        DuckDbBridge db(path);   // reopen: the constructor recreates it
        CHECK(One(db, "erpl_rev_xfeld('X')") == R"({"v":true})");
    }
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
}
