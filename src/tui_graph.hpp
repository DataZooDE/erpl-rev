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
#include <utility>
#include <vector>

namespace erpl_rev {
namespace tui {

// How many rows each target held at one moment.
struct CountSample {
    double at_epoch = 0;
    std::vector<std::pair<std::string, long long>> counts;
};

// Rows per second per target, between two adjacent samples.
struct RateBucket {
    std::vector<std::pair<std::string, double>> rates;
    double Total() const;
};

// Differentiate a history of counts into rate buckets.
//
// N samples yield N-1 buckets: the first sample establishes a baseline and
// nothing more. Skipping that is the difference between "this target is
// replicating 7,700 rows a second" and "this target contains 100,000 rows" --
// the second rendered as the first is a spectacular-looking lie, and it is the
// state every already-populated target is in the moment the graph is opened.
//
// A count that went DOWN (a reload truncates before it loads) clamps to zero
// rather than rendering a negative band.
std::vector<RateBucket> Rates(const std::vector<CountSample> &samples);

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

// Stacked band heights for one column, in braille points.
//
// The bands must sum to exactly the height the TOTAL deserves: rounding each
// share independently loses or invents rows, and at three equal rates in a
// ten-point bar it is visibly wrong. Largest-remainder apportionment, which is
// the same rule used for seats in a parliament and for the same reason.
std::vector<int> BandHeights(const std::vector<double> &rates, double ceiling, int height);

// One drawn band: a vertical run of braille points in column x.
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
    int y_top = 0;      // canvas y grows downward, so top < bottom
    int y_bottom = 0;
    int band = 0;       // index into the caller's palette
};

// Lay out the whole graph: newest bucket at the right, bands stacked from the
// baseline up, at most `px` columns and `py` points tall.
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
