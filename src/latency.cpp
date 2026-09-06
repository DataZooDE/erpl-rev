#include "latency.hpp"

#include <cctype>
#include <cstdio>

#include "cycle.hpp"

namespace erpl_rev {

namespace {
std::string Lit(const std::string &v) {
    std::string out = "'";
    for (char c : v) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}
}  // namespace

LatencyStats GetLatencyStats(duckdb::Connection &con, const std::string &target,
                             long long window_secs) {
    LatencyStats s;
    const std::string log = cycle::ChangeLogName(target);

    auto exists = con.Query("SELECT count(*) FROM duckdb_tables() WHERE table_name=" + Lit(log));
    if (exists->HasError() || exists->RowCount() == 0 ||
        exists->GetValue(0, 0).GetValue<int64_t>() == 0)
        return s;   // never logged: no samples, which is NOT zero latency

    // A row with no source change time -- a snapshot or full-load row -- is
    // excluded rather than counted as instant, which would silently improve
    // every percentile.
    std::string where = "_commit_ts IS NOT NULL AND _applied_at IS NOT NULL";
    if (window_secs > 0)
        where += " AND _commit_ts >= now() - INTERVAL '" + std::to_string(window_secs) +
                 "' SECOND";

    // ONE SAMPLE PER CHANGE, at its FIRST apply.
    //
    // The safety overlap re-reads recently-changed rows on purpose and the keyed
    // merge absorbs the duplicates, so a single source change appears in the log
    // several times, each with a later _applied_at. Counting every row answers
    // "how long after it changed did we write this particular copy" -- a
    // question nobody asked, whose answer is dominated by the overlap window.
    // The promise is about how long until a change is VISIBLE, which is the
    // first apply. Measured on real data the two differed by 5x, in the
    // pessimistic direction.
    //
    // A change is identified by its key plus its source timestamp; without the
    // key, two rows that changed in the same second would collapse into one.
    std::string group_cols = "_commit_ts";
    {
        auto k = con.Query("SELECT keys FROM _erpl_rev_delta_state WHERE target=" + Lit(target));
        if (!k->HasError() && k->RowCount() > 0) {
            const auto keys = k->GetValue(0, 0).ToString();
            std::string cur;
            for (char c : keys + ",") {
                if (c == ',') {
                    if (!cur.empty()) group_cols += ", " + cur;
                    cur.clear();
                } else if (!std::isspace(static_cast<unsigned char>(c))) {
                    cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
        }
    }

    const std::string per_change =
        "(SELECT epoch(min(_applied_at)) - epoch(_commit_ts) AS lat, "
        "any_value(_op) AS _op FROM " + log + " WHERE " + where +
        " GROUP BY " + group_cols + ")";

    auto r = con.Query(
        "SELECT count(*), avg(lat), "
        // quantile_disc, not quantile_cont: a reported p95 should be a latency
        // that a change actually experienced, not an interpolation between two
        // that did. Interpolation also understates a sparse tail, which is the
        // half of the distribution this exists to expose.
        "quantile_disc(lat, 0.50), quantile_disc(lat, 0.95), quantile_disc(lat, 0.99), "
        "min(lat), max(lat), "
        "count(*) FILTER (WHERE _op='I'), count(*) FILTER (WHERE _op='U'), "
        "count(*) FILTER (WHERE _op='D') FROM " + per_change);
    if (r->HasError() || r->RowCount() == 0) return s;

    s.samples = r->GetValue(0, 0).GetValue<int64_t>();
    if (s.samples == 0) return s;

    auto num = [&](duckdb::idx_t c) {
        return r->GetValue(c, 0).IsNull() ? 0.0 : r->GetValue(c, 0).GetValue<double>();
    };
    s.has_data = true;
    s.mean = num(1);
    s.p50 = num(2);
    s.p95 = num(3);
    s.p99 = num(4);
    s.min = num(5);
    s.max = num(6);
    s.inserts = r->GetValue(7, 0).GetValue<int64_t>();
    s.updates = r->GetValue(8, 0).GetValue<int64_t>();
    s.deletes = r->GetValue(9, 0).GetValue<int64_t>();
    return s;
}

bool MeetsTarget(const LatencyStats &s, double p95_target, double p99_target) {
    // No data is not a pass. A harness that ran and captured nothing has proved
    // nothing, and reporting that as "meets target" is the worst possible answer.
    if (!s.has_data) return false;
    return s.p95 <= p95_target && s.p99 <= p99_target;
}

std::string Describe(const LatencyStats &s) {
    if (!s.has_data) return "no latency samples (nothing logged with a source change time)";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "n=%lld  p50=%.2fs  p95=%.2fs  p99=%.2fs  min=%.2fs  max=%.2fs  "
                  "mean=%.2fs  (I=%lld U=%lld D=%lld)",
                  static_cast<long long>(s.samples), s.p50, s.p95, s.p99, s.min, s.max, s.mean,
                  static_cast<long long>(s.inserts), static_cast<long long>(s.updates),
                  static_cast<long long>(s.deletes));
    return buf;
}

}  // namespace erpl_rev
