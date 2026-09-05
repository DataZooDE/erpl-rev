// The cycle contract.
//
//   Begin(target, load_type, read_start)
//       allocates the run id, computes the read bounds, claims the target with a
//       fencing token, opens the stats row, discards any orphaned stage from a
//       previous run, and names this run's stage.
//
//   Commit(target, run_id, counts)
//       ONE transaction: merge the stage into the target, append the change log,
//       advance the watermark, finish the stats row, drop the stage.
//
// The invariant everything rests on: wm_value advances only inside Commit, after
// the merge. A cycle that dies at any earlier point therefore leaves the
// watermark where it was -- so the read is simply replayed, and the orphaned
// stage is free to discard.
//
// The fencing token is `active_run_id`, not the lease. A healthy cycle can
// legitimately block for longer than any lease TTL (the ingest pipe waits up to
// an hour), so the lease is advisory; the compare-and-swap in Commit is what
// actually prevents a reclaimed target being written by the cycle it was taken
// from.
//
// Free functions over a duckdb::Connection rather than methods on DuckDbBridge,
// so this is drivable from a bare in-memory DuckDB with no bridge and no RFC.
#pragma once

#include <duckdb.hpp>

#include <string>

#include "delta_plan.hpp"
#include "load_type.hpp"

namespace erpl_rev {
namespace cycle {

struct BeginResult {
    long long run_id = 0;
    wm::Bounds bounds;
    LoadPlan plan;
    std::string stage_table;
    std::string chg_col;
    std::string time_col;
    std::string keys;
    std::string source_from;
};

struct CommitCounts {
    long long rows_read = 0;
};

struct CommitResult {
    long long ins = 0, upd = 0, del = 0, logged = 0;
    std::string new_watermark;
};

BeginResult Begin(duckdb::Connection &con, const std::string &target, LoadType load_type,
                  int64_t read_start_epoch);

CommitResult Commit(duckdb::Connection &con, const std::string &target, long long run_id,
                    const CommitCounts &counts);

// Per-target change log. Named through the collision-safe token, because the
// input is a customer-chosen target name.
std::string ChangeLogName(const std::string &target);

// The stage a run writes into: named for the run, so an orphan identifies itself
// and cleanup is a DROP over names that do not match an in-flight run.
std::string StageName(const std::string &target, long long run_id);

}  // namespace cycle
}  // namespace erpl_rev
