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
#include <vector>

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

// `sap_now` is SAP's own clock as YYYYMMDDHHMMSS, and it is not optional
// decoration: a DATS/TIMS column is wall-clock in the SAP SYSTEM's timezone,
// which the server cannot know and must not guess. Measured on A4H the two
// differed by two hours, and the symptom was not "wrong timezone" -- it was a
// DATETIME target that replicated its first batch and then silently stopped,
// because every subsequent floor sat in the future.
//
// Empty falls back to the server clock, which is correct for the UTC-based
// TIMESTAMPL kinds and is the best available answer for the rest.
BeginResult Begin(duckdb::Connection &con, const std::string &target, LoadType load_type,
                  int64_t read_start_epoch, const std::string &sap_now = "");

CommitResult Commit(duckdb::Connection &con, const std::string &target, long long run_id,
                    const CommitCounts &counts);

// The fence, as one named operation: FINISH the cycle -- store the new
// watermark, release the target (status IDLE, active_run_id NULL), reset the
// failure count and record rows_applied -- but ONLY while this run still owns
// it.
//
// Named for all of that rather than for the watermark alone: a caller reaching
// for "advance the watermark" would silently release a target they still hold.
//
// MUST be called inside an open transaction. Its throw is the rollback signal
// for the whole commit; called outside one it releases the target while the
// merge that should have accompanied it is already durable.
//
// It is separate from Commit so it can be tested for the case that matters and
// is otherwise unreachable -- the target reclaimed by another cycle -- without
// having to reproduce a race. Commit calls it inside its transaction, so a
// throw here rolls the whole cycle back.
//
// Throws when no row was updated. A zero-row UPDATE is not a DuckDB error, and
// swallowing it is precisely the silent outcome fencing exists to prevent: the
// stage merged, the log appended, SUCCESS reported, and a watermark that was
// never stored.
void FinishCycleFenced(duckdb::Connection &con, const std::string &target, long long run_id,
                            const std::string &new_watermark, long long rows_applied);

// The failure counterpart of FinishCycleFenced: record that this run failed and
// release the target -- but, like its sibling, ONLY while this run still owns
// it. A run whose commit threw because it had already lost the target must not
// release the successor that took it.
//
// Called from Commit's catch block, OUTSIDE the rolled-back transaction, because
// the failure has to survive the rollback.
void ReleaseFailedCycle(duckdb::Connection &con, const std::string &target, long long run_id);

// Per-target change log. Named through the collision-safe token, because the
// input is a customer-chosen target name.
std::string ChangeLogName(const std::string &target);

// Create the change log for `target` if it is missing, and widen it to carry
// `cols` if the target has grown columns since. Both replication tiers call
// this: the watermark cycle and the trigger apply. The trigger tier used to
// have no provisioner at all -- it probed for the table and skipped the append
// when it was absent -- so a log-enabled CDC target had no log, forever, and
// said nothing.
void EnsureChangeLog(duckdb::Connection &con, const std::string &target,
                     const std::vector<std::string> &cols);

// The stage a run writes into: named for the run, so an orphan identifies itself
// and cleanup is a DROP over names that do not match an in-flight run.
std::string StageName(const std::string &target, long long run_id);

}  // namespace cycle
}  // namespace erpl_rev
