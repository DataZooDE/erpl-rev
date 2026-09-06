// The monitor's model: which target is worst, and how its lag reads.

#include <catch2/catch_test_macros.hpp>

#include "duckdb_bridge.hpp"
#include "tui_model.hpp"

using namespace erpl_rev;

TEST_CASE("tui: lag reads as a duration, not a second count", "[tui]") {
    // A raw count makes the reader do arithmetic to notice that a target on a
    // five-second cadence is an hour behind.
    CHECK(tui::FormatLag(-1) == "never");
    CHECK(tui::FormatLag(0) == "0s");
    CHECK(tui::FormatLag(12) == "12s");
    CHECK(tui::FormatLag(250) == "4m10s");
    CHECK(tui::FormatLag(10920) == "3h02m");
}

TEST_CASE("tui: the worst target sorts first, not the alphabetical one", "[tui]") {
    // An operator opening a monitor is looking for the problem. Sorting by name
    // puts the one broken target on page three.
    std::vector<tui::Row> rows;
    tui::Row ok;   ok.target = "aaa_fine";     ok.healthy = true;  ok.lag_seconds = 5;
    tui::Row late; late.target = "bbb_late";                       late.lag_seconds = 900;
    tui::Row park; park.target = "ccc_parked"; park.parked = true;
    tui::Row blk;  blk.target = "ddd_blocked"; blk.blocked = true;
    rows = {ok, late, park, blk};

    tui::SortForOperator(rows);

    CHECK(rows[0].target == "ddd_blocked");   // cannot run at all
    CHECK(rows[1].target == "ccc_parked");    // backed off
    CHECK(rows[2].target == "bbb_late");      // running but behind
    CHECK(rows[3].target == "aaa_fine");
}

TEST_CASE("tui: a never-run target outranks a merely late one", "[tui]") {
    // "No data at all" is a worse state than "data is old", and it is the
    // commonest support call. It must not sort below a target with a big lag
    // number just because -1 is a small integer.
    std::vector<tui::Row> rows;
    tui::Row never; never.target = "never"; never.lag_seconds = -1;
    tui::Row late;  late.target = "late";   late.lag_seconds = 9999;
    rows = {late, never};

    tui::SortForOperator(rows);
    CHECK(rows[0].target == "never");
}

TEST_CASE("tui: a snapshot reads both views through one connection", "[tui]") {
    DuckDbBridge db;
    db.Execute("INSERT INTO _erpl_rev_delta_state "
               "(target, method, source_from, keys, cadence, last_run_ts, rows_applied) "
               "VALUES ('sales','WATERMARK','VBAK','id','micro:5', now() - INTERVAL '10 seconds', 7)");

    const auto snap = tui::Load([&](const std::string &sql) { return db.Query(sql); });
    REQUIRE(snap.error.empty());
    REQUIRE(snap.rows.size() == 1);
    CHECK(snap.rows[0].target == "sales");
    CHECK(snap.rows[0].rows == 7);
    CHECK(snap.rows[0].healthy);
    CHECK(snap.summary.targets == 1);
}

TEST_CASE("tui: a failed read is shown, not thrown", "[tui]") {
    // The monitor runs against a server that can restart under it. Throwing
    // would drop the operator back to a shell prompt at the exact moment they
    // were watching something.
    const auto snap = tui::Load([](const std::string &) -> QueryResult {
        throw std::runtime_error("connection reset");
    });
    CHECK_FALSE(snap.error.empty());
    CHECK(snap.rows.empty());
}
