// The portion state machine for a mass run.
//
// The portion list is persisted before any worker starts. That is what makes the
// run restartable: the coordinator can see which portions finished, so a retry
// touches only the rest, and a portion whose worker died can be handed to
// another one rather than stalling the run forever.
#pragma once

#include <string>
#include <vector>

namespace erpl_rev {
namespace portion {

enum class State { Pending, Running, Done, Failed };
enum class JobState { Running, Finished, Aborted, Unknown };

struct PortionRow {
    int portion_no = 0;
    std::string predicate;
    State status = State::Pending;
    std::string worker_job;
    int attempts = 0;
    long long rows = 0;
};

struct JobStatus {
    std::string job;
    JobState state = JobState::Unknown;
};

struct Dispatch {
    std::vector<int> dispatch;  // start these
    std::vector<int> reset;     // ...after resetting them to Pending
    std::vector<int> fail;      // out of attempts
};

// One coordinator tick: what to start, what to reclaim, what to give up on.
Dispatch PlanDispatch(const std::vector<PortionRow> &rows, int max_workers, int max_attempts,
                      const std::vector<JobStatus> &jobs);

// A restart re-runs everything that is not Done -- failed portions, and portions
// left Running by a coordinator that died.
std::vector<int> PlanRestart(const std::vector<PortionRow> &rows);

// Every portion done. Only then may the coordinator build the primary key.
bool IsComplete(const std::vector<PortionRow> &rows);

// Nothing left to wait for -- which is not the same as success.
bool IsFinished(const std::vector<PortionRow> &rows);

struct ProgressInfo {
    int done = 0;
    int total = 0;
    long long rows = 0;
};
ProgressInfo Progress(const std::vector<PortionRow> &rows);

}  // namespace portion
}  // namespace erpl_rev
