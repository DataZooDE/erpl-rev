// The tick planner.
//
// One place decides what runs: which targets are due, which go to a worker,
// how long to sleep. The batch tick and the daemon both go through it, so they
// cannot disagree about what "due" means -- which is the reason it moved out of
// ABAP rather than being duplicated there.
//
// Pure. Rows in, plan out, and the clock is an argument. Backoff, parking and
// lease expiry are therefore ordinary unit tests rather than something that
// needs real time to pass.
#pragma once

#include <string>
#include <vector>

namespace erpl_rev {
namespace plan {

struct TargetRow {
    std::string target;
    std::string method;             // WATERMARK | CHANGEDOC | INSERT_ONLY | SNAPSHOT | CDC
    std::string cadence;            // micro:<s> | hourly | nightly | manual
    std::string status;             // IDLE | RUNNING | BLOCKED | ERROR
    std::string load_type_default;  // F | I | L | D -- registration intent
    // The engine's half: set by Commit once a one-shot type has actually run.
    // Kept separate because the two have different owners and different
    // lifetimes, and sharing one column produced two defects that each needed
    // their own fix.
    bool one_shot_spent = false;
    double last_run_epoch = 0;
    double lease_epoch = 0;
    double parked_until_epoch = 0;
    int fail_count = 0;
    int max_cycle_secs = 3600;
    long long last_rows = 0;        // rows the previous cycle moved
};

struct CdcRow {
    std::string target;
    std::string status;             // ACTIVE | SEEDED | DISABLED | INCONSISTENT
    long long shadow_rows = 0;
};

struct DaemonRow {
    std::string instance_id;
    double heartbeat_epoch = 0;
    int tick_secs = 2;
    int max_workers = 2;
    double full_load_share = 0.5;   // fraction of the budget full loads may take
    bool stop = false;
};

struct Cycle {
    std::string target;
    std::string method;
    std::string load_type;
    bool worker = false;            // dispatch to a background job rather than inline
};

struct TickPlan {
    bool stop = false;
    int sleep_secs = 2;
    std::vector<Cycle> cycles;
};

TickPlan PlanTick(const std::vector<TargetRow> &targets, const std::vector<CdcRow> &cdc,
                  const DaemonRow &daemon, double now_epoch);

// Seconds between runs for a cadence string; 0 means "never on its own".
double CadenceSeconds(const std::string &cadence);

}  // namespace plan
}  // namespace erpl_rev
