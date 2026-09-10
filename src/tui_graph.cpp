#include "tui_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>

namespace erpl_rev {
namespace tui {

const char *OpGlyph(Op op) {
    // Direction is the mnemonic: a row arriving points up, a row leaving points
    // down, and a row changed in place is neither. Chosen over I/U/D letters
    // because a column of letters reads as text and stops looking like a bar.
    switch (op) {
        case Op::kInsert: return "▲";
        case Op::kUpdate: return "◆";
        case Op::kDelete: return "▼";
    }
    return " ";
}

const char *OpName(Op op) {
    switch (op) {
        case Op::kInsert: return "ins";
        case Op::kUpdate: return "upd";
        case Op::kDelete: return "del";
    }
    return "";
}

double OpRate::At(Op op) const {
    switch (op) {
        case Op::kInsert: return ins;
        case Op::kUpdate: return upd;
        case Op::kDelete: return del;
    }
    return 0;
}

double RateBucket::Total() const {
    double t = 0;
    for (const auto &r : rates) t += r.Total();
    return t;
}

double RateBucket::TotalFor(const std::string &target) const {
    for (const auto &r : rates)
        if (r.target == target) return r.Total();
    return 0;
}

std::vector<RateBucket> Rates(const std::vector<CountSample> &samples) {
    std::vector<RateBucket> out;
    if (samples.size() < 2) return out;   // one sample is a baseline, nothing more

    for (size_t i = 1; i < samples.size(); ++i) {
        const auto &prev = samples[i - 1];
        const auto &cur = samples[i];
        const double dt = cur.at_epoch - prev.at_epoch;
        // An interval too short to measure has no rate, and inventing one does
        // real damage: refresh() is reachable from the ticker AND from the key
        // handlers, so a keypress landing milliseconds after a tick can put two
        // samples either side of a single commit batch. Fifty thousand rows
        // over twenty milliseconds reads as 2.5M rows/s, the auto-scale lifts
        // the whole window to that, and every genuine bar rounds to zero points
        // for as long as the sample stays in the window. The graph draws
        // nothing, which looks exactly like the monitor having hung.
        //
        // Dropped rather than clamped: there is no honest number for this pair.
        // Negative dt is dropped by the same test -- two threads appending
        // means the history is not guaranteed monotonic.
        constexpr double kMinInterval = 0.5;
        if (dt < kMinInterval) continue;

        RateBucket b;
        for (const auto &c : cur.counts) {
            // Only targets present in BOTH samples have a rate. A target seen
            // for the first time here contributes nothing to this bucket --
            // its whole existing size is not throughput, and drawing it as
            // such is the single most flattering bug this file could have.
            const auto at_prev = std::find_if(
                prev.counts.begin(), prev.counts.end(),
                [&](const TargetCount &p) { return p.target == c.target; });
            if (at_prev == prev.counts.end()) continue;

            OpRate r;
            r.target = c.target;

            const long long dn = c.rows - at_prev->rows;
            const long long du = c.upd - at_prev->upd;
            const long long dd = c.del - at_prev->del;

            // Inserts from the ROW COUNT, which is the only signal that moves
            // while a load is still running -- a cycle reports nothing until it
            // finishes, so a graph fed from reports alone sits flat for the
            // whole load and then jumps.
            r.ins = dn > 0 ? static_cast<double>(dn) / dt : 0.0;

            // Deletes from whichever signal sees MORE of them: the count, which
            // sees a delete only when nothing else was inserted alongside it,
            // or the cycle's own report, which is exact.
            //
            // Under continuous traffic the count sees none at all. Twenty-two
            // inserts and seven deletes in one interval net to fifteen, and
            // fifteen is all a count can tell you -- so a table being written
            // and pruned at the same time, which is what an ordinary working
            // day looks like, drew no deletes whatsoever. The report fixes
            // that, and taking the larger of the two keeps a delete visible in
            // the interval it happened even when the cycle has not reported yet.
            const long long shrank = dn < 0 ? -dn : 0;
            const long long del = std::max(dd, shrank);
            r.del = del > 0 ? static_cast<double>(del) / dt : 0.0;

            // Updates from the report and only from the report: an update
            // changes no row count, so counting cannot see it at all.
            r.upd = du > 0 ? static_cast<double>(du) / dt : 0.0;

            // Reported INSERTS stay deliberately unused. A full load writes one
            // statistics row, at the end, carrying the whole million; adding it
            // to the count that already drew those rows arriving would draw
            // them twice, the second time as one spike at a rate the machine
            // never reached. Nothing is added back into the insert band for the
            // same reason: in an interval that both inserted and deleted, the
            // insert band stays NET, which is a lower bound and never an
            // invention. The exact figures are in the target table.

            b.rates.push_back(std::move(r));
        }
        out.push_back(std::move(b));
    }
    return out;
}

double ScaleFor(const std::vector<RateBucket> &buckets, int px) {
    if (buckets.empty() || px <= 0) return 0.0;
    // Only the buckets that are drawn. The history is longer than the window,
    // and scaling to a load that scrolled off the left is what made every
    // subsequent change round to nothing.
    const size_t n = std::min<size_t>(buckets.size(), static_cast<size_t>(px));
    double peak = 0;
    for (size_t i = buckets.size() - n; i < buckets.size(); ++i)
        peak = std::max(peak, buckets[i].Total());
    return NiceCeiling(peak);
}

double NiceCeiling(double peak) {
    if (peak <= 0) return 0.0;
    const double mag = std::pow(10.0, std::floor(std::log10(peak)));
    const double norm = peak / mag;   // in [1, 10)
    // 1 / 2 / 5 / 10 -- the steps a person reads without converting.
    const double step = norm <= 1.0 ? 1.0 : norm <= 2.0 ? 2.0 : norm <= 5.0 ? 5.0 : 10.0;
    return step * mag;
}

std::string FormatCount(double rows) {
    char buf[32];
    if (rows < 1000) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(rows + 0.5));
    } else if (rows < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fk", rows / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1fM", rows / 1000000.0);
    }
    return buf;
}

std::string FormatRate(double rows_per_sec) { return FormatCount(rows_per_sec) + "/s"; }

std::vector<int> BandHeights(const std::vector<double> &rates, double ceiling, int height) {
    std::vector<int> out(rates.size(), 0);
    if (ceiling <= 0 || height <= 0 || rates.empty()) return out;

    const double total = std::accumulate(rates.begin(), rates.end(), 0.0);
    if (total <= 0) return out;

    // The height the TOTAL deserves, decided once. Everything below only
    // divides this up -- so the stack can never disagree with the bar it is.
    int bar = static_cast<int>(std::lround(total / ceiling * height));
    bar = std::clamp(bar, 0, height);
    // Rounded UP, never away. On an axis set by a bulk load, every ordinary
    // change rounds to zero and the graph reads as idle while replication is
    // working. One cell says "some, below the resolution of this scale"; the
    // legend says how much.
    if (bar == 0) bar = 1;

    // Largest remainder: floor every share, then hand the leftover points to
    // whoever was cut by the most. Rounding each share on its own loses or
    // invents points, and at three equal rates in a ten-point bar it is
    // visibly wrong.
    std::vector<double> remainder(rates.size(), 0.0);
    int assigned = 0;
    for (size_t i = 0; i < rates.size(); ++i) {
        const double exact = rates[i] / total * bar;
        out[i] = static_cast<int>(std::floor(exact));
        remainder[i] = exact - out[i];
        assigned += out[i];
    }
    std::vector<size_t> order(rates.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return remainder[a] > remainder[b]; });
    for (size_t k = 0; assigned < bar && k < order.size(); ++k, ++assigned) out[order[k]]++;
    return out;
}

std::vector<BandSpan> BandSpans(const std::vector<RateBucket> &buckets,
                                const std::vector<std::string> &names, double ceiling,
                                int px, int py) {
    std::vector<BandSpan> out;
    if (buckets.empty() || names.empty() || ceiling <= 0 || px <= 0 || py <= 0) return out;

    // Newest on the right, which is the direction every monitor reads. An
    // older history than fits is cropped from the left rather than squeezed.
    const size_t n = std::min<size_t>(buckets.size(), static_cast<size_t>(px));
    for (size_t i = 0; i < n; ++i) {
        const auto &b = buckets[buckets.size() - n + i];
        const int x = px - static_cast<int>(n) + static_cast<int>(i);

        std::vector<double> totals;
        totals.reserve(names.size());
        for (const auto &nm : names) totals.push_back(b.TotalFor(nm));

        // Level one: divide the column between targets.
        const auto heights = BandHeights(totals, ceiling, py);

        // Stack from the baseline up, so the bands sit on one another and the
        // top of the stack is the total.
        int y = py - 1;
        for (size_t k = 0; k < heights.size() && y >= 0; ++k) {
            if (heights[k] <= 0) continue;

            const OpRate *r = nullptr;
            for (const auto &cand : b.rates)
                if (cand.target == names[k]) r = &cand;
            if (r == nullptr) continue;

            // Level two: divide this target's slice between its operations,
            // with the target's own total as the ceiling so the three parts sum
            // to exactly the height level one already granted it. Inserts sit
            // at the base and deletes on top: inserts are the bulk in almost
            // every workload, so putting them at the bottom keeps the part of
            // the bar that moves least at the part of the graph that moves
            // least.
            const std::vector<double> ops{r->ins, r->upd, r->del};
            const auto sub = BandHeights(ops, r->Total(), heights[k]);
            for (int o = 0; o < kOps && y >= 0; ++o) {
                if (sub[o] <= 0) continue;
                const int top = std::max(0, y - sub[o] + 1);
                out.push_back({x, top, y, static_cast<int>(k), static_cast<Op>(o)});
                y = top - 1;
            }
        }
    }
    return out;
}

int ColorSlotFor(const std::string &target, int palette_size) {
    if (palette_size <= 0) return 0;
    // FNV-1a rather than std::hash: this decides a colour a person will learn
    // to recognise, and std::hash is not required to agree between builds or
    // platforms. A target should be the same colour on every machine.
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : target) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return static_cast<int>(h % static_cast<uint64_t>(palette_size));
}

}  // namespace tui
}  // namespace erpl_rev
