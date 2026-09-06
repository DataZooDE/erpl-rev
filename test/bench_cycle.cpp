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

#include <algorithm>
#include <chrono>
#include <vector>
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

// P-STAGE-PK: what building a PRIMARY KEY on the staging table costs.
//
// The ingest path builds a PK on whatever it writes into (iv_build_pk defaults
// on), which is right for a full load: the target keeps that index for the rest
// of its life. A delta stage lives for one cycle and is dropped.
//
// Read the result as a COST, not as a comparison. In Commit the stage is only
// ever the driving side -- the pre-counts are `FROM stage s WHERE [NOT] EXISTS
// (... target ...)`, the log append is `FROM stage s LEFT JOIN target`, and the
// merge is `MERGE INTO target USING stage` -- so every keyed lookup hits the
// target, which has its own PK. The join predicate is `IS NOT DISTINCT FROM`,
// which an ART index does not serve anyway. A stage-side index therefore cannot
// be consulted by any statement here, and this benchmark cannot show one paying
// off; what it measures is what an index nothing queries costs to build.
//
// Each arm is repeated and reported as a MEDIAN, over three batch shapes:
// update-only (every staged key already in the target), insert-heavy (none of
// them), and mixed. A single unwarmed sample per arm is what the first version
// of this reported, and the small-row rows were inside its noise.
TEST_CASE("P-STAGE-PK: a primary key on the delta stage", "[bench][.]") {
    constexpr long long kBase = 1'000'000;
    constexpr int kReps = 5;

    struct Shape { const char *name; long long first_id; };
    // first_id decides the overlap with a target holding ids [0, kBase):
    // 0 means every staged key already exists, kBase means none do.
    const Shape shapes[] = {{"update-only", 0}, {"insert-heavy", kBase}, {"mixed", 0}};

    std::printf("\n        shape |   rows | no stage PK (ms) | stage PK (ms) | of which build\n");
    std::printf("--------------+--------+------------------+---------------+---------------\n");

    for (const auto &shape : shapes) {
        for (long long n : {1'000LL, 10'000LL, 100'000LL}) {
            duckdb::DuckDB db(nullptr);
            duckdb::Connection con(db);
            schema::Migrate(con, "bench");
            Run(con, "INSERT INTO _erpl_rev_delta_state "
                     "(target, method, source_from, keys, chg_col, wm_kind, wm_value, "
                     " safety_secs, cadence, status, log_enabled) VALUES "
                     "('t','WATERMARK','T','id','CHANGED_AT','NUMTS','20260101000000',120,"
                     "'manual','IDLE',false)");

            auto cycle_once = [&](bool with_pk) {
                Seed(con, kBase);
                Run(con, "UPDATE _erpl_rev_delta_state SET wm_value='20260101000000', "
                         "status='IDLE', active_run_id=NULL");
                auto b = cycle::Begin(con, "t", LoadType::Delta, 1788609600);
                Run(con, "DROP TABLE IF EXISTS " + b.stage_table);
                // "mixed" straddles the boundary: half the staged keys exist.
                const long long start = std::string(shape.name) == "mixed"
                                            ? kBase - n / 2
                                            : shape.first_id;
                Run(con, "CREATE TABLE " + b.stage_table + " AS SELECT " +
                             std::to_string(start) + " + i AS id, 'new' || i AS v, "
                             "'20260905120000' AS changed_at FROM range(" +
                             std::to_string(n) + ") tbl(i)");

                double build = 0;
                if (with_pk) {
                    const auto p0 = Clock::now();
                    Run(con, "ALTER TABLE " + b.stage_table + " ADD PRIMARY KEY (id)");
                    build = Ms(p0, Clock::now());
                }
                const auto t0 = Clock::now();
                cycle::Commit(con, "t", b.run_id, {n});
                return std::pair<double, double>{Ms(t0, Clock::now()), build};
            };

            // Median of kReps, and one discarded warm-up per arm: a single
            // unwarmed sample is an anecdote, and the first version of this
            // benchmark reported five of them as a trend.
            auto median = [](std::vector<double> v) {
                std::sort(v.begin(), v.end());
                return v[v.size() / 2];
            };
            std::vector<double> plain, keyed, builds;
            cycle_once(false);
            cycle_once(true);
            for (int r = 0; r < kReps; ++r) {
                plain.push_back(cycle_once(false).first);
                const auto k = cycle_once(true);
                keyed.push_back(k.first + k.second);
                builds.push_back(k.second);
            }

            std::printf("%13s | %6lld | %16.1f | %13.1f | %14.1f\n", shape.name, n,
                        median(plain), median(keyed), median(builds));
        }
    }
    std::printf("\n  (1,000,000-row target, median of %d cycles per arm after a warm-up.\n"
                "   The 'stage PK' column includes the time to build the index, because a\n"
                "   delta stage is created, indexed, read once and dropped -- the build is\n"
                "   part of the cycle, not setup. No statement in Commit can consult a\n"
                "   stage-side index, so this is a cost, not a comparison.)\n\n", kReps);
    SUCCEED();
}
