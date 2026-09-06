// Streaming latency: the statistical view of change-to-target delay.
//
// NFR-2 promises p95 <= 5s and p99 <= 10s at up to 1000 changed rows per tick.
// A promise like that needs a distribution, not an average and not a single
// timing -- the whole point of a percentile is what the tail does.
//
// The change log is the instrument. Every logged row carries when the SOURCE
// says it changed and when erpl-rev applied it, so the latency of every single
// replicated change is recoverable after the fact, with no separate harness and
// no sampling.

#include <catch2/catch_test_macros.hpp>
#include <duckdb.hpp>

#include "control_schema.hpp"
#include "cycle.hpp"
#include "latency.hpp"

using namespace erpl_rev;

namespace {
void Exec(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    INFO(sql);
    REQUIRE_FALSE(r->HasError());
}

// A change log holding `n` rows whose apply lag is a known sequence of seconds.
// Each row gets its own key, so they are distinct CHANGES rather than repeated
// deliveries of one -- the stats group by key plus source timestamp.
void SeedLog(duckdb::Connection &con, const std::vector<int> &lags_secs) {
    auto reg = con.Query(
        "INSERT OR REPLACE INTO _erpl_rev_delta_state "
        "(target, method, source_from, keys, chg_col, wm_kind, cadence, status) VALUES "
        "('t','WATERMARK','T','id','CHANGED_AT','NUMTS','manual','IDLE')");
    REQUIRE_FALSE(reg->HasError());
    const auto log = cycle::ChangeLogName("t");
    Exec(con, "DROP TABLE IF EXISTS " + log);
    Exec(con, "CREATE TABLE " + log + "(id BIGINT, _seq BIGINT, _op VARCHAR, _run_id BIGINT, "
              "_commit_ts TIMESTAMPTZ, _applied_at TIMESTAMPTZ)");
    int i = 0;
    for (int lag : lags_secs) {
        Exec(con, "INSERT INTO " + log + " VALUES (" + std::to_string(i) + "," +
                      std::to_string(i) + ",'U',1, TIMESTAMPTZ '2026-09-05 12:00:00', "
                      "TIMESTAMPTZ '2026-09-05 12:00:00' + INTERVAL '" +
                      std::to_string(lag) + "' SECOND)");
        ++i;
    }
}
}  // namespace

TEST_CASE("latency: a re-delivered change is counted once, at its FIRST apply",
          "[latency]") {
    // The safety overlap deliberately re-reads recently-changed rows, and the
    // keyed merge absorbs the duplicates -- so one source change is logged
    // several times, each with a LATER _applied_at.
    //
    // Counting every log row answers "how long after it changed did we write
    // this particular copy", which nobody asked. The question the promise is
    // about is "how long until the change was visible", and that is the FIRST
    // apply. Measured on real data the difference was p95 9.94s versus 2.08s --
    // the same pipeline, reported five times worse.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, cadence, status) VALUES "
              "('t','WATERMARK','T','id','CHANGED_AT','NUMTS','manual','IDLE')");

    const auto log = cycle::ChangeLogName("t");
    Exec(con, "CREATE TABLE " + log + "(id BIGINT, _seq BIGINT, _op VARCHAR, _run_id BIGINT, "
              "_commit_ts TIMESTAMPTZ, _applied_at TIMESTAMPTZ)");
    // One change to key 1, applied at +1s and then re-delivered at +3s and +5s.
    for (int lag : {1, 3, 5})
        Exec(con, "INSERT INTO " + log + " VALUES (1,1,'U',1, TIMESTAMPTZ '2026-09-05 12:00:00', "
                  "TIMESTAMPTZ '2026-09-05 12:00:00' + INTERVAL '" + std::to_string(lag) +
                  "' SECOND)");
    // A second, distinct change to the same key, delivered once at +2s.
    Exec(con, "INSERT INTO " + log + " VALUES (1,4,'U',2, TIMESTAMPTZ '2026-09-05 12:01:00', "
              "TIMESTAMPTZ '2026-09-05 12:01:02')");

    const auto s = GetLatencyStats(con, "t");
    // Two distinct changes, not four log rows.
    CHECK(s.samples == 2);
    CHECK(s.max == 2.0);       // the 5s re-delivery is not the latency of anything
    CHECK(s.p50 <= 2.0);
}

TEST_CASE("latency: percentiles over the change log", "[latency]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");

    // 100 changes, one second of lag each except a slow tail.
    std::vector<int> lags(95, 1);
    for (int i = 0; i < 5; ++i) lags.push_back(20);   // 5% at 20s
    SeedLog(con, lags);

    const auto s = GetLatencyStats(con, "t");
    CHECK(s.samples == 100);
    CHECK(s.p50 == 1.0);
    // The tail is the point: a mean would report 1.95s and hide it entirely.
    CHECK(s.p99 >= 19.0);
    CHECK(s.max >= 20.0);
}

TEST_CASE("latency: a mean would hide exactly what the promise is about",
          "[latency]") {
    // Stated as a test because it is the reason this is not one number: a
    // distribution where 99% is instant and 1% takes a minute has a mean under a
    // second and violates every tail guarantee in the BRD.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");

    // 5% slow, so the slow group genuinely reaches p99. With a SINGLE outlier
    // in 100 the honest answer is that p99 is still fast and the outlier is
    // p100 -- which is why max is reported alongside, and why a percentile
    // alone is not a substitute for looking at the maximum.
    std::vector<int> lags(95, 0);
    for (int i = 0; i < 5; ++i) lags.push_back(60);
    SeedLog(con, lags);

    const auto s = GetLatencyStats(con, "t");
    CHECK(s.mean < 5.0);      // looks fine on average
    CHECK(s.p99 >= 60.0);     // ...and one change in twenty took a minute
    CHECK(s.max >= 60.0);
}

TEST_CASE("latency: rows with no source timestamp are excluded, not counted as zero",
          "[latency]") {
    // A snapshot or full-load row has no source change time. Treating a NULL as
    // zero latency would silently improve every percentile.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    SeedLog(con, {5, 5, 5, 5});
    const auto log = cycle::ChangeLogName("t");
    Exec(con, "INSERT INTO " + log + " VALUES (99,99,'I',1,NULL,now())");

    const auto s = GetLatencyStats(con, "t");
    CHECK(s.samples == 4);
    CHECK(s.p50 == 5.0);
}

TEST_CASE("latency: an empty or absent log reports no samples, not zero latency",
          "[latency]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");

    const auto none = GetLatencyStats(con, "never_logged");
    CHECK(none.samples == 0);
    CHECK_FALSE(none.has_data);

    SeedLog(con, {});
    const auto empty = GetLatencyStats(con, "t");
    CHECK(empty.samples == 0);
    CHECK_FALSE(empty.has_data);
}

TEST_CASE("latency: the verdict states the promise it is judged against",
          "[latency]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");

    SeedLog(con, std::vector<int>(100, 2));
    auto s = GetLatencyStats(con, "t");
    CHECK(MeetsTarget(s, 5.0, 10.0));
    CHECK(Describe(s).find("p95") != std::string::npos);

    SeedLog(con, std::vector<int>(100, 30));
    s = GetLatencyStats(con, "t");
    CHECK_FALSE(MeetsTarget(s, 5.0, 10.0));
}

TEST_CASE("latency: a window restricts the sample to recent changes", "[latency]") {
    // A target that was slow an hour ago and is fine now should read as fine now;
    // otherwise one bad episode poisons the number for as long as the log is
    // retained.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    const auto log = cycle::ChangeLogName("t");
    Exec(con, "DROP TABLE IF EXISTS " + log);
    Exec(con, "CREATE TABLE " + log + "(id BIGINT, _seq BIGINT, _op VARCHAR, _run_id BIGINT, "
              "_commit_ts TIMESTAMPTZ, _applied_at TIMESTAMPTZ)");
    // Old and slow.
    Exec(con, "INSERT INTO " + log + " VALUES (1,1,'U',1, now() - INTERVAL '2' HOUR, "
              "now() - INTERVAL '2' HOUR + INTERVAL '60' SECOND)");
    // Recent and fast.
    Exec(con, "INSERT INTO " + log + " VALUES (2,2,'U',2, now() - INTERVAL '10' SECOND, "
              "now() - INTERVAL '9' SECOND)");

    CHECK(GetLatencyStats(con, "t").samples == 2);
    const auto recent = GetLatencyStats(con, "t", /*window_secs=*/300);
    CHECK(recent.samples == 1);
    CHECK(recent.max < 5.0);
}
