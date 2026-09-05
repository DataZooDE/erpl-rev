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
