// Throughput graph geometry: samples in, bar heights out.
//
// Pure on purpose. There is no FTXUI here and no DuckDB -- the terminal half
// lives in cmd_top.cpp, the query half in tui_model.cpp, and the arithmetic
// that decides what a viewer actually sees lives here where it can be tested
// without either. The same split the rest of the TUI already uses.
//
// What the graph measures, and what it does not: erpl-rev has no internal
// throughput meter. A full load writes ONE run-statistics row, at the end, so a
// graph fed from those sits flat for the whole load and then jumps. Instead the
// monitor samples each target's row COUNT on every refresh and differentiates
// it. That is an observation of rows arriving, which is honest, and it has two
// consequences that shape everything below:
//
//   - a rate needs TWO samples, so a target's first sample must produce no bar;
//   - the resolution is the refresh interval, not the engine's real granularity.
#pragma once

#include <string>
#include <vector>

namespace erpl_rev {
namespace tui {

// What a replication does to a row. Drawn as a glyph, never as a colour:
// colour is spent on WHICH TARGET, because several concurrent replications is
// the thing this graph exists to show.
enum class Op { kInsert = 0, kUpdate = 1, kDelete = 2 };
constexpr int kOps = 3;

// The glyph for an operation. One place, so the graph, the legend and the
// target table cannot disagree about what a triangle means.
const char *OpGlyph(Op op);
const char *OpName(Op op);

// One target at one moment: how many rows it holds, and how many rows every
// cycle it has ever run reported applying.
struct TargetCount {
    std::string target;
    long long rows = 0;                     // count(*) on the target table
    long long ins = 0, upd = 0, del = 0;    // CUMULATIVE, from the run statistics
};

struct CountSample {
    double at_epoch = 0;
    std::vector<TargetCount> counts;
};

// Rows per second, per target, per operation, between two adjacent samples.
struct OpRate {
    std::string target;
    double ins = 0, upd = 0, del = 0;
    double Total() const { return ins + upd + del; }
    double At(Op op) const;
};

struct RateBucket {
    std::vector<OpRate> rates;
    double Total() const;
    double TotalFor(const std::string &target) const;
};

// Differentiate a history of counts into rate buckets, split by operation.
//
// N samples yield N-1 buckets: the first sample establishes a baseline and
// nothing more. Skipping that is the difference between "this target is
// replicating 7,700 rows a second" and "this target contains 100,000 rows" --
// the second rendered as the first is a spectacular-looking lie, and it is the
// state every already-populated target is in the moment the graph is opened.
//
// HOW THE THREE OPERATIONS ARE DERIVED, which is the whole design:
//
//   insert = max(+delta rows, 0)                      from the row count
//   delete = max(delta reported deletes, -delta rows) from either
//   update = delta of reported updates                from the run statistics
//
// The count is differentiated for inserts because it is the only signal that
// moves DURING a load: a cycle reports nothing until it finishes, so a graph
// fed from reports alone sits flat for the whole load and then jumps.
//
// Updates come from the report because a count cannot see them at all, and
// deletes from whichever of the two sees more. Under continuous traffic the
// count sees no deletes: twenty-two inserts and seven deletes in one interval
// net to fifteen, and fifteen is all a count can say -- so a table being
// written and pruned at once, which is what an ordinary working day looks
// like, drew no deletes whatsoever. Taking the larger of the two keeps a delete
// visible in the interval it happened even before the cycle has reported it.
//
// Reported INSERTS stay unused, and nothing is ever added back into the insert
// band. A full load writes one statistics row, at the end, carrying the whole
// million; adding it to the count that already drew those rows arriving would
// draw them twice, the second time as one fabricated spike. So in an interval
// that both inserted and deleted, the insert band is NET -- a lower bound,
// never an invention. The target table carries the exact per-cycle figures.
std::vector<RateBucket> Rates(const std::vector<CountSample> &samples);

// The vertical scale for a graph: the peak of the buckets actually DRAWN.
//
// Not of the whole history, which is longer than the window -- so when a bulk
// load scrolls off the left the axis comes down on its own, with no rule about
// when to rescale and nothing on screen that outranks the scale it is drawn
// against.
//
// This replaced a scale that followed only the recent past and CLIPPED whatever
// exceeded it. Clipping bought the small traffic its height at the price of
// drawing finished loads at a height they never had, capped with a marker to
// say so -- a mark on almost every bar, explaining away a distortion that did
// not need to exist. Nothing here is ever truncated: the tallest bar in the
// window is the top of the axis, and everything else is honestly in proportion
// to it. Small traffic is kept visible by the floor in BandHeights instead.
double ScaleFor(const std::vector<RateBucket> &buckets, int px);

// The smallest "nice" number >= peak, from the 1/2/5 x 10^n series.
//
// The axis label has to be readable and the graph has to stop rescaling on
// every frame -- a chart whose baseline moves continuously cannot be read at
// all. Returns 0 for a peak of 0, so an idle graph is empty rather than
// dividing by zero.
double NiceCeiling(double peak);

// 7742 -> "7.7k", 1500000 -> "1.5M". Three significant characters plus a unit,
// because it is rendered into a border with a fixed budget.
std::string FormatRate(double rows_per_sec);
// The same abbreviation without the unit, for the per-operation counters that
// sit three-to-a-column in the target table.
std::string FormatCount(double rows);

// Stacked band heights for one column, in cells.
//
// The bands must sum to exactly the height the TOTAL deserves: rounding each
// share independently loses or invents rows, and at three equal rates in a
// ten-point bar it is visibly wrong. Largest-remainder apportionment, which is
// the same rule used for seats in a parliament and for the same reason.
//
// Used twice, at both levels of the stack: once to divide a column between
// targets, then once inside each target to divide it between operations. The
// second call passes the target's own total as the ceiling, so the operation
// bands sum to exactly the height the target was already given and the two
// levels cannot disagree.
std::vector<int> BandHeights(const std::vector<double> &rates, double ceiling, int height);

// One drawn band: a vertical run of cells in column x, belonging to one target
// and one operation.
//
// Computed as VALUES rather than drawn straight onto a canvas, because FTXUI's
// canvas() stores its callback and runs it during layout -- after the function
// that built it has returned. A callback capturing the sample vectors by
// reference reads them destroyed, and a destroyed vector's size is whatever is
// in that memory: the draw loop then never ends and the monitor's thread spins
// at 100% with the display frozen. That looks exactly like a hung query.
//
// Returning plain data makes the mistake unavailable: there is nothing to
// dangle, and the geometry becomes testable without a terminal.
struct BandSpan {
    int x = 0;
    int y_top = 0;      // y grows downward, so top < bottom
    int y_bottom = 0;
    int band = 0;       // index into the caller's palette, i.e. which target
    Op op = Op::kInsert;
};

// Lay out the whole graph: newest bucket at the right, targets stacked from the
// baseline up, and each target's slice split again by operation.
//
// `names` fixes both the band order and the palette index, so a target keeps
// its colour and its place in the stack from frame to frame.
std::vector<BandSpan> BandSpans(const std::vector<RateBucket> &buckets,
                                const std::vector<std::string> &names, double ceiling,
                                int px, int py);

// A palette slot for a target, derived from its NAME.
//
// Never from its position: the target list is sorted by severity
// (tui::SortForOperator), so an index-keyed palette makes every colour change
// places the moment a target starts failing -- while the operator is watching
// the graph to find out what changed.
int ColorSlotFor(const std::string &target, int palette_size);

}  // namespace tui
}  // namespace erpl_rev
