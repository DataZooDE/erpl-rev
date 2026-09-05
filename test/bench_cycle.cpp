// P-CYCLE: what unconditional staging costs the delta cycle.
//
// The cycle used to merge each package straight into the target. It now stages
// the whole read and commits once -- which is what buys merge + change-log +
// watermark-advance in ONE transaction, and what makes a crashed cycle
// replayable. That guarantee is not free, and this measures the bill.
//
// Both arms do the same useful work (get N changed rows into the target); they
// differ only in how. Hidden by default ([.]); run with:
//   ./erpl_rev_tests "[bench]"

#include <catch2/catch_test_macros.hpp>
#include <duckdb.hpp>

#include <chrono>
#include <cstdio>
#include <string>

#include "control_schema.hpp"
#include "cycle.hpp"

using namespace erpl_rev;
using Clock = std::chrono::steady_clock;

namespace {

double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

void Run(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    if (r->HasError()) throw std::runtime_error(sql + " -> " + r->GetError());
}

// A target with `base` rows, and a source of `changed` rows that overlap it.
void Seed(duckdb::Connection &con, long long base) {
    Run(con, "DROP TABLE IF EXISTS t");
    Run(con, "CREATE TABLE t(id BIGINT PRIMARY KEY, v VARCHAR, changed_at VARCHAR)");
    Run(con, "INSERT INTO t SELECT i, 'v' || i, '20260101000000' FROM range(" +
                 std::to_string(base) + ") tbl(i)");
}

void SeedChanges(duckdb::Connection &con, const std::string &rel, long long n) {
    Run(con, "DROP TABLE IF EXISTS " + rel);
    Run(con, "CREATE TABLE " + rel + " AS SELECT i AS id, 'new' || i AS v, "
             "'20260905120000' AS changed_at FROM range(" + std::to_string(n) + ") tbl(i)");
}

}  // namespace

TEST_CASE("P-CYCLE: direct merge vs staged commit", "[bench][.]") {
    constexpr long long kBase = 1'000'000;

    std::printf("\n  rows |   direct (ms) |   staged (ms) |  staged+log (ms) | overhead | 1st cycle\n");
    std::printf("-------+---------------+---------------+------------------+----------+----------\n");

    for (long long n : {10LL, 100LL, 1'000LL, 10'000LL, 100'000LL}) {
        duckdb::DuckDB db(nullptr);
        duckdb::Connection con(db);
        schema::Migrate(con, "bench");

        // --- old path: one MERGE from the read straight into the target -------
        Seed(con, kBase);
        SeedChanges(con, "src", n);
        auto t0 = Clock::now();
        Run(con, "MERGE INTO t USING src AS s ON t.id = s.id "
                 "WHEN MATCHED THEN UPDATE SET v = s.v, changed_at = s.changed_at "
                 "WHEN NOT MATCHED THEN INSERT (id, v, changed_at) "
                 "VALUES (s.id, s.v, s.changed_at)");
        auto t1 = Clock::now();
        const double direct = Ms(t0, t1);

        // --- new path: stage, then one transactional commit -------------------
        Seed(con, kBase);
        Run(con, "DELETE FROM _erpl_rev_delta_state");
        Run(con, "INSERT INTO _erpl_rev_delta_state "
                 "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_secs, "
                 " cadence, status, log_enabled) VALUES "
                 "('t','WATERMARK','T','id','CHANGED_AT','NUMTS','20260101000000',120,"
                 "'manual','IDLE',false)");
        auto b = cycle::Begin(con, "t", LoadType::Delta, 1788609600);
        SeedChanges(con, b.stage_table, n);
        t0 = Clock::now();
        cycle::Commit(con, "t", b.run_id, {n});
        t1 = Clock::now();
        const double staged = Ms(t0, t1);

        // --- new path with the change log on ----------------------------------
        // Measured in STEADY STATE. The first cycle for a target also provisions
        // the log table and its sequence; charging that one-off to every cycle
        // would overstate the ongoing cost by an order of magnitude on small
        // batches -- which is exactly the mistake the first version of this
        // benchmark made.
        Seed(con, kBase);
        Run(con, "UPDATE _erpl_rev_delta_state SET log_enabled=true, wm_value='20260101000000', "
                 "status='IDLE', active_run_id=NULL");
        Run(con, "DROP TABLE IF EXISTS " + cycle::ChangeLogName("t"));

        auto warm = cycle::Begin(con, "t", LoadType::Delta, 1788609600);
        SeedChanges(con, warm.stage_table, n);
        t0 = Clock::now();
        cycle::Commit(con, "t", warm.run_id, {n});
        t1 = Clock::now();
        const double first_logged = Ms(t0, t1);

        Seed(con, kBase);
        Run(con, "UPDATE _erpl_rev_delta_state SET wm_value='20260101000000', "
                 "status='IDLE', active_run_id=NULL");
        auto b2 = cycle::Begin(con, "t", LoadType::Delta, 1788609600);
        SeedChanges(con, b2.stage_table, n);
        t0 = Clock::now();
        cycle::Commit(con, "t", b2.run_id, {n});
        t1 = Clock::now();
        const double logged = Ms(t0, t1);

        std::printf("%6lld | %13.1f | %13.1f | %16.1f | %+7.0f%% | %8.1f\n",
                    n, direct, staged, logged,
                    direct > 0 ? (staged - direct) / direct * 100.0 : 0.0, first_logged);
    }
    std::printf("\n  (1,000,000-row target; the staged arm also advances the watermark,\n"
                "   updates run statistics and drops its staging table.)\n\n");
    SUCCEED();
}
