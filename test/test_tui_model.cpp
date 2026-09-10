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

TEST_CASE("model: SampleCounts reads the row counts it asked for", "[tui][graph]") {
    // Written after the graph shipped 0/s over a table that had grown from
    // 1,000 rows to 79,500: the sampling was the one piece with no test, and
    // it was the one that was wrong.
    std::vector<std::string> seen;
    auto q = [&](const std::string &sql) {
        seen.push_back(sql);
        QueryResult r;
        if (sql.find("erpl_rev_run_stats") != std::string::npos) {
            r.rows = {R"({"target":"stock_moves","ins":1000000,"upd":40,"del":7})"};
            r.row_count = 1;
            return r;
        }
        r.rows = {sql.find("stock_moves") != std::string::npos ? R"({"n":79500})"
                                                              : R"({"n":12200})"};
        r.row_count = 1;
        return r;
    };

    const auto got = tui::SampleCounts(q, {"stock_moves", "material_master"});
    REQUIRE(got.size() == 2);
    CHECK(got[0].target == "stock_moves");
    CHECK(got[0].rows == 79500);
    CHECK(got[1].rows == 12200);
    // The operation counters travel with the count, so one sample is one
    // moment: reading them on a separate pass would let the two halves of a
    // bucket come from different instants.
    CHECK(got[0].ins == 1000000);
    CHECK(got[0].upd == 40);
    CHECK(got[0].del == 7);
    // A target that has never completed a cycle keeps zeros rather than
    // inheriting its neighbour's.
    CHECK(got[1].upd == 0);
    // One aggregate for every target's operation counters, then one count per
    // target -- so a missing table costs only its own sample.
    //
    // This does NOT assert the absence of a UNION: an earlier version did,
    // enforcing a diagnosis that turned out to be wrong, which is how a
    // refuted claim outlives the evidence against it.
    CHECK(seen.size() == 3);
}

TEST_CASE("model: run statistics that cannot be read cost only the updates",
          "[tui][graph]") {
    // The operation counters come from a view; the row counts do not. If the
    // view is missing -- an older database file, a permission -- the graph
    // should lose its update bands and keep everything else, rather than going
    // blank at the moment someone opened it to find out what was happening.
    auto q = [&](const std::string &sql) -> QueryResult {
        if (sql.find("erpl_rev_run_stats") != std::string::npos)
            throw std::runtime_error("Catalog Error: view erpl_rev_run_stats does not exist");
        QueryResult r;
        r.rows = {R"({"n":500})"};
        r.row_count = 1;
        return r;
    };
    const auto got = tui::SampleCounts(q, {"stock_moves"});
    REQUIRE(got.size() == 1);
    CHECK(got[0].rows == 500);
    CHECK(got[0].upd == 0);
}

TEST_CASE("model: a target with no table yet is skipped, not fatal", "[tui][graph]") {
    // Registered but never loaded. Counting it throws, and one new target must
    // not blank the whole graph.
    auto q = [&](const std::string &sql) -> QueryResult {
        if (sql.find("erpl_rev_run_stats") != std::string::npos) return QueryResult{};
        if (sql.find("material_master") != std::string::npos)
            throw std::runtime_error("Catalog Error: Table with name material_master does not exist");
        QueryResult r;
        r.rows = {R"({"n":10})"};
        return r;
    };
    const auto got = tui::SampleCounts(q, {"stock_moves", "material_master"});
    REQUIRE(got.size() == 1);
    CHECK(got[0].target == "stock_moves");
}

TEST_CASE("model: a target name that is not a plain identifier is refused", "[tui][graph]") {
    // The name is interpolated into SQL because a table name cannot be bound.
    // The engine wrote these names, so they are already safe -- but a
    // hand-edited registry must not reach the query builder, and the check
    // that stops it needs a test of its own.
    std::vector<std::string> seen;
    auto q = [&](const std::string &sql) {
        seen.push_back(sql);
        return QueryResult{};
    };
    const auto got = tui::SampleCounts(q, {"a; DROP TABLE x"});
    CHECK(got.empty());
    // The fixed aggregate over the run statistics carries no target name and
    // is issued regardless; nothing built from the rejected one is.
    for (const auto &sql : seen) CHECK(sql.find("DROP TABLE") == std::string::npos);
}
