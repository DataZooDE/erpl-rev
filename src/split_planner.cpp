#include "split_planner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace erpl_rev {
namespace split {

namespace {

std::string Upper(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// Combine the portion's own predicate with the caller's filter. Dropping the
// caller's WHERE would replicate rows they excluded on purpose, and a row count
// would not reveal it.
std::string WithUserFilter(const std::string &predicate, const std::string &user_where) {
    if (user_where.empty()) return predicate;
    if (predicate.empty()) return user_where;
    return "(" + user_where + ") AND " + predicate;
}

// Cutting on a column the caller has also constrained produces portions that
// overlap or collapse to nothing, and it surfaces as missing rows long after the
// run. Refuse it rather than produce a plausible-looking plan.
void RefuseFilterOnPartitionColumn(const SplitRequest &r) {
    if (r.user_where.empty() || r.part_col.empty()) return;
    const auto where = Upper(r.user_where);
    const auto col = Upper(r.part_col);
    if (where.find(col) != std::string::npos)
        throw std::runtime_error(
            "split: the filter already constrains " + r.part_col +
            ", which is also the partition column -- the portions would overlap or "
            "be empty. Split on another column, or fold the filter into the ranges.");
}

int DaysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

std::string Dats(int y, int m, int d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", y, m, d);
    return buf;
}

// Half-open [from, to): a closed range on both ends double-counts every boundary
// day. Against an idempotent merge that is invisible until the target's counts
// are compared with the source's.
std::string HalfOpen(const std::string &col, const std::string &from, const std::string &to) {
    return col + " >= '" + from + "' AND " + col + " < '" + to + "'";
}

std::vector<Portion> FromRanges(const SplitRequest &r, const std::vector<Range> &ranges,
                                bool half_open) {
    std::vector<Portion> out;
    int n = 0;
    for (const auto &g : ranges) {
        Portion p;
        p.portion_no = ++n;
        p.predicate = WithUserFilter(
            half_open ? HalfOpen(r.part_col, g.from, g.to)
                      : r.part_col + " >= '" + g.from + "' AND " + r.part_col + " <= '" + g.to + "'",
            r.user_where);
        out.push_back(p);
    }
    return out;
}

// Buckets are the smallest thing the source can filter on, so a portion is a run
// of whole buckets. A single bucket bigger than the limit becomes its own
// oversized portion -- the honest answer, rather than dropping it.
std::vector<Portion> ByBuckets(const SplitRequest &r, long long limit) {
    std::vector<Portion> out;
    if (r.histogram.empty() || limit <= 0) return out;

    size_t i = 0;
    int n = 0;
    while (i < r.histogram.size()) {
        const size_t start = i;
        long long rows = 0;
        do {
            rows += r.histogram[i].rows;
            ++i;
        } while (i < r.histogram.size() && rows + r.histogram[i].rows <= limit);

        Portion p;
        p.portion_no = ++n;
        p.est_rows = rows;
        p.predicate = WithUserFilter(r.part_col + " >= '" + r.histogram[start].value + "' AND " +
                                         r.part_col + " <= '" + r.histogram[i - 1].value + "'",
                                     r.user_where);
        out.push_back(p);
    }
    return out;
}

}  // namespace

// Equal-width ranges over [range_min, range_max], as many as the row count and
// the per-portion limit imply. Boundaries are computed HERE, from facts ABAP
// supplied -- ABAP provides data, never a cut, which is what keeps one code path
// cutting every strategy.
//
// The arithmetic is on the numeric value, but the predicate keeps the source's
// text width: a NUMC document number compared as text sorts wrongly the moment
// a leading zero is dropped.
std::vector<Portion> ByEvenRanges(const SplitRequest &r, long long limit) {
    std::vector<Portion> out;
    if (limit <= 0 || r.total_rows <= 0 || r.range_min.empty() || r.range_max.empty())
        return out;
    // Bounds come from source DATA, not from a fixed vocabulary, and they land
    // in a predicate ABAP runs as dynamic Open SQL. A quote in one would end the
    // literal early. They are digit-checked above, so this is defence rather
    // than the only guard -- but the only guard should never be somewhere else.
    auto lit = [](const std::string &v) {
        std::string q;
        for (char c : v) { if (c == '\'') q += "''"; else q += c; }
        return q;
    };
    const bool numeric = r.range_min.find_first_not_of("0123456789") == std::string::npos &&
                         r.range_max.find_first_not_of("0123456789") == std::string::npos;
    if (!numeric) {
        // A non-numeric column cannot be cut arithmetically. One portion over
        // the whole range is honest; pretending otherwise would produce
        // boundaries that do not partition the data.
        Portion p;
        p.portion_no = 1;
        p.est_rows = r.total_rows;
        p.predicate = WithUserFilter(r.part_col + " >= '" + lit(r.range_min) + "' AND " +
                                         r.part_col + " <= '" + lit(r.range_max) + "'",
                                     r.user_where);
        out.push_back(p);
        return out;
    }
    const long long lo = std::stoll(r.range_min);
    const long long hi = std::stoll(r.range_max);
    const size_t width = r.range_min.size();
    long long n = (r.total_rows + limit - 1) / limit;
    if (n < 1) n = 1;
    if (hi < lo) return out;
    const long long span = hi - lo + 1;
    if (n > span) n = span;
    const long long step = (span + n - 1) / n;

    auto pad = [&](long long v) {
        std::string t = std::to_string(v);
        return t.size() >= width ? t : std::string(width - t.size(), '0') + t;
    };
    int no = 0;
    for (long long start = lo; start <= hi; start += step) {
        const long long end = std::min(start + step - 1, hi);
        Portion p;
        p.portion_no = ++no;
        p.est_rows = r.total_rows / n;
        p.predicate = WithUserFilter(r.part_col + " >= '" + lit(pad(start)) + "' AND " +
                                         r.part_col + " <= '" + lit(pad(end)) + "'",
                                     r.user_where);
        out.push_back(p);
    }
    return out;
}

std::vector<Portion> PlanSplit(const SplitRequest &r) {
    RefuseFilterOnPartitionColumn(r);

    switch (r.strategy) {
        case Strategy::List:
            return FromRanges(r, r.explicit_ranges, /*half_open=*/false);

        case Strategy::Fiscal:
            // ABAP resolved the fiscal-year variant into date ranges; from here
            // they are cut exactly like any other explicit range list.
            return FromRanges(r, r.periods, /*half_open=*/false);

        case Strategy::Records:
            if (!r.histogram.empty()) return ByBuckets(r, r.limit_rows);
            return ByEvenRanges(r, r.limit_rows);

        case Strategy::Size: {
            // Rows per portion derived from a measured bytes-per-row, taken from
            // a probe package rather than assumed.
            if (r.bytes_per_row <= 0 || r.limit_mb <= 0) return {};
            const long long rows = (r.limit_mb * 1024 * 1024) / r.bytes_per_row;
            if (!r.histogram.empty()) return ByBuckets(r, rows);
            return ByEvenRanges(r, rows);
        }

        case Strategy::Time: {
            std::vector<Range> ranges;
            if (r.time_from.size() < 8 || r.time_to.size() < 8) return {};
            int y = std::stoi(r.time_from.substr(0, 4));
            int m = std::stoi(r.time_from.substr(4, 2));
            int d = std::stoi(r.time_from.substr(6, 2));
            const std::string end = r.time_to;
            const std::string unit = r.time_unit.empty() ? "month" : r.time_unit;

            while (Dats(y, m, d) < end) {
                const std::string from = Dats(y, m, d);
                if (unit == "day") {
                    if (++d > DaysInMonth(y, m)) { d = 1; if (++m > 12) { m = 1; ++y; } }
                } else if (unit == "year") {
                    ++y; m = 1; d = 1;
                } else {
                    if (++m > 12) { m = 1; ++y; }
                    d = 1;
                }
                const std::string to = std::min(Dats(y, m, d), end);
                ranges.push_back({from, to});
            }

            std::vector<Portion> out;
            int n = 0;
            for (const auto &g : ranges) {
                Portion p;
                p.portion_no = ++n;
                p.predicate = WithUserFilter(HalfOpen(r.part_col, g.from, g.to), r.user_where);
                out.push_back(p);
            }
            return out;
        }

        case Strategy::Key:
        default:
            return FromRanges(r, r.explicit_ranges, /*half_open=*/false);
    }
}

}  // namespace split
}  // namespace erpl_rev
