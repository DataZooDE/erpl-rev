// The throughput graph's arithmetic, tested without a terminal.
//
// Everything here is a way the graph could be confidently wrong: a bar that
// looks like 100,000 rows a second when nothing is moving, bands that do not
// add up to the total above them, colours that swap while someone is watching.
// A graph is a claim about numbers, and a wrong one is worse than none because
// it is believed.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "tui_graph.hpp"

using namespace erpl_rev;
using namespace erpl_rev::tui;

namespace {
CountSample S(double at, std::vector<std::pair<std::string, long long>> c) {
    CountSample s;
    s.at_epoch = at;
    s.counts = std::move(c);
    return s;
}
double RateOf(const RateBucket &b, const std::string &target) {
    for (const auto &r : b.rates)
        if (r.first == target) return r.second;
    return -1;
}
}  // namespace

TEST_CASE("graph: one sample is a baseline, not a bar", "[tui][graph]") {
    // THE defect this whole file exists for. Open the monitor on a target that
    // already holds 100,000 rows and the naive reading is "100,000 rows in this
    // bucket" -- a full-height bar, instantly, for a system doing nothing at
    // all. It looks like the best benchmark you have ever seen.
    const auto buckets = Rates({S(100, {{"stock_moves", 100000}})});
    CHECK(buckets.empty());
}

TEST_CASE("graph: a rate is the difference between two samples", "[tui][graph]") {
    const auto buckets = Rates({
        S(100, {{"stock_moves", 100000}}),
        S(102, {{"stock_moves", 115400}}),   // +15,400 in 2s
    });
    REQUIRE(buckets.size() == 1);
    CHECK(RateOf(buckets[0], "stock_moves") == 7700.0);
    CHECK(buckets[0].Total() == 7700.0);
}

TEST_CASE("graph: concurrent targets each contribute to the total", "[tui][graph]") {
    const auto buckets = Rates({
        S(100, {{"stock_moves", 0}, {"material_master", 0}}),
        S(102, {{"stock_moves", 12400}, {"material_master", 3000}}),
    });
    REQUIRE(buckets.size() == 1);
    CHECK(RateOf(buckets[0], "stock_moves") == 6200.0);
    CHECK(RateOf(buckets[0], "material_master") == 1500.0);
    CHECK(buckets[0].Total() == 7700.0);
}

TEST_CASE("graph: a target that shrank does not render a negative band", "[tui][graph]") {
    // A reload truncates before it loads, so between two samples a target can
    // legitimately hold fewer rows than it did. That is not negative throughput.
    const auto buckets = Rates({
        S(100, {{"stock_moves", 100000}}),
        S(102, {{"stock_moves", 0}}),
    });
    REQUIRE(buckets.size() == 1);
    CHECK(RateOf(buckets[0], "stock_moves") == 0.0);
}

TEST_CASE("graph: a target that appears mid-history starts from its own baseline",
          "[tui][graph]") {
    // Registering a second target while the graph is open must not draw its
    // whole existing size as one bucket of throughput.
    const auto buckets = Rates({
        S(100, {{"stock_moves", 1000}}),
        S(102, {{"stock_moves", 2000}, {"material_master", 50000}}),
        S(104, {{"stock_moves", 3000}, {"material_master", 51000}}),
    });
    REQUIRE(buckets.size() == 2);
    CHECK(RateOf(buckets[0], "material_master") == -1);   // absent, not 25000/s
    CHECK(RateOf(buckets[1], "material_master") == 500.0);
}

TEST_CASE("graph: bands sum to the height the total deserves", "[tui][graph]") {
    // Three equal rates in a ten-point bar cannot each have 3.33 points.
    // Rounding them independently yields 9 or 12 and the stack stops matching
    // the total drawn above it.
    const auto h = BandHeights({100, 100, 100}, /*ceiling=*/300, /*height=*/10);
    REQUIRE(h.size() == 3);
    CHECK(h[0] + h[1] + h[2] == 10);

    const auto half = BandHeights({100, 100, 100}, /*ceiling=*/600, /*height=*/10);
    CHECK(half[0] + half[1] + half[2] == 5);
}

TEST_CASE("graph: a rate too small to draw is not silently promoted", "[tui][graph]") {
    // A trickle beside a bulk load rounds to zero points, and that is correct.
    // Giving every non-zero rate a minimum of one point makes a 3-row/s target
    // look like a tenth of a 7,000-row/s one.
    const auto h = BandHeights({7000, 3}, /*ceiling=*/7000, /*height=*/10);
    REQUIRE(h.size() == 2);
    CHECK(h[0] == 10);
    CHECK(h[1] == 0);
}

TEST_CASE("graph: an idle graph is empty, not a division by zero", "[tui][graph]") {
    CHECK(NiceCeiling(0) == 0.0);
    const auto h = BandHeights({0, 0}, /*ceiling=*/0, /*height=*/10);
    REQUIRE(h.size() == 2);
    CHECK(h[0] == 0);
    CHECK(h[1] == 0);
}

TEST_CASE("graph: the axis snaps to a readable number", "[tui][graph]") {
    // 1/2/5 x 10^n, so the border label is a number a person can hold, and the
    // baseline stops moving on every frame.
    CHECK(NiceCeiling(7700) == 10000.0);
    CHECK(NiceCeiling(4200) == 5000.0);
    CHECK(NiceCeiling(1500) == 2000.0);
    CHECK(NiceCeiling(950) == 1000.0);
    CHECK(NiceCeiling(1) == 1.0);
}

TEST_CASE("graph: rates render in the border's budget", "[tui][graph]") {
    CHECK(FormatRate(0) == "0/s");
    CHECK(FormatRate(742) == "742/s");
    CHECK(FormatRate(7742) == "7.7k/s");
    CHECK(FormatRate(1500000) == "1.5M/s");
}

TEST_CASE("graph: a target keeps its colour when the table re-sorts", "[tui][graph]") {
    // The list is ordered by severity, so a target moves rows the moment it
    // starts failing. If colour came from the row index, every band in the
    // graph would change places at exactly the moment an operator looked at it
    // to find out what had changed.
    const int a1 = ColorSlotFor("stock_moves", 6);
    const int b1 = ColorSlotFor("material_master", 6);
    CHECK(a1 == ColorSlotFor("stock_moves", 6));
    CHECK(b1 == ColorSlotFor("material_master", 6));
    CHECK(a1 >= 0);
    CHECK(a1 < 6);
    CHECK(b1 >= 0);
    CHECK(b1 < 6);
}

TEST_CASE("graph: spans stack from the baseline and never leave the canvas", "[tui][graph]") {
    // This layout used to be done inside the canvas callback, capturing the
    // sample vectors by reference. FTXUI runs that callback during LAYOUT --
    // after the function that built it has returned -- so it read destroyed
    // vectors, and a destroyed vector's size is whatever was in that memory:
    // the draw loop never ended and the monitor's thread spun at 100% with the
    // display frozen. It presented as a hung query for half a day.
    //
    // Returning values instead makes the mistake unavailable, and makes this
    // testable at all.
    std::vector<RateBucket> buckets(1);
    buckets[0].rates = {{"a", 300}, {"b", 100}};
    const std::vector<std::string> names = {"a", "b"};

    const auto spans = BandSpans(buckets, names, /*ceiling=*/400, /*px=*/10, /*py=*/20);
    REQUIRE(spans.size() == 2);

    for (const auto &s : spans) {
        CHECK(s.x >= 0);
        CHECK(s.x < 10);
        CHECK(s.y_top >= 0);
        CHECK(s.y_bottom < 20);
        CHECK(s.y_top <= s.y_bottom);
    }
    // Newest bucket at the right edge.
    CHECK(spans[0].x == 9);
    // Stacked, not overlapping: the second band sits directly above the first.
    CHECK(spans[1].y_bottom == spans[0].y_top - 1);
    // The first band is drawn from the baseline.
    CHECK(spans[0].y_bottom == 19);
}

TEST_CASE("graph: nothing to draw yields no spans", "[tui][graph]") {
    CHECK(BandSpans({}, {"a"}, 100, 10, 20).empty());
    std::vector<RateBucket> idle(2);
    idle[0].rates = {{"a", 0}};
    idle[1].rates = {{"a", 0}};
    CHECK(BandSpans(idle, {"a"}, 0, 10, 20).empty());
}
