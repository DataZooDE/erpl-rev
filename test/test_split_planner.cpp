// Mass-execution split strategies.
//
// Parallel full load could only cut a source by numeric key range. The portion
// list generated here is persisted before any worker starts, which is what makes
// a failed run restartable: the coordinator knows which portions finished.
//
// ABAP is a data provider, not a decision maker. Fiscal periods need T009B and
// a key histogram needs a GROUP BY against the source -- both come back as data,
// and every strategy is cut here, by the same code.

#include <catch2/catch_test_macros.hpp>

#include <numeric>

#include "split_planner.hpp"

using namespace erpl_rev::split;

namespace {
SplitRequest Base() {
    SplitRequest r;
    r.part_col = "belnr";
    return r;
}
}  // namespace

TEST_CASE("split: an explicit range list is passed through unchanged", "[split]") {
    auto r = Base();
    r.strategy = Strategy::List;
    // Values arrive unquoted; the planner owns the SQL quoting, so a caller
    // cannot half-quote its way into a broken predicate.
    r.explicit_ranges = {{"0000000001", "0000005000"}, {"0000005001", "0000009999"}};
    const auto p = PlanSplit(r);
    REQUIRE(p.size() == 2);
    CHECK(p[0].predicate == "belnr >= '0000000001' AND belnr <= '0000005000'");
    CHECK(p[1].portion_no == 2);
}

TEST_CASE("split: by record count cuts on the histogram, not on guesses", "[split]") {
    // ABAP ships count(*) GROUP BY bucket; the server decides where the cuts go.
    auto r = Base();
    r.strategy = Strategy::Records;
    r.limit_rows = 1000;
    r.histogram = {{"A", 400}, {"B", 400}, {"C", 400}, {"D", 400}};
    const auto p = PlanSplit(r);
    // 1600 rows at 1000 per portion: two portions, and no bucket is split
    // across them because a bucket is the smallest unit the source can filter.
    REQUIRE(p.size() == 2);
    CHECK(p[0].est_rows == 800);
    CHECK(p[1].est_rows == 800);
}

TEST_CASE("split: a single oversized bucket becomes its own portion", "[split]") {
    // It cannot be cut finer, so the honest answer is one portion that is too
    // big -- not a silently dropped bucket.
    auto r = Base();
    r.strategy = Strategy::Records;
    r.limit_rows = 100;
    r.histogram = {{"BIG", 10000}, {"small", 10}};
    const auto p = PlanSplit(r);
    REQUIRE(p.size() == 2);
    CHECK(p[0].est_rows == 10000);
}

TEST_CASE("split: by size derives a row count from measured bytes per row", "[split]") {
    auto r = Base();
    r.strategy = Strategy::Size;
    r.limit_mb = 10;
    r.bytes_per_row = 1000;         // 10 MB -> ~10485 rows
    r.histogram = {{"A", 5000}, {"B", 5000}, {"C", 5000}};
    const auto p = PlanSplit(r);
    CHECK(p.size() == 2);
}

TEST_CASE("split: by time field cuts whole days, months or years", "[split]") {
    auto r = Base();
    r.strategy = Strategy::Time;
    r.part_col = "budat";
    r.time_from = "20260101";
    r.time_to = "20260401";

    r.time_unit = "month";
    const auto months = PlanSplit(r);
    CHECK(months.size() == 3);      // Jan, Feb, Mar
    CHECK(months[0].predicate == "budat >= '20260101' AND budat < '20260201'");
    CHECK(months[2].predicate == "budat >= '20260301' AND budat < '20260401'");

    r.time_unit = "year";
    r.time_to = "20280101";
    CHECK(PlanSplit(r).size() == 2);
}

TEST_CASE("split: a time range is half-open so no row lands in two portions", "[split]") {
    // The bug this prevents: >= / <= on both ends double-counts every boundary
    // day, which on an idempotent merge is invisible until the counts are
    // compared against the source.
    auto r = Base();
    r.strategy = Strategy::Time;
    r.part_col = "budat";
    r.time_unit = "day";
    r.time_from = "20260101";
    r.time_to = "20260104";
    const auto p = PlanSplit(r);
    REQUIRE(p.size() == 3);
    for (const auto &portion : p) {
        CHECK(portion.predicate.find(">=") != std::string::npos);
        CHECK(portion.predicate.find("<") != std::string::npos);
        CHECK(portion.predicate.find("<=") == std::string::npos);
    }
}

TEST_CASE("split: fiscal periods come from ABAP as resolved date ranges", "[split]") {
    // The fiscal-year variant lives in SAP, so ABAP resolves it and ships the
    // ranges. The server then cuts them with the same code path as Time --
    // which is what stops a second, subtly different implementation existing.
    auto r = Base();
    r.strategy = Strategy::Fiscal;
    r.part_col = "budat";
    r.periods = {{"20260101", "20260131"}, {"20260201", "20260228"}};
    const auto p = PlanSplit(r);
    REQUIRE(p.size() == 2);
    CHECK(p[0].predicate == "budat >= '20260101' AND budat <= '20260131'");
}

TEST_CASE("split: a user filter is combined, never dropped", "[split]") {
    // Silently ignoring the caller's WHERE would replicate rows they excluded on
    // purpose -- and they would not find out from a row count.
    auto r = Base();
    r.strategy = Strategy::List;
    r.explicit_ranges = {{"A", "B"}};
    r.user_where = "bukrs = '1000'";
    const auto p = PlanSplit(r);
    REQUIRE(p.size() == 1);
    CHECK(p[0].predicate.find("bukrs = '1000'") != std::string::npos);
    CHECK(p[0].predicate.find("belnr >= 'A'") != std::string::npos);
}

TEST_CASE("split: a filter that already constrains the partition column is refused",
          "[split]") {
    // Cutting on a column the caller has also filtered gives portions that
    // overlap or vanish, and the failure looks like missing data much later.
    auto r = Base();
    r.strategy = Strategy::List;
    r.explicit_ranges = {{"A", "B"}};
    r.user_where = "BELNR > '0000001000'";
    CHECK_THROWS(PlanSplit(r));
}

TEST_CASE("split: portions are numbered from one and contiguous", "[split]") {
    auto r = Base();
    r.strategy = Strategy::Records;
    r.limit_rows = 10;
    r.histogram = {{"A", 10}, {"B", 10}, {"C", 10}, {"D", 10}, {"E", 10}};
    const auto p = PlanSplit(r);
    for (size_t i = 0; i < p.size(); ++i) CHECK(p[i].portion_no == static_cast<int>(i) + 1);
}

TEST_CASE("split: no strategy yields no portions rather than one silent portion",
          "[split]") {
    auto r = Base();
    r.strategy = Strategy::Records;
    r.limit_rows = 100;
    CHECK(PlanSplit(r).empty());   // no histogram: nothing to cut
}

TEST_CASE("split: every row of the histogram ends up in exactly one portion",
          "[split]") {
    // The invariant that matters: a split that loses a bucket loses data, and a
    // split that repeats one does redundant work.
    auto r = Base();
    r.strategy = Strategy::Records;
    r.limit_rows = 333;
    for (int i = 0; i < 20; ++i) r.histogram.push_back({"b" + std::to_string(i), 100});

    const auto p = PlanSplit(r);
    const auto total = std::accumulate(p.begin(), p.end(), 0LL,
                                       [](long long a, const Portion &x) { return a + x.est_rows; });
    CHECK(total == 2000);
}
