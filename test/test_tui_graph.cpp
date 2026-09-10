// The throughput graph's arithmetic, tested without a terminal.
//
// Everything here is a way the graph could be confidently wrong: a bar that
// looks like 100,000 rows a second when nothing is moving, bands that do not
// add up to the total above them, colours that swap while someone is watching,
// a million rows drawn twice. A graph is a claim about numbers, and a wrong one
// is worse than none because it is believed.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "tui_graph.hpp"

using namespace erpl_rev;
using namespace erpl_rev::tui;

namespace {
// at, then one target's (rows, cumulative ins, cumulative upd, cumulative del).
CountSample S(double at, std::vector<TargetCount> c) {
    CountSample s;
    s.at_epoch = at;
    s.counts = std::move(c);
    return s;
}
TargetCount T(std::string name, long long rows, long long ins = 0, long long upd = 0,
              long long del = 0) {
    TargetCount t;
    t.target = std::move(name);
    t.rows = rows;
    t.ins = ins;
    t.upd = upd;
    t.del = del;
    return t;
}
const OpRate *RateOf(const RateBucket &b, const std::string &target) {
    for (const auto &r : b.rates)
        if (r.target == target) return &r;
    return nullptr;
}
}  // namespace

TEST_CASE("graph: one sample is a baseline, not a bar", "[tui][graph]") {
    // THE defect this whole file exists for. Open the monitor on a target that
    // already holds 100,000 rows and the naive reading is "100,000 rows in this
    // bucket" -- a full-height bar, instantly, for a system doing nothing at
    // all. It looks like the best benchmark you have ever seen.
    const auto buckets = Rates({S(100, {T("stock_moves", 100000)})});
    CHECK(buckets.empty());
}

TEST_CASE("graph: a rate is the difference between two samples", "[tui][graph]") {
    const auto buckets = Rates({
        S(100, {T("stock_moves", 100000)}),
        S(102, {T("stock_moves", 115400)}),   // +15,400 in 2s
    });
    REQUIRE(buckets.size() == 1);
    REQUIRE(RateOf(buckets[0], "stock_moves") != nullptr);
    CHECK(RateOf(buckets[0], "stock_moves")->ins == 7700.0);
    CHECK(buckets[0].Total() == 7700.0);
}

TEST_CASE("graph: rows arriving are inserts, rows leaving are deletes", "[tui][graph]") {
    // The row count is the only signal that moves DURING a load, so it is what
    // the insert and delete bands are made of. A count that falls is not
    // negative throughput -- it is rows being removed, which is exactly what
    // the archiving step in the demo does and what the trigger tier exists for.
    const auto up = Rates({
        S(100, {T("t", 1000)}),
        S(102, {T("t", 1600)}),
    });
    REQUIRE(up.size() == 1);
    CHECK(RateOf(up[0], "t")->ins == 300.0);
    CHECK(RateOf(up[0], "t")->del == 0.0);

    const auto down = Rates({
        S(100, {T("t", 1000)}),
        S(102, {T("t", 600)}),
    });
    REQUIRE(down.size() == 1);
    CHECK(down[0].Total() == 200.0);
    CHECK(RateOf(down[0], "t")->del == 200.0);
    CHECK(RateOf(down[0], "t")->ins == 0.0);
}

TEST_CASE("graph: deletes stay visible under traffic that also inserts",
          "[tui][graph]") {
    // The defect the closing workload exposed. Twenty-two inserts and seven
    // deletes in one interval net to a count that rose by fifteen, and fifteen
    // is everything a count can tell you -- so a table being written and pruned
    // at the same time, which is what an ordinary working day looks like, drew
    // no deletes at all. Every delete band in the earlier beats had only
    // appeared because nothing was being inserted alongside it.
    const auto buckets = Rates({
        S(100, {T("t", 1000, /*ins=*/0, /*upd=*/0, /*del=*/0)}),
        S(102, {T("t", 1015, /*ins=*/0, /*upd=*/0, /*del=*/7)}),
    });
    REQUIRE(buckets.size() == 1);
    CHECK(RateOf(buckets[0], "t")->del == 3.5);    // 7 reported, in 2s
    // The insert band stays NET: fifteen, not the twenty-two that actually
    // happened. A lower bound is honest; adding the deletes back would invent
    // rows whenever a cycle reports late.
    CHECK(RateOf(buckets[0], "t")->ins == 7.5);
}

TEST_CASE("graph: a delete is visible before the cycle has reported it",
          "[tui][graph]") {
    // The count is the faster of the two signals and the report is the exact
    // one. Taking whichever sees MORE deletes means a delete never waits for a
    // cycle to finish before it appears, and never disappears once it has.
    const auto counted = Rates({
        S(100, {T("t", 1000)}),
        S(102, {T("t", 998)}),          // count fell; nothing reported yet
    });
    REQUIRE(counted.size() == 1);
    CHECK(RateOf(counted[0], "t")->del == 1.0);

    const auto reported = Rates({
        S(100, {T("t", 1000, 0, 0, 0)}),
        S(102, {T("t", 998, 0, 0, 2)}),   // and now the cycle says so too
    });
    CHECK(RateOf(reported[0], "t")->del == 1.0);   // not counted twice
}

TEST_CASE("graph: an update is visible even though it changes no row count",
          "[tui][graph]") {
    // Counting cannot see an update at all: the table is exactly as big
    // afterwards. Without the reported counter a cycle that updated two
    // thousand rows draws nothing, and the monitor says a busy target is idle
    // -- which is the class of bug that made trigger CDC look like it worked.
    const auto buckets = Rates({
        S(100, {T("t", 5000, /*ins=*/0, /*upd=*/0)}),
        S(102, {T("t", 5000, /*ins=*/0, /*upd=*/2000)}),
    });
    REQUIRE(buckets.size() == 1);
    CHECK(RateOf(buckets[0], "t")->upd == 1000.0);
    CHECK(RateOf(buckets[0], "t")->ins == 0.0);
    CHECK(buckets[0].Total() == 1000.0);
}

TEST_CASE("graph: a finished load does not draw its rows a second time", "[tui][graph]") {
    // The trap that decided the whole design. A full load writes ONE
    // run-statistics row, at the end, carrying the entire million. The rows
    // themselves were already drawn as they arrived, bucket by bucket, from the
    // row count. Differentiating the reported INSERT counter as well would draw
    // them again -- one fabricated spike, several seconds after the load
    // finished, at a rate the machine never reached.
    //
    // So reported inserts and deletes are ignored on purpose. Only the reported
    // UPDATE counter is read, because it is the only one counting cannot see.
    const auto buckets = Rates({
        // The load runs: rows arrive, nothing has been reported yet.
        S(100, {T("t", 0, /*ins=*/0)}),
        S(102, {T("t", 200000, /*ins=*/0)}),
        // The cycle finishes: the counter jumps by the whole million, and the
        // last of the rows land.
        S(104, {T("t", 1000000, /*ins=*/1000000)}),
        // Idle.
        S(106, {T("t", 1000000, /*ins=*/1000000)}),
    });
    REQUIRE(buckets.size() == 3);
    CHECK(buckets[0].Total() == 100000.0);   // 200k rows in 2s
    CHECK(buckets[1].Total() == 400000.0);   // the remaining 800k in 2s
    CHECK(buckets[2].Total() == 0.0);        // idle means idle
}

TEST_CASE("graph: concurrent targets each contribute to the total", "[tui][graph]") {
    const auto buckets = Rates({
        S(100, {T("stock_moves", 0), T("material_master", 0)}),
        S(102, {T("stock_moves", 12400), T("material_master", 3000)}),
    });
    REQUIRE(buckets.size() == 1);
    CHECK(buckets[0].TotalFor("stock_moves") == 6200.0);
    CHECK(buckets[0].TotalFor("material_master") == 1500.0);
    CHECK(buckets[0].Total() == 7700.0);
}

TEST_CASE("graph: a target that appears mid-history starts from its own baseline",
          "[tui][graph]") {
    // Registering a second target while the graph is open must not draw its
    // whole existing size as one bucket of throughput.
    const auto buckets = Rates({
        S(100, {T("stock_moves", 1000)}),
        S(102, {T("stock_moves", 2000), T("material_master", 50000)}),
        S(104, {T("stock_moves", 3000), T("material_master", 51000)}),
    });
    REQUIRE(buckets.size() == 2);
    CHECK(RateOf(buckets[0], "material_master") == nullptr);   // absent, not 25000/s
    CHECK(RateOf(buckets[1], "material_master")->ins == 500.0);
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

TEST_CASE("graph: a bucket that did anything is never drawn as nothing",
          "[tui][graph]") {
    // The defect the recording made obvious. On an axis set by a bulk load,
    // every ordinary change afterwards rounds to zero cells: two updates a
    // second against 100,000 is four ten-thousandths of the height. The graph
    // showed the load and then NOTHING for the rest of the clip, while three
    // changes replicated in plain view on the other pane.
    //
    // Rounded up to one cell. That cell is an indicator, not a measurement --
    // it says "some, below the resolution of this scale" -- and the legend
    // carries the figure.
    const auto tiny = BandHeights({2}, /*ceiling=*/100000, /*height=*/9);
    REQUIRE(tiny.size() == 1);
    CHECK(tiny[0] == 1);

    // Nothing happened is still nothing. The floor separates "too small to
    // measure" from "idle", which is the whole reason it is worth having.
    const auto idle = BandHeights({0}, /*ceiling=*/100000, /*height=*/9);
    CHECK(idle[0] == 0);
}

TEST_CASE("graph: the scale is the window that is drawn, and comes down with it",
          "[tui][graph]") {
    // Rescaling happens because the load LEAVES, not because a rule decided the
    // load had stopped mattering. While it is on screen it is the top of the
    // axis and everything else is honestly in proportion to it; once it scrolls
    // out of the drawn window the axis is whatever is left.
    std::vector<RateBucket> b;
    auto push = [&](double rate) {
        OpRate r;
        r.target = "t";
        r.ins = rate;
        RateBucket bucket;
        bucket.rates = {r};
        b.push_back(bucket);
    };
    for (int i = 0; i < 4; ++i) push(90000);
    for (int i = 0; i < 6; ++i) push(2);

    // A window wide enough to hold the load is scaled by the load.
    CHECK(ScaleFor(b, /*px=*/20) == 100000.0);
    // One that only reaches the six newest buckets is not.
    CHECK(ScaleFor(b, /*px=*/6) == 2.0);
    CHECK(ScaleFor(b, /*px=*/0) == 0.0);
    CHECK(ScaleFor({}, /*px=*/20) == 0.0);
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
    // The same abbreviation without the unit, three to a table column.
    CHECK(FormatCount(0) == "0");
    CHECK(FormatCount(1000000) == "1.0M");
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

TEST_CASE("graph: each operation has its own glyph, and they are distinct",
          "[tui][graph]") {
    // Colour is spent on which TARGET, so the operation has nothing left but
    // shape. Two operations sharing a glyph would silently merge two different
    // claims into one band.
    const std::string i = OpGlyph(Op::kInsert);
    const std::string u = OpGlyph(Op::kUpdate);
    const std::string d = OpGlyph(Op::kDelete);
    CHECK(i != u);
    CHECK(u != d);
    CHECK(i != d);
    CHECK_FALSE(i.empty());
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
    OpRate a, b;
    a.target = "a";
    a.ins = 300;
    b.target = "b";
    b.ins = 100;
    buckets[0].rates = {a, b};
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

TEST_CASE("graph: one target's operations stack inside the height it was given",
          "[tui][graph]") {
    // Two levels of apportionment -- targets, then operations within a target
    // -- and the second must sum to exactly what the first granted. If it does
    // not, a stack of three operations is taller or shorter than the single
    // band it replaced, and the graph stops agreeing with the total in its own
    // border.
    std::vector<RateBucket> buckets(1);
    OpRate r;
    r.target = "t";
    r.ins = 200;
    r.upd = 100;
    r.del = 100;
    buckets[0].rates = {r};

    const auto spans = BandSpans(buckets, {"t"}, /*ceiling=*/400, /*px=*/1, /*py=*/20);
    REQUIRE(spans.size() == 3);

    int cells = 0;
    for (const auto &s : spans) cells += s.y_bottom - s.y_top + 1;
    CHECK(cells == 20);   // the whole column: 400 of a 400 ceiling

    // Inserts at the base, deletes on top -- inserts are the bulk of almost
    // every workload, so the part of the bar that moves least sits at the part
    // of the graph that moves least.
    CHECK(spans[0].op == Op::kInsert);
    CHECK(spans[0].y_bottom == 19);
    CHECK(spans[1].op == Op::kUpdate);
    CHECK(spans[2].op == Op::kDelete);
    CHECK(spans[2].y_top == 0);
    // Half the column is inserts.
    CHECK(spans[0].y_bottom - spans[0].y_top + 1 == 10);
}

TEST_CASE("graph: an operation with no rows draws no band", "[tui][graph]") {
    // A target that only ever inserts must not carry a permanent one-cell
    // delete band at the top of its bar. Every glyph on screen has to mean
    // that something actually happened.
    std::vector<RateBucket> buckets(1);
    OpRate r;
    r.target = "t";
    r.ins = 400;
    buckets[0].rates = {r};

    const auto spans = BandSpans(buckets, {"t"}, /*ceiling=*/400, /*px=*/1, /*py=*/8);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].op == Op::kInsert);
}

TEST_CASE("graph: nothing to draw yields no spans", "[tui][graph]") {
    CHECK(BandSpans({}, {"a"}, 100, 10, 20).empty());
    std::vector<RateBucket> idle(2);
    OpRate z;
    z.target = "a";
    idle[0].rates = {z};
    idle[1].rates = {z};
    CHECK(BandSpans(idle, {"a"}, 0, 10, 20).empty());
}

TEST_CASE("graph: two samples an instant apart are not a rate", "[tui][graph]") {
    // refresh() is reachable from the ticker AND from the key handlers, so a
    // keypress landing a few milliseconds after a tick can straddle one commit
    // batch: 50,000 rows over 0.02s reads as 2.5M rows/s. NiceCeiling then
    // scales the whole window to that, every real bar rounds to zero points,
    // and the graph draws nothing for as long as the sample stays in the
    // window -- the same symptom as a hang, from a single stray sample.
    //
    // Below the floor the pair is dropped rather than clamped: an interval too
    // short to measure has no rate, and inventing one is what caused the
    // damage.
    const auto buckets = Rates({
        S(100.00, {T("stock_moves", 100000)}),
        S(100.02, {T("stock_moves", 150000)}),   // 20ms later, 50k more rows
    });
    CHECK(buckets.empty());
}

TEST_CASE("graph: samples that arrive out of order are ignored, not negated",
          "[tui][graph]") {
    // Two threads appending means the history is not guaranteed monotonic.
    // A pair that goes backwards in time must not produce a negative dt.
    const auto buckets = Rates({
        S(100, {T("a", 1000)}),
        S(98, {T("a", 2000)}),    // earlier than its predecessor
        S(102, {T("a", 3000)}),
    });
    for (const auto &b : buckets) CHECK(b.Total() >= 0);
}
