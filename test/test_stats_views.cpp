// The operational views.
//
// What an operator asks is never "show me the run table". It is "is replication
// keeping up, and which target is the problem". These two views answer exactly
// that, and everything that displays them -- the CLI, the TUI, the Prometheus
// endpoint, the ABAP screen -- reads the SAME view, so four surfaces cannot
// disagree about whether a target is healthy.
//
// Views over the stored tables rather than a rollup: nothing new to write on the
// cycle path, nothing to keep consistent, and they cannot drift from the truth
// because they ARE the truth.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "duckdb_bridge.hpp"

using namespace erpl_rev;

namespace {
std::string One(DuckDbBridge &db, const std::string &sql) {
    const auto r = db.Query(sql);
    REQUIRE(r.rows.size() == 1);
    return r.rows[0];
}

// A target that ran two minutes ago and is healthy.
void SeedHealthy(DuckDbBridge &db, const std::string &name) {
    db.Execute("INSERT INTO _erpl_rev_delta_state "
               "(target, method, source_from, keys, cadence, status, last_run_ts, "
               " rows_applied, fail_count) VALUES ('" + name +
               "','WATERMARK','T','id','micro:5','IDLE', now() - INTERVAL '2 minutes', 100, 0)");
}
}  // namespace

TEST_CASE("stats: a healthy target reports its lag in seconds", "[stats]") {
    DuckDbBridge db;
    SeedHealthy(db, "ok1");

    // Seconds since the last cycle, which is the number an operator watches.
    // A target whose lag is climbing is behind whatever its cadence promised.
    const auto r = One(db, "SELECT target, "
                           "lag_seconds BETWEEN 100 AND 200 AS lag_sane, "
                           "is_healthy FROM erpl_rev_targets WHERE target='ok1'");
    CHECK(r.find("\"lag_sane\":true") != std::string::npos);
    CHECK(r.find("\"is_healthy\":true") != std::string::npos);
}

TEST_CASE("stats: a target that has never run is not reported as up to date",
          "[stats]") {
    // NULL last_run_ts must not read as lag 0. A target registered and never
    // run is the single most common "why is there no data" call, and reporting
    // it as current sends the operator looking anywhere but at it.
    DuckDbBridge db;
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, cadence) "
               "VALUES ('never','WATERMARK','T','id','micro:5')");

    const auto r = One(db, "SELECT lag_seconds IS NULL AS never_ran, is_healthy "
                           "FROM erpl_rev_targets WHERE target='never'");
    CHECK(r.find("\"never_ran\":true") != std::string::npos);
    CHECK(r.find("\"is_healthy\":false") != std::string::npos);
}

TEST_CASE("stats: parked, blocked and failing targets are each distinguishable",
          "[stats]") {
    // Three different operator actions. Collapsing them into "unhealthy" makes
    // the view useless for deciding what to DO: a parked target needs unpark, a
    // blocked one needs re-registering, a failing one needs its error read.
    DuckDbBridge db;
    SeedHealthy(db, "parked");
    db.Execute("UPDATE _erpl_rev_delta_state SET parked_until = now() + INTERVAL '1 hour', "
               "park_reason='too many failures' WHERE target='parked'");
    SeedHealthy(db, "blocked");
    db.Execute("UPDATE _erpl_rev_delta_state SET status='BLOCKED' WHERE target='blocked'");
    SeedHealthy(db, "failing");
    db.Execute("UPDATE _erpl_rev_delta_state SET fail_count=3, last_error='boom' "
               "WHERE target='failing'");

    CHECK(One(db, "SELECT is_parked FROM erpl_rev_targets WHERE target='parked'")
              .find("\"is_parked\":true") != std::string::npos);
    CHECK(One(db, "SELECT is_blocked FROM erpl_rev_targets WHERE target='blocked'")
              .find("\"is_blocked\":true") != std::string::npos);
    CHECK(One(db, "SELECT fail_count FROM erpl_rev_targets WHERE target='failing'")
              .find("\"fail_count\":3") != std::string::npos);
    // ...and none of them counts as healthy.
    const auto n = One(db, "SELECT count(*) AS c FROM erpl_rev_targets WHERE is_healthy");
    CHECK(n.find("\"c\":0") != std::string::npos);
}

TEST_CASE("stats: health summarises the whole system in one row", "[stats]") {
    // The first thing on any operator screen: how many targets, how many are
    // wrong, and what is the worst lag. One row, so a display never has to
    // aggregate client-side and get it subtly different from the next display.
    DuckDbBridge db;
    SeedHealthy(db, "a");
    SeedHealthy(db, "b");
    SeedHealthy(db, "c");
    db.Execute("UPDATE _erpl_rev_delta_state SET status='BLOCKED' WHERE target='c'");

    const auto r = One(db, "SELECT targets, healthy, blocked, parked, failing FROM erpl_rev_health");
    CHECK(r.find("\"targets\":3") != std::string::npos);
    CHECK(r.find("\"healthy\":2") != std::string::npos);
    CHECK(r.find("\"blocked\":1") != std::string::npos);
}

TEST_CASE("stats: health reports whether the daemon is actually beating", "[stats]") {
    // "Nothing is replicating" is usually the daemon, not the targets. A stale
    // heartbeat says so directly instead of leaving the operator to infer it
    // from every target being late at once.
    DuckDbBridge db;
    db.Execute("UPDATE _erpl_rev_daemon SET status='RUNNING', instance_id='x', "
               "heartbeat_ts = now() - INTERVAL '1 hour' WHERE id=1");

    const auto r = One(db, "SELECT daemon_status, daemon_heartbeat_age_s > 3000 AS stale "
                           "FROM erpl_rev_health");
    CHECK(r.find("\"stale\":true") != std::string::npos);
}

TEST_CASE("stats: a trigger target whose registry is broken is not healthy", "[stats]") {
    // The gap this closes. A trigger set dropped by a transport, a log table
    // removed, a provisioning that never finished -- each leaves
    // _erpl_rev_cdc outside ACTIVE/SEEDED, which silently stops the tick
    // planner scheduling the target. Nothing in _erpl_rev_delta_state changes:
    // it still holds whatever the last SUCCESSFUL cycle wrote. So the target
    // stops replicating and every operator surface reports it healthy, with a
    // lag that ages and a status that reads IDLE.
    //
    // The registry knew the whole time. Nobody who needed to know was told.
    DuckDbBridge db;
    SeedHealthy(db, "trg");
    db.Execute("UPDATE _erpl_rev_delta_state SET method='CDC' WHERE target='trg'");
    db.Execute("INSERT INTO _erpl_rev_cdc (target, source, keys, mode, status, error) "
               "VALUES ('trg','ZSRC','id','KEYS_IUD','ERROR','trigger ZCDC_ZSRC_TRG is missing')");

    CHECK(One(db, "SELECT is_healthy FROM erpl_rev_targets WHERE target='trg'") ==
          R"({"is_healthy":false})");
    CHECK(One(db, "SELECT cdc_status FROM erpl_rev_targets WHERE target='trg'") ==
          R"({"cdc_status":"ERROR"})");
    // The reason travels with it, so the surface can say WHY rather than only
    // that something is wrong.
    CHECK(One(db, "SELECT cdc_error FROM erpl_rev_targets WHERE target='trg'").find(
              "ZCDC_ZSRC_TRG") != std::string::npos);
}

TEST_CASE("stats: a provisioned trigger target that is running is healthy", "[stats]") {
    // The other half, and the one that stops the check above being a check that
    // simply fails everything on the trigger tier. ACTIVE and SEEDED are both
    // states the planner schedules, so both are healthy.
    DuckDbBridge db;
    SeedHealthy(db, "act");
    db.Execute("INSERT INTO _erpl_rev_cdc (target, source, keys, mode, status) "
               "VALUES ('act','ZSRC','id','KEYS_IUD','ACTIVE')");
    CHECK(One(db, "SELECT is_healthy FROM erpl_rev_targets WHERE target='act'") ==
          R"({"is_healthy":true})");

    SeedHealthy(db, "sd");
    db.Execute("INSERT INTO _erpl_rev_cdc (target, source, keys, mode, status) "
               "VALUES ('sd','ZSRC','id','KEYS_IUD','SEEDED')");
    CHECK(One(db, "SELECT is_healthy FROM erpl_rev_targets WHERE target='sd'") ==
          R"({"is_healthy":true})");
}

TEST_CASE("stats: a watermark target is not judged by a trigger registry it has none of",
          "[stats]") {
    // The join is a LEFT one and the columns come back NULL. A target that was
    // never on the trigger tier must read as "no registry", not as "registry
    // broken" -- otherwise this change would mark every watermark target in
    // the system unhealthy at once.
    DuckDbBridge db;
    SeedHealthy(db, "wm");
    CHECK(One(db, "SELECT is_healthy FROM erpl_rev_targets WHERE target='wm'") ==
          R"({"is_healthy":true})");
    CHECK(One(db, "SELECT cdc_status FROM erpl_rev_targets WHERE target='wm'") ==
          R"({"cdc_status":null})");
}
