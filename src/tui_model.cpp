#include "tui_model.hpp"

#include <set>

#include <algorithm>
#include <cstdio>

#include "json_util.hpp"

namespace erpl_rev {
namespace tui {
namespace {

std::string Field(const std::string &row, const std::string &key) {
    const auto rows = json::ParseRows("[" + row + "]");
    if (rows.empty()) return {};
    for (const auto &c : rows[0])
        if (c.key == key) return c.is_null ? std::string() : c.value;
    return {};
}

long long Num(const std::string &row, const std::string &key, long long dflt = 0) {
    const auto v = Field(row, key);
    return v.empty() ? dflt : std::atoll(v.c_str());
}

bool Flag(const std::string &row, const std::string &key) {
    return Field(row, key) == "true";
}

// How bad a row is, worst first. Blocked cannot run at all; parked has been
// backed off; never-run has no data whatsoever, which is a worse state than old
// data and is the commonest support call.
int Severity(const Row &r) {
    if (r.blocked) return 0;
    if (r.parked) return 1;
    if (r.lag_seconds < 0) return 2;
    if (r.fail_count > 0) return 3;
    if (!r.healthy) return 4;
    return 5;
}

}  // namespace

std::string FormatLag(long long s) {
    if (s < 0) return "never";
    if (s < 60) return std::to_string(s) + "s";
    char buf[32];
    if (s < 3600) {
        std::snprintf(buf, sizeof(buf), "%lldm%02llds", s / 60, s % 60);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%lldh%02lldm", s / 3600, (s % 3600) / 60);
    return buf;
}

void SortForOperator(std::vector<Row> &rows) {
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        const int sa = Severity(a), sb = Severity(b);
        if (sa != sb) return sa < sb;
        // Within a severity, the most overdue first; a name tiebreak so the
        // order does not shuffle between refreshes, which makes a moving
        // display impossible to read.
        if (a.lag_seconds != b.lag_seconds) return a.lag_seconds > b.lag_seconds;
        return a.target < b.target;
    });
}

std::vector<std::pair<std::string, long long>> SampleCounts(
    const QueryFn &q, const std::vector<std::string> &targets) {
    std::vector<std::pair<std::string, long long>> out;
    // One small count per target, deliberately, rather than a single
    // UNION ALL over all of them.
    //
    // The combined form -- SELECT 'a' AS t, count(*) FROM a UNION ALL ... --
    // wedged the monitor's refresh loop after a handful of iterations when it
    // travelled through the quack client, and a stalled monitor looks exactly
    // like a broken graph. A plain count per target does not, and it costs one
    // round trip per registered target every couple of seconds, only while the
    // graph is open.
    //
    // It also removes the need to ask the catalogue what exists first: a
    // target registered but never loaded has no table, that count throws, and
    // it is skipped. One new target must not blank the whole graph.
    for (const auto &t : targets) {
        if (t.empty()) continue;
        // The engine created these names, so they are already safe; checking
        // again costs nothing and means a hand-edited registry cannot reach
        // the SQL.
        if (t.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            continue;
        try {
            const auto r = q("SELECT count(*) AS n FROM " + t);
            if (!r.rows.empty()) out.emplace_back(t, Num(r.rows[0], "n"));
        } catch (const std::exception &) {
            continue;   // no table yet, or gone: a gap, not a failure
        }
    }
    return out;
}

Snapshot Load(const QueryFn &q) {
    Snapshot out;
    try {
        const auto t = q("SELECT target, method, cadence, status, lag_seconds, last_rows, "
                         "fail_count, last_error, is_healthy, is_blocked, is_parked, park_reason "
                         "FROM erpl_rev_targets");
        for (const auto &r : t.rows) {
            Row row;
            row.target = Field(r, "target");
            row.method = Field(r, "method");
            row.cadence = Field(r, "cadence");
            row.status = Field(r, "status");
            row.last_error = Field(r, "last_error");
            row.park_reason = Field(r, "park_reason");
            row.lag_seconds = Field(r, "lag_seconds").empty() ? -1 : Num(r, "lag_seconds");
            row.rows = Num(r, "last_rows");
            row.fail_count = Num(r, "fail_count");
            row.healthy = Flag(r, "is_healthy");
            row.blocked = Flag(r, "is_blocked");
            row.parked = Flag(r, "is_parked");
            out.rows.push_back(row);
        }
        SortForOperator(out.rows);

        const auto h = q("SELECT targets, healthy, blocked, parked, failing, never_run, "
                         "worst_lag_seconds, daemon_status, daemon_heartbeat_age_s, daemon_ticks "
                         "FROM erpl_rev_health");
        if (!h.rows.empty()) {
            const auto &s = h.rows[0];
            out.summary.targets = Num(s, "targets");
            out.summary.healthy = Num(s, "healthy");
            out.summary.blocked = Num(s, "blocked");
            out.summary.parked = Num(s, "parked");
            out.summary.failing = Num(s, "failing");
            out.summary.never_run = Num(s, "never_run");
            out.summary.worst_lag =
                Field(s, "worst_lag_seconds").empty() ? -1 : Num(s, "worst_lag_seconds");
            out.summary.daemon_status = Field(s, "daemon_status");
            out.summary.daemon_age =
                Field(s, "daemon_heartbeat_age_s").empty() ? -1 : Num(s, "daemon_heartbeat_age_s");
            out.summary.daemon_ticks = Num(s, "daemon_ticks");
        }
    } catch (const std::exception &e) {
        // Shown, never thrown. The monitor runs against a server that can
        // restart under it, and throwing would drop the operator back to a
        // shell prompt at the moment they were watching something.
        out.error = e.what();
        out.rows.clear();
    }
    return out;
}

}  // namespace tui
}  // namespace erpl_rev
