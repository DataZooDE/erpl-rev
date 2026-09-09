// The Prometheus exposition text.
//
// Rendering is a pure function of the operational views, so it is tested
// without a socket: the listener's job is to hand this string back, and
// everything that can be wrong about the metrics is wrong in here.

#include <catch2/catch_test_macros.hpp>
#include <duckdb.hpp>

#include <string>

#include "duckdb_bridge.hpp"
#include "metrics.hpp"

using namespace erpl_rev;

TEST_CASE("metrics: a target's lag and health are exposed per target", "[metrics]") {
    DuckDbBridge db;
    db.Execute("INSERT INTO _erpl_rev_delta_state "
               "(target, method, source_from, keys, cadence, last_run_ts, rows_applied) "
               "VALUES ('sales','WATERMARK','VBAK','id','micro:5', "
               "now() - INTERVAL '30 seconds', 42)");

    const auto text = metrics::Render(db);

    // A scraper needs the type declared or it guesses, and it guesses wrong for
    // anything that is not a counter.
    CHECK(text.find("# TYPE erpl_rev_target_lag_seconds gauge") != std::string::npos);
    CHECK(text.find("erpl_rev_target_lag_seconds{target=\"sales\"}") != std::string::npos);
    CHECK(text.find("erpl_rev_target_healthy{target=\"sales\"} 1") != std::string::npos);
    CHECK(text.find("erpl_rev_target_rows_applied{target=\"sales\"} 42") != std::string::npos);
}

TEST_CASE("metrics: a target that has never run emits no lag rather than zero",
          "[metrics]") {
    // Zero would read as "perfectly current" on every dashboard and alert rule
    // there is. Absent is the honest encoding: Prometheus treats a missing
    // series as no data, which is exactly what it is.
    DuckDbBridge db;
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, cadence) "
               "VALUES ('never','WATERMARK','T','id','micro:5')");

    const auto text = metrics::Render(db);
    CHECK(text.find("erpl_rev_target_lag_seconds{target=\"never\"}") == std::string::npos);
    // ...but it is still visible as unhealthy, or it would vanish from the
    // dashboard entirely and look like it does not exist.
    CHECK(text.find("erpl_rev_target_healthy{target=\"never\"} 0") != std::string::npos);
}

TEST_CASE("metrics: the daemon's heartbeat is exposed", "[metrics]") {
    // "Nothing is replicating" is usually the daemon. Without this, every
    // target goes stale at once and the alert points at all of them.
    DuckDbBridge db;
    db.Execute("UPDATE _erpl_rev_daemon SET status='RUNNING', instance_id='x', "
               "heartbeat_ts = now() - INTERVAL '5 seconds' WHERE id=1");

    const auto text = metrics::Render(db);
    CHECK(text.find("# TYPE erpl_rev_daemon_heartbeat_age_seconds gauge") != std::string::npos);
    CHECK(text.find("erpl_rev_daemon_up 1") != std::string::npos);
}

TEST_CASE("metrics: a target name with a quote cannot break the exposition",
          "[metrics]") {
    // Target names are customer-chosen. An unescaped quote in a label ends the
    // label early and every metric after it is silently discarded by the
    // scraper -- a monitoring outage caused by a table name.
    DuckDbBridge db;
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, cadence, "
               "last_run_ts) VALUES ('we\"ird','WATERMARK','T','id','micro:5', now())");

    const auto text = metrics::Render(db);
    CHECK(text.find("target=\"we\\\"ird\"") != std::string::npos);
}
