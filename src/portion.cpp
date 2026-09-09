#include "portion.hpp"

#include <algorithm>

namespace erpl_rev {
namespace portion {

namespace {
JobState StateOf(const std::vector<JobStatus> &jobs, const std::string &job) {
    for (const auto &j : jobs)
        if (j.job == job) return j.state;
    return JobState::Unknown;
}
}  // namespace

Dispatch PlanDispatch(const std::vector<PortionRow> &rows, int max_workers, int max_attempts,
                      const std::vector<JobStatus> &jobs) {
    Dispatch out;

    int live = 0;
    std::vector<int> reclaimable;

    for (const auto &p : rows) {
        if (p.status != State::Running) continue;
        const auto js = StateOf(jobs, p.worker_job);
        if (js == JobState::Running || js == JobState::Unknown) {
            // Unknown is treated as alive on purpose: reclaiming a portion whose
            // job status we simply could not read would run it twice.
            ++live;
            continue;
        }
        // The job is gone and the portion never reached Done. Somebody has to
        // pick it up, or the run waits forever on a worker that is not coming
        // back.
        if (p.attempts >= max_attempts) out.fail.push_back(p.portion_no);
        else reclaimable.push_back(p.portion_no);
    }

    int slots = max_workers - live;
    for (int no : reclaimable) {
        if (slots <= 0) break;
        out.reset.push_back(no);
        out.dispatch.push_back(no);
        --slots;
    }
    for (const auto &p : rows) {
        if (slots <= 0) break;
        if (p.status != State::Pending) continue;
        out.dispatch.push_back(p.portion_no);
        --slots;
    }
    return out;
}

std::vector<int> PlanRestart(const std::vector<PortionRow> &rows) {
    std::vector<int> out;
    for (const auto &p : rows)
        if (p.status != State::Done) out.push_back(p.portion_no);
    return out;
}

bool IsComplete(const std::vector<PortionRow> &rows) {
    return !rows.empty() &&
           std::all_of(rows.begin(), rows.end(),
                       [](const PortionRow &p) { return p.status == State::Done; });
}

bool IsFinished(const std::vector<PortionRow> &rows) {
    return !rows.empty() &&
           std::all_of(rows.begin(), rows.end(), [](const PortionRow &p) {
               return p.status == State::Done || p.status == State::Failed;
           });
}

ProgressInfo Progress(const std::vector<PortionRow> &rows) {
    ProgressInfo p;
    p.total = static_cast<int>(rows.size());
    for (const auto &r : rows) {
        if (r.status == State::Done) { ++p.done; p.rows += r.rows; }
    }
    return p;
}

}  // namespace portion
}  // namespace erpl_rev
