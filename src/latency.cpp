#include "latency.hpp"

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

    auto r = con.Query(
        "SELECT count(*), "
        "avg(epoch(_applied_at) - epoch(_commit_ts)), "
        // quantile_disc, not quantile_cont: a reported p95 should be a latency
        // that a change actually experienced, not an interpolation between two
        // that did. Interpolation also understates a sparse tail, which is the
        // half of the distribution this exists to expose.
        "quantile_disc(epoch(_applied_at) - epoch(_commit_ts), 0.50), "
        "quantile_disc(epoch(_applied_at) - epoch(_commit_ts), 0.95), "
        "quantile_disc(epoch(_applied_at) - epoch(_commit_ts), 0.99), "
        "min(epoch(_applied_at) - epoch(_commit_ts)), "
        "max(epoch(_applied_at) - epoch(_commit_ts)), "
        "count(*) FILTER (WHERE _op='I'), "
        "count(*) FILTER (WHERE _op='U'), "
        "count(*) FILTER (WHERE _op='D') "
        "FROM " + log + " WHERE " + where);
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
