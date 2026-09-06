#include "tick_planner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace erpl_rev {
namespace plan {

namespace {

// Beyond this many consecutive failures the delay stops doubling. Without a cap
// 2^30 intervals is indistinguishable from "never retry", and a target that
// recovers on its own would never be noticed.
constexpr int kMaxBackoffShift = 6;

// A cycle that moved more than this many rows last time is handed to a worker
// job rather than run inline, so one heavy target cannot hold the daemon's tick.
constexpr long long kWorkerRowThreshold = 100'000;

bool IsFullLoad(const std::string &load_type) {
    return load_type == "L" || load_type == "F";
}

}  // namespace

double CadenceSeconds(const std::string &cadence) {
    if (cadence.rfind("micro:", 0) == 0) {
        const auto secs = std::atof(cadence.substr(6).c_str());
        return secs > 0 ? secs : 1.0;
    }
    if (cadence == "hourly") return 3600.0;
    if (cadence == "nightly") return 86400.0;
    return 0.0;   // manual, or unknown: never due on its own
}

TickPlan PlanTick(const std::vector<TargetRow> &targets, const std::vector<CdcRow> &cdc,
                  const DaemonRow &daemon, double now_epoch) {
    TickPlan plan;
    plan.sleep_secs = daemon.tick_secs;
    plan.stop = daemon.stop;
    if (daemon.stop) return plan;   // finish the tick, start nothing new

    auto cdc_for = [&](const std::string &target) -> const CdcRow * {
        for (const auto &c : cdc)
            if (c.target == target) return &c;
        return nullptr;
    };

    // Candidates, most overdue first, so a starved target wins a scarce slot.
    struct Candidate { const TargetRow *t; double overdue; bool full; };
    std::vector<Candidate> due;

    for (const auto &t : targets) {
        // Drift blocked this target: applying rows against a schema known to
        // disagree is worse than falling behind.
        if (t.status == "BLOCKED") continue;

        // A live cycle owns the target. A RUNNING row whose lease has aged past
        // the cycle budget belongs to a cycle that died, and is reclaimable.
        if (t.status == "RUNNING" && now_epoch - t.lease_epoch < t.max_cycle_secs) continue;

        if (t.parked_until_epoch > now_epoch) continue;

        const CdcRow *c = cdc_for(t.target);
        double overdue = 0;

        if (t.method == "CDC") {
            // The trigger tier is driven by whether anything is waiting in the
            // shadow table, not by a clock.
            if (!c) continue;
            // A trigger set with a missing or disabled object must not run: the
            // cycle would advance the position past changes never captured.
            if (c->status != "ACTIVE" && c->status != "SEEDED") continue;
            if (c->shadow_rows <= 0) continue;
            overdue = static_cast<double>(c->shadow_rows);
        } else {
            const double interval = CadenceSeconds(t.cadence);
            if (interval <= 0) continue;   // manual
            // Exponential backoff on consecutive failures, capped.
            const int shift = std::min(t.fail_count, kMaxBackoffShift);
            const double needed = interval * static_cast<double>(1 << shift);
            const double since = t.last_run_epoch <= 0 ? needed + 1
                                                       : now_epoch - t.last_run_epoch;
            if (since < needed) continue;
            overdue = since - needed;
        }

        due.push_back({&t, overdue, IsFullLoad(t.load_type_default)});
    }

    // Most overdue first, then by NAME. The tiebreak is not cosmetic: a set of
    // targets registered together is due at exactly the same instant, so ties
    // are the normal case, and without a total order which of them ran this
    // tick was decided by the order the rows left the database. A starved
    // target is then unreproducible.
    std::sort(due.begin(), due.end(), [](const Candidate &a, const Candidate &b) {
        if (a.overdue != b.overdue) return a.overdue > b.overdue;
        return a.t->target < b.t->target;
    });

    // Budget. Full loads get at most their share, so a mass load that is days
    // overdue cannot take every slot and starve the micro-cadence deltas -- the
    // deltas are the ones with a latency promise attached.
    const int budget = std::max(1, daemon.max_workers);
    // The share caps full loads so a mass load cannot take every slot. At one
    // worker the fraction floors to zero, which does not protect the deltas --
    // there is only one slot -- it just means the full load is never planned,
    // on any tick, forever. So the cap is at least one whenever full loads are
    // wanted at all; a share of zero still means zero, because that is an
    // operator saying "I schedule those myself".
    const int full_cap =
        daemon.full_load_share <= 0.0
            ? 0
            : std::max(1, static_cast<int>(std::floor(budget * daemon.full_load_share)));
    int used = 0, fulls = 0;

    for (const auto &c : due) {
        if (used >= budget) break;
        if (c.full && fulls >= full_cap) continue;

        Cycle cy;
        cy.target = c.t->target;
        cy.method = c.t->method;
        cy.load_type = c.t->load_type_default.empty() ? "D" : c.t->load_type_default;
        // Size decides inline vs worker: the plan is where that is known, because
        // it is the only place holding the previous cycle's row count.
        cy.worker = c.t->last_rows > kWorkerRowThreshold;
        plan.cycles.push_back(cy);

        ++used;
        if (c.full) ++fulls;
    }

    return plan;
}

}  // namespace plan
}  // namespace erpl_rev
