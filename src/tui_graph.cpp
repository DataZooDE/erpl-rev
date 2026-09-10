#include "tui_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>

namespace erpl_rev {
namespace tui {

double RateBucket::Total() const {
    double t = 0;
    for (const auto &r : rates) t += r.second;
    return t;
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
            const auto at_prev =
                std::find_if(prev.counts.begin(), prev.counts.end(),
                             [&](const std::pair<std::string, long long> &p) {
                                 return p.first == c.first;
                             });
            if (at_prev == prev.counts.end()) continue;

            // A count can legitimately fall: a reload truncates before it
            // loads. That is not negative throughput.
            const long long delta = c.second - at_prev->second;
            b.rates.emplace_back(c.first, delta > 0 ? static_cast<double>(delta) / dt : 0.0);
        }
        out.push_back(std::move(b));
    }
    return out;
}

double NiceCeiling(double peak) {
    if (peak <= 0) return 0.0;
    const double mag = std::pow(10.0, std::floor(std::log10(peak)));
    const double norm = peak / mag;   // in [1, 10)
    // 1 / 2 / 5 / 10 -- the steps a person reads without converting.
    const double step = norm <= 1.0 ? 1.0 : norm <= 2.0 ? 2.0 : norm <= 5.0 ? 5.0 : 10.0;
    return step * mag;
}

std::string FormatRate(double rows_per_sec) {
    char buf[32];
    if (rows_per_sec < 1000) {
        std::snprintf(buf, sizeof(buf), "%lld/s", static_cast<long long>(rows_per_sec + 0.5));
    } else if (rows_per_sec < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fk/s", rows_per_sec / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1fM/s", rows_per_sec / 1000000.0);
    }
    return buf;
}

std::vector<int> BandHeights(const std::vector<double> &rates, double ceiling, int height) {
    std::vector<int> out(rates.size(), 0);
    if (ceiling <= 0 || height <= 0 || rates.empty()) return out;

    const double total = std::accumulate(rates.begin(), rates.end(), 0.0);
    if (total <= 0) return out;

    // The height the TOTAL deserves, decided once. Everything below only
    // divides this up -- so the stack can never disagree with the bar it is.
    int bar = static_cast<int>(std::lround(total / ceiling * height));
    bar = std::clamp(bar, 0, height);
    if (bar == 0) return out;

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

        std::vector<double> rates;
        rates.reserve(names.size());
        for (const auto &nm : names) {
            double v = 0;
            for (const auto &r : b.rates)
                if (r.first == nm) v = r.second;
            rates.push_back(v);
        }

        // Stack from the baseline up, so the bands sit on one another and the
        // top of the stack is the total.
        const auto heights = BandHeights(rates, ceiling, py);
        int y = py - 1;
        for (size_t k = 0; k < heights.size() && y >= 0; ++k) {
            if (heights[k] <= 0) continue;
            const int top = std::max(0, y - heights[k] + 1);
            out.push_back({x, top, y, static_cast<int>(k)});
            y = top - 1;
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
