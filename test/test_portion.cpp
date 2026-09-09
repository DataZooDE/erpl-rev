// The portion state machine for a mass run.
//
// A portion list is persisted before any worker starts, which is the whole point:
// the coordinator can then see which portions finished, so a failed run is
// restartable and a dead worker's portion can be re-dispatched to another.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "portion.hpp"

using namespace erpl_rev::portion;

namespace {
std::vector<PortionRow> Rows(int n) {
    std::vector<PortionRow> v;
    for (int i = 1; i <= n; ++i) {
        PortionRow p;
        p.portion_no = i;
        p.predicate = "belnr >= 'A' AND belnr <= 'B'";
        p.status = State::Pending;
        v.push_back(p);
    }
    return v;
}
}  // namespace

TEST_CASE("portion: a fresh run dispatches up to the worker budget", "[portion]") {
    auto rows = Rows(10);
    const auto d = PlanDispatch(rows, /*max_workers=*/3, /*max_attempts=*/3, {});
    CHECK(d.dispatch.size() == 3);
    CHECK(d.dispatch[0] == 1);
}

TEST_CASE("portion: a running portion is not dispatched twice", "[portion]") {
    auto rows = Rows(4);
    rows[0].status = State::Running;
    rows[0].worker_job = "JOB1";
    const auto d = PlanDispatch(rows, 4, 3, {{"JOB1", JobState::Running}});
    // Three free slots, three pending portions.
    CHECK(d.dispatch.size() == 3);
    CHECK(std::find(d.dispatch.begin(), d.dispatch.end(), 1) == d.dispatch.end());
}

TEST_CASE("portion: a dead worker's portion is re-dispatched", "[portion]") {
    // Keep-alive. Without this the run stalls forever on a portion whose job
    // aborted -- the portion stays RUNNING and nothing ever revisits it.
    auto rows = Rows(2);
    rows[0].status = State::Running;
    rows[0].worker_job = "DEAD";
    rows[0].attempts = 1;

    const auto d = PlanDispatch(rows, 2, 3, {{"DEAD", JobState::Aborted}});
    CHECK(std::find(d.dispatch.begin(), d.dispatch.end(), 1) != d.dispatch.end());
    CHECK(std::find(d.reset.begin(), d.reset.end(), 1) != d.reset.end());
}

TEST_CASE("portion: a portion that keeps dying is failed, not retried forever",
          "[portion]") {
    // A poison portion would otherwise consume a worker slot indefinitely and
    // the run would never finish or fail.
    auto rows = Rows(1);
    rows[0].status = State::Running;
    rows[0].worker_job = "DEAD";
    rows[0].attempts = 3;

    const auto d = PlanDispatch(rows, 2, /*max_attempts=*/3, {{"DEAD", JobState::Aborted}});
    CHECK(d.dispatch.empty());
    CHECK(std::find(d.fail.begin(), d.fail.end(), 1) != d.fail.end());
}

TEST_CASE("portion: a restart touches only what is not done", "[portion]") {
    auto rows = Rows(5);
    rows[0].status = State::Done;
    rows[1].status = State::Done;
    rows[2].status = State::Failed;
    rows[3].status = State::Pending;
    rows[4].status = State::Running;
    rows[4].worker_job = "GONE";

    const auto d = PlanRestart(rows);
    CHECK(d.size() == 3);
    CHECK(std::find(d.begin(), d.end(), 1) == d.end());   // done stays done
    CHECK(std::find(d.begin(), d.end(), 3) != d.end());   // failed is retried
    CHECK(std::find(d.begin(), d.end(), 5) != d.end());   // orphaned running too
}

TEST_CASE("portion: a run is complete only when every portion is done", "[portion]") {
    auto rows = Rows(3);
    rows[0].status = State::Done;
    rows[1].status = State::Done;
    CHECK_FALSE(IsComplete(rows));
    CHECK_FALSE(IsFinished(rows));

    rows[2].status = State::Done;
    CHECK(IsComplete(rows));
    CHECK(IsFinished(rows));
}

TEST_CASE("portion: a run with a failed portion is finished but not complete",
          "[portion]") {
    // The distinction matters: the coordinator must stop waiting, but must not
    // build the primary key or report success over missing data.
    auto rows = Rows(2);
    rows[0].status = State::Done;
    rows[1].status = State::Failed;
    CHECK(IsFinished(rows));
    CHECK_FALSE(IsComplete(rows));
}

TEST_CASE("portion: progress is reported over all portions", "[portion]") {
    auto rows = Rows(4);
    rows[0].status = State::Done;
    rows[0].rows = 100;
    rows[1].status = State::Done;
    rows[1].rows = 250;
    const auto p = Progress(rows);
    CHECK(p.done == 2);
    CHECK(p.total == 4);
    CHECK(p.rows == 350);
}
