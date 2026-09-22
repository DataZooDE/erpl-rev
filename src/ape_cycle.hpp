// ApeCycle APE_FULL: decoded package -> staging -> snapshot merge.
//
// ABAP drives the DHAPE session (create/poll/stop in one SAP session, so
// affinity holds by construction) and hands each raw PORT_DATA envelope to
// the server. This layer decodes it with the tested pure codec
// (ape_envelope/ape_decode), stages the rows in <target>__apesnap, and on
// the last batch reconciles the staging onto the target with the same
// set-based merge the SNAPSHOT method uses: upsert present + delete absent
// (CURR semantics, BRD BR-3). No spill for FULL: a failed scan just re-runs
// (HLD §6); the spill table is the DELTA crash copy (Phase 3).
#pragma once

#include <string>
#include <vector>

#include "ape_decode.hpp"

namespace erpl_rev {

class DuckDbBridge;

namespace ape {

struct ApeApplyResult {
    long long rows_staged = 0;
    bool last_batch = false;
    long long batch_index = -1;
};

struct ApeFullCounts {
    long long ins = 0, upd = 0, del = 0;
};

// The FULL-scan staging table. Distinct from the SNAPSHOT method's __snap so
// the two never share a half-built stage.
std::string ApeStagingName(const std::string &target);

// Decode one PORT_DATA envelope and append its rows to the staging table
// (created from the target's shape on batch 0). Control envelopes stage
// nothing but still report last_batch. Throws ApeDecodeError (ape_decode.hpp)
// -- unknown target, missing staging, row-width mismatch, a target column the
// package does not carry, or a D operation inside a FULL scan.
ApeApplyResult ApeApplyFullPackage(DuckDbBridge &db, const std::string &target,
                                   const std::string &package_json,
                                   long long batch_index);

// Reconcile the staging onto the target (SnapshotMerge: drops the staging).
ApeFullCounts ApeFinalizeFull(DuckDbBridge &db, const std::string &target,
                              const std::vector<std::string> &keys);

struct ApeDeltaCounts {
    long long rows_applied = 0;
    long long upserted = 0;  // U rows (blank counts as U, BR-3)
    long long deleted = 0;   // D rows
};

// One replication package: decode, spill the raw envelope to
// _erpl_rev_ape_spill BEFORE merging (ADR-2: the crash copy), then merge
// onto the target (U = keyed upsert of the after-image, D = delete by key).
// Advances spill_batch past the merged batch, so a crash between spill and
// merge replays exactly the unmerged tail. Empty polls (control envelopes)
// spill nothing and advance nothing. Throws ApeDecodeError on anything
// refusal-shaped: unknown target, missing registration, no operation column,
// row-width mismatch, a target column the package does not carry.
ApeDeltaCounts ApeApplyDeltaPackage(DuckDbBridge &db, const std::string &target,
                                    const std::vector<std::string> &keys,
                                    const std::string &package_json,
                                    long long batch_index);

// Crash replay without touching SAP: re-apply every spilled batch past
// spill_batch, in order, then discard what was replayed. Keyed merge absorbs
// the re-delivery of already-merged batches (FR-6/AC-2). No spill rows past
// the position is a no-op.
ApeDeltaCounts ApeRecover(DuckDbBridge &db, const std::string &target,
                          const std::vector<std::string> &keys);

}  // namespace ape
}  // namespace erpl_rev
