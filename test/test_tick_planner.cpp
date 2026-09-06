// The tick planner: which targets run this tick, on how many workers, and when
// the next tick is.
//
// It is the one place that knows cadence, backoff, parking and the worker
// budget, so that the batch tick and the daemon cannot disagree about what is
// due. Pure -- targets in, plan out, with the clock passed as an argument -- so
// backoff and parking are millisecond tests instead of waiting for real time.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "tick_planner.hpp"

using namespace erpl_rev::plan;

namespace {
constexpr double kNow = 1788609600.0;   // 2026-09-05 12:00:00 UTC

TargetRow T(const std::string &name, const std::string &cadence, double last_run) {
    TargetRow t;
    t.target = name;
    t.method = "WATERMARK";
    t.cadence = cadence;
    t.status = "IDLE";
    t.last_run_epoch = last_run;
    t.max_cycle_secs = 3600;
    return t;
}

DaemonRow Daemon(int workers = 2, double share = 0.5) {
    DaemonRow d;
    d.instance_id = "i1";
    d.heartbeat_epoch = kNow;
    d.tick_secs = 2;
    d.max_workers = workers;
    d.full_load_share = share;
    d.stop = false;
    return d;
}

bool Has(const TickPlan &p, const std::string &target) {
    return std::any_of(p.cycles.begin(), p.cycles.end(),
                       [&](const Cycle &c) { return c.target == target; });
}
}  // namespace

TEST_CASE("tick_planner: a micro cadence is due once its interval has passed", "[plan]") {
    std::vector<TargetRow> t{T("fast", "micro:5", kNow - 10), T("slow", "micro:60", kNow - 10)};
    const auto p = PlanTick(t, {}, Daemon(), kNow);
    CHECK(Has(p, "fast"));
    CHECK_FALSE(Has(p, "slow"));
}

TEST_CASE("tick_planner: hourly, nightly and manual cadences", "[plan]") {
    std::vector<TargetRow> t{
        T("hourly_due", "hourly", kNow - 3700),
        T("hourly_not", "hourly", kNow - 100),
        T("nightly_due", "nightly", kNow - 90000),
        T("manual", "manual", kNow - 999999),   // never due on its own
    };
    const auto p = PlanTick(t, {}, Daemon(9), kNow);
    CHECK(Has(p, "hourly_due"));
    CHECK_FALSE(Has(p, "hourly_not"));
    CHECK(Has(p, "nightly_due"));
    CHECK_FALSE(Has(p, "manual"));
}

TEST_CASE("tick_planner: a target that has never run is due immediately", "[plan]") {
    auto t = T("new", "hourly", 0);
    t.last_run_epoch = 0;
    const auto p = PlanTick({t}, {}, Daemon(), kNow);
    CHECK(Has(p, "new"));
}

TEST_CASE("tick_planner: a running target is not started again", "[plan]") {
    // The lease is advisory; the planner should not even propose a second cycle
    // for a target that is mid-run.
    auto t = T("busy", "micro:1", kNow - 60);
    t.status = "RUNNING";
    t.lease_epoch = kNow - 5;
    const auto p = PlanTick({t}, {}, Daemon(), kNow);
    CHECK_FALSE(Has(p, "busy"));
}

TEST_CASE("tick_planner: a stale lease is reclaimed", "[plan]") {
    // ...but a RUNNING row whose lease has aged out belongs to a cycle that died.
    auto t = T("stale", "micro:1", kNow - 60);
    t.status = "RUNNING";
    t.lease_epoch = kNow - 7200;     // older than max_cycle_secs
    const auto p = PlanTick({t}, {}, Daemon(), kNow);
    CHECK(Has(p, "stale"));
}

TEST_CASE("tick_planner: backoff doubles with each consecutive failure", "[plan]") {
    // 2^fail_count intervals, so one broken target does not hammer SAP.
    for (int fails = 0; fails <= 4; ++fails) {
        auto t = T("flaky", "micro:10", kNow - 10);
        t.fail_count = fails;
        const double needed = 10.0 * (1 << fails);

        auto just_before = t;
        just_before.last_run_epoch = kNow - needed + 1;
        CHECK_FALSE(Has(PlanTick({just_before}, {}, Daemon(), kNow), "flaky"));

        auto just_after = t;
        just_after.last_run_epoch = kNow - needed - 1;
        CHECK(Has(PlanTick({just_after}, {}, Daemon(), kNow), "flaky"));
    }
}

TEST_CASE("tick_planner: backoff is capped so a target still retries", "[plan]") {
    // Without a cap, 2^30 intervals means "never again".
    auto t = T("dead", "micro:10", kNow - 100000);
    t.fail_count = 30;
    CHECK(Has(PlanTick({t}, {}, Daemon(), kNow), "dead"));
}

TEST_CASE("tick_planner: a parked target waits, then comes back", "[plan]") {
    auto t = T("parked", "micro:1", kNow - 60);
    t.parked_until_epoch = kNow + 60;
    CHECK_FALSE(Has(PlanTick({t}, {}, Daemon(), kNow), "parked"));

    t.parked_until_epoch = kNow - 1;
    CHECK(Has(PlanTick({t}, {}, Daemon(), kNow), "parked"));
}

TEST_CASE("tick_planner: one broken target does not stop the others", "[plan]") {
    auto broken = T("broken", "micro:1", kNow - 60);
    broken.fail_count = 6;
    broken.parked_until_epoch = kNow + 3600;
    std::vector<TargetRow> t{broken, T("healthy", "micro:1", kNow - 60)};
    const auto p = PlanTick(t, {}, Daemon(), kNow);
    CHECK_FALSE(Has(p, "broken"));
    CHECK(Has(p, "healthy"));
}

TEST_CASE("tick_planner: the worker budget is never exceeded", "[plan]") {
    std::vector<TargetRow> t;
    for (int i = 0; i < 10; ++i) t.push_back(T("t" + std::to_string(i), "micro:1", kNow - 60));
    const auto p = PlanTick(t, {}, Daemon(3), kNow);
    CHECK(p.cycles.size() <= 3);
}

TEST_CASE("tick_planner: a full load cannot starve the micro deltas", "[plan]") {
    // With a 50% share and 4 workers, at most 2 slots go to full loads even when
    // full loads are what is most overdue.
    std::vector<TargetRow> t;
    for (int i = 0; i < 4; ++i) {
        auto f = T("full" + std::to_string(i), "nightly", kNow - 999999);
        f.load_type_default = "L";
        t.push_back(f);
    }
    for (int i = 0; i < 4; ++i) t.push_back(T("micro" + std::to_string(i), "micro:1", kNow - 60));

    const auto p = PlanTick(t, {}, Daemon(4, 0.5), kNow);
    const auto fulls = std::count_if(p.cycles.begin(), p.cycles.end(),
                                     [](const Cycle &c) { return c.load_type == "L"; });
    CHECK(fulls <= 2);
    CHECK(p.cycles.size() > static_cast<size_t>(fulls));   // deltas got in
}

TEST_CASE("tick_planner: the stop flag empties the plan", "[plan]") {
    std::vector<TargetRow> t{T("a", "micro:1", kNow - 60), T("b", "micro:1", kNow - 60)};
    auto d = Daemon();
    d.stop = true;
    const auto p = PlanTick(t, {}, d, kNow);
    CHECK(p.stop);
    CHECK(p.cycles.empty());
}

TEST_CASE("tick_planner: a trigger target with shadow rows is due", "[plan]") {
    // The trigger tier is driven by whether its shadow table has anything in it,
    // not by a clock.
    auto t = T("cdc", "micro:60", kNow - 1);   // nowhere near due by cadence
    t.method = "CDC";
    CdcRow c;
    c.target = "cdc";
    c.status = "ACTIVE";
    c.shadow_rows = 17;
    CHECK(Has(PlanTick({t}, {c}, Daemon(), kNow), "cdc"));

    c.shadow_rows = 0;
    CHECK_FALSE(Has(PlanTick({t}, {c}, Daemon(), kNow), "cdc"));
}

TEST_CASE("tick_planner: an inconsistent trigger set is not run", "[plan]") {
    // Running a cycle against a trigger set with a missing trigger would advance
    // the position past changes that were never captured.
    auto t = T("cdc", "micro:1", kNow - 60);
    t.method = "CDC";
    CdcRow c;
    c.target = "cdc";
    c.status = "INCONSISTENT";
    c.shadow_rows = 5;
    CHECK_FALSE(Has(PlanTick({t}, {c}, Daemon(), kNow), "cdc"));
}

TEST_CASE("tick_planner: a heavy target goes to a worker, a small one runs inline",
          "[plan]") {
    auto big = T("big", "micro:1", kNow - 60);
    big.last_rows = 5'000'000;
    auto small = T("small", "micro:1", kNow - 60);
    small.last_rows = 12;

    const auto p = PlanTick({big, small}, {}, Daemon(4), kNow);
    for (const auto &c : p.cycles) {
        if (c.target == "big") CHECK(c.worker);
        if (c.target == "small") CHECK_FALSE(c.worker);
    }
}

TEST_CASE("tick_planner: the sleep interval comes from the daemon row", "[plan]") {
    auto d = Daemon();
    d.tick_secs = 5;
    CHECK(PlanTick({}, {}, d, kNow).sleep_secs == 5);
}

TEST_CASE("tick_planner: a blocked target is never planned", "[plan]") {
    // Schema drift blocks a target until an operator accepts or waives it.
    // Running it anyway would apply rows against a schema known to disagree.
    auto t = T("drifted", "micro:1", kNow - 60);
    t.status = "BLOCKED";
    CHECK_FALSE(Has(PlanTick({t}, {}, Daemon(), kNow), "drifted"));
}

TEST_CASE("tick_planner: equally overdue targets are planned in a stable order", "[plan]") {
    // Ten targets that all became due at exactly the same moment -- the normal
    // case for a set registered together on the same cadence. Ordering them by
    // overdue-ness alone leaves ties unordered, so which four run this tick
    // depended on the order the rows came out of the database. That makes a
    // starved target impossible to diagnose and this planner's own tests
    // non-reproducible.
    std::vector<TargetRow> t;
    for (int i = 0; i < 10; ++i) t.push_back(T("t" + std::to_string(i), "micro:1", kNow - 60));

    const auto p = PlanTick(t, {}, Daemon(4), kNow);
    REQUIRE(p.cycles.size() == 4);

    std::vector<std::string> got;
    for (const auto &c : p.cycles) got.push_back(c.target);
    CHECK(got == std::vector<std::string>{"t0", "t1", "t2", "t3"});

    // And the same answer whatever order the rows arrive in.
    std::vector<TargetRow> reversed(t.rbegin(), t.rend());
    const auto q = PlanTick(reversed, {}, Daemon(4), kNow);
    std::vector<std::string> got2;
    for (const auto &c : q.cycles) got2.push_back(c.target);
    CHECK(got2 == got);
}

TEST_CASE("tick_planner: a single worker still runs full loads", "[plan]") {
    // The share is a cap expressed as a fraction of the budget, so at one worker
    // it floors to zero and the full load was never planned -- on any tick,
    // forever. A share protects the deltas from a mass load taking EVERY slot;
    // with one slot there is nothing to protect, and refusing to run the work at
    // all is the worse failure.
    auto f = T("full", "nightly", kNow - 999999);
    f.load_type_default = "L";
    const auto p = PlanTick({f}, {}, Daemon(1, 0.5), kNow);
    CHECK(Has(p, "full"));
}

TEST_CASE("tick_planner: a zero share means the daemon runs no full loads", "[plan]") {
    // The deliberate setting must survive the floor above: an operator who set
    // the share to zero asked for full loads to be scheduled by hand.
    auto f = T("full", "nightly", kNow - 999999);
    f.load_type_default = "L";
    const auto p = PlanTick({f}, {}, Daemon(4, 0.0), kNow);
    CHECK_FALSE(Has(p, "full"));
}

TEST_CASE("tick_planner: a trigger target is not starved by busier delta targets",
          "[plan]") {
    // `overdue` was a row count for a CDC target and seconds-past-due for
    // everything else, and the two were sorted against each other. A trigger
    // target with three pending shadow rows therefore sorted below any delta
    // target four seconds late -- so on a budget smaller than the candidate
    // set, with several busy micro-cadence targets, the trigger tier never got
    // a slot and its shadow table grew without bound. Comparing a count with a
    // duration is not a tiebreak problem; it is a units problem.
    std::vector<TargetRow> t;
    for (int i = 0; i < 6; ++i) t.push_back(T("d" + std::to_string(i), "micro:1", kNow - 600));

    auto c = T("trig", "micro:1", kNow - 1);
    c.method = "CDC";
    t.push_back(c);

    CdcRow cdc;
    cdc.target = "trig";
    cdc.status = "ACTIVE";
    cdc.shadow_rows = 3;

    const auto p = PlanTick(t, {cdc}, Daemon(2), kNow);
    CHECK(Has(p, "trig"));
}

TEST_CASE("tick_planner: a long-waiting trigger target outranks a barely-late delta",
          "[plan]") {
    // The units half, on its own. The reserved-slot test above passes under BOTH
    // the old row-count formula and the new seconds one, so it pins the
    // reservation and says nothing about the conversion. Here the budget is 1 --
    // no slot can be reserved -- and the CDC target has been waiting 300s with
    // 3 shadow rows against a delta 10s past due. As a row count it scores 3 and
    // loses; as seconds it scores 300 and wins.
    std::vector<TargetRow> t{T("d0", "micro:1", kNow - 10)};
    auto c = T("trig", "micro:1", kNow - 300);
    c.method = "CDC";
    t.push_back(c);

    CdcRow cdc;
    cdc.target = "trig";
    cdc.status = "ACTIVE";
    cdc.shadow_rows = 3;

    const auto p = PlanTick(t, {cdc}, Daemon(1), kNow);
    REQUIRE(p.cycles.size() == 1);
    CHECK(p.cycles[0].target == "trig");
}

TEST_CASE("tick_planner: a reservation nobody can spend does not waste the slot",
          "[plan]") {
    // The slot is held for the trigger tier, but the CDC candidate may still be
    // skipped for an unrelated reason -- here it carries a truncating load type
    // and the full-load cap is already spent. The reservation was computed once,
    // up front, so it stayed held for a candidate that could never take it and
    // a worker went unused on every tick.
    std::vector<TargetRow> t;
    for (int i = 0; i < 4; ++i) t.push_back(T("d" + std::to_string(i), "micro:1", kNow - 600));

    auto full = T("bigload", "nightly", kNow - 999999);
    full.load_type_default = "L";
    t.push_back(full);

    auto c = T("trig", "micro:1", kNow - 300);
    c.method = "CDC";
    c.load_type_default = "L";     // also a full load; the cap is already spent
    t.push_back(c);

    CdcRow cdc;
    cdc.target = "trig";
    cdc.status = "ACTIVE";
    cdc.shadow_rows = 5;

    const auto p = PlanTick(t, {cdc}, Daemon(3, 0.34), kNow);
    // Three workers, three cycles planned. The held slot goes back to the pool
    // rather than being carried for a candidate that was never eligible.
    CHECK(p.cycles.size() == 3);
}

TEST_CASE("tick_planner: a target blocked by an impossible load type stops being planned",
          "[plan]") {
    // The other half of blocking it: the planner must actually stop handing it
    // out, or blocking is just a message. This is what turns "refused every two
    // seconds forever" into "refused once".
    auto t = T("bad", "micro:1", kNow - 600);
    t.method = "SNAPSHOT";
    t.load_type_default = "F";
    t.status = "BLOCKED";

    const auto p = PlanTick({t}, {}, Daemon(2), kNow);
    CHECK_FALSE(Has(p, "bad"));
}

TEST_CASE("tick_planner: a spent one-shot load type falls back to delta", "[plan]") {
    // The two halves, combined where they belong. load_type_default is what the
    // operator asked for and stays that way; one_shot_spent is what the engine
    // recorded. Sharing one column meant the engine crossed out the operator's
    // value, and a re-registration or a manual run could then cancel or consume
    // a seed nobody meant to touch.
    auto t = T("seeded", "micro:1", kNow - 600);
    t.load_type_default = "L";

    CHECK(PlanTick({t}, {}, Daemon(2), kNow).cycles[0].load_type == "L");

    t.one_shot_spent = true;
    CHECK(PlanTick({t}, {}, Daemon(2), kNow).cycles[0].load_type == "D");
}

TEST_CASE("tick_planner: a spent seed no longer counts against the full-load share",
          "[plan]") {
    // It is a delta now, so it must not be capped as a full load -- otherwise a
    // seeded target keeps competing for the reserved full-load slots forever.
    std::vector<TargetRow> t;
    for (int i = 0; i < 3; ++i) {
        auto s = T("s" + std::to_string(i), "micro:1", kNow - 600);
        s.load_type_default = "L";
        s.one_shot_spent = true;
        t.push_back(s);
    }
    const auto p = PlanTick(t, {}, Daemon(3, 0.0), kNow);   // no full loads allowed at all
    CHECK(p.cycles.size() == 3);
}

TEST_CASE("tick_planner: an upgraded target that was already seeded stays a delta",
          "[plan]") {
    // The planner half of the same upgrade question. A database migrated to v8
    // has one_shot_spent=false on every row, including targets seeded long ago
    // -- so the planner must not read "not spent" as "seed it again". It does
    // not, because a spent target's INTENT was already rewritten to 'D' by the
    // old code, and intent is what the planner starts from.
    auto seeded = T("seeded_before_upgrade", "micro:1", kNow - 600);
    seeded.load_type_default = "D";      // what the old spend left behind
    seeded.one_shot_spent = false;       // what v8 defaults it to

    const auto p = PlanTick({seeded}, {}, Daemon(2), kNow);
    REQUIRE(p.cycles.size() == 1);
    CHECK(p.cycles[0].load_type == "D");
}

TEST_CASE("tick_planner: a refused target does not starve the healthy ones", "[plan]") {
    // The property, not the mechanism. A target that is refused every tick
    // never moves its last_run_ts, so its overdue grows without bound -- and
    // the planner sorts by overdue against a budget of max_workers. Two
    // mis-registered targets therefore occupied the entire budget forever and
    // no healthy target was planned again: a total replication outage from one
    // operator typo, with status IDLE, fail_count 0 and no run-statistics row.
    //
    // Whatever refuses a target must take it out of the due set. This asserts
    // the consequence rather than the status value, so it keeps meaning
    // something if the mechanism changes.
    std::vector<TargetRow> t;
    for (int i = 0; i < 2; ++i) {
        auto bad = T("bad" + std::to_string(i), "micro:1", kNow - 999999);   // wildly overdue
        bad.method = "SNAPSHOT";
        bad.load_type_default = "F";
        bad.status = "BLOCKED";     // what a refusal leaves behind
        t.push_back(bad);
    }
    auto good = T("healthy", "micro:1", kNow - 10);
    t.push_back(good);

    const auto p = PlanTick(t, {}, Daemon(2), kNow);
    CHECK(Has(p, "healthy"));
    CHECK_FALSE(Has(p, "bad0"));
    CHECK_FALSE(Has(p, "bad1"));
}

TEST_CASE("tick_planner: load type I is not planned forever", "[plan]") {
    // I is "adopt a position, transfer nothing" -- once per target by
    // definition. Undemoted it planned on every tick: the read is skipped, the
    // watermark is re-seeded to the new ceiling, and the run is recorded
    // SUCCESS with zero rows. A permanently empty target reporting green.
    auto t = T("adopted", "micro:1", kNow - 600);
    t.load_type_default = "I";

    CHECK(PlanTick({t}, {}, Daemon(2), kNow).cycles[0].load_type == "I");
    t.one_shot_spent = true;
    CHECK(PlanTick({t}, {}, Daemon(2), kNow).cycles[0].load_type == "D");
}
