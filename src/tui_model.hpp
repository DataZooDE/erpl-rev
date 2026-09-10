// What the monitor shows, as data.
//
// Kept apart from the drawing so the part that can be wrong -- which target is
// worst, how a lag reads, what counts as a problem -- is testable without a
// terminal. The FTXUI layer turns this into cells and does no thinking.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "duckdb_bridge.hpp"
#include "tui_graph.hpp"

namespace erpl_rev {
namespace tui {

// Any source of rows: the in-process bridge, or a remote quack client. The
// monitor runs from the shell against a running server, so it must not assume
// it owns the database.
using QueryFn = std::function<QueryResult(const std::string &)>;

struct Row {
    std::string target, method, cadence, status, last_error, park_reason;
    long long lag_seconds = -1;      // -1 means never run
    long long rows = 0, fail_count = 0;
    // What the LAST completed cycle did, split three ways. `rows` is their sum,
    // and it was all the monitor used to show -- which cannot distinguish a
    // target that inserted a million rows from one that deleted them. The
    // graph's split is net within a bucket; these are the exact figures the
    // engine reported, which is why both surfaces exist.
    long long last_ins = 0, last_upd = 0, last_del = 0;
    bool healthy = false, blocked = false, parked = false;
};

struct Summary {
    long long targets = 0, healthy = 0, blocked = 0, parked = 0, failing = 0, never_run = 0;
    long long worst_lag = -1;
    std::string daemon_status = "STOPPED";
    long long daemon_age = -1, daemon_ticks = 0;
};

struct Snapshot {
    Summary summary;
    std::vector<Row> rows;
    std::string error;   // non-empty when the read failed; shown, never thrown
};

Snapshot Load(const QueryFn &q);

// Row counts and cumulative operation counters for the named targets, at one
// moment -- everything tui::Rates needs to differentiate.
//
// Called ONLY while the throughput graph is open. It is a count per target per
// refresh -- which DuckDB answers from metadata and is cheap -- plus ONE
// aggregate over the run statistics for all targets at once, but it is still
// work nobody asked for when the graph is closed, which is the argument for the
// graph being a key rather than always on.
//
// A registered target whose table does not exist yet (registered, never loaded)
// is skipped rather than erroring: a monitor that goes blank because one target
// is new is worse than one that shows the rest. A target with no completed run
// keeps zero counters, which differentiate to nothing.
std::vector<TargetCount> SampleCounts(const QueryFn &q,
                                      const std::vector<std::string> &targets);

// Worst first. An operator opening a monitor is looking for the problem, not
// for an alphabetical list -- so blocked, then parked, then failing, then the
// most overdue, and only then by name. Sorting by name would put the one broken
// target on page three.
void SortForOperator(std::vector<Row> &rows);

// "never", "12s", "4m10s", "3h02m". A raw second count makes the reader do
// arithmetic to notice that a five-second target is an hour behind.
std::string FormatLag(long long seconds);

}  // namespace tui
}  // namespace erpl_rev
