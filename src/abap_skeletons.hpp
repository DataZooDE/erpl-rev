// The ABAP the CLI generates, and the machinery that deploys and runs it.
//
// These live as C++ raw strings rather than in abap/ because they are
// scaffolding, not product: `setup` deploys abap::ProductionAssets() to a
// customer's system, and a throwaway class must never end up in that list.
//
// The cost of that choice is that nothing here is syntax-checked at build
// time. The e2e suite activates each skeleton against a real system, which is
// the only thing standing between a typo and a customer's first `replicate`.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "adt.hpp"

namespace erpl_rev::abapgen {

// Parameters for a full load; mirrors Z_ERPL_REV_REPLICATE's selection screen
// and zcl_erpl_rev_util=>replicate.
struct ReplicateParams {
    std::string table, target, columns, where, cds_params, init;
    std::string mode = "UPSERT";     // UPSERT | INSERT
    long long batch = 50000;
    long long maxrows = 0;
    bool truncate = true;
    bool verify = true;
    bool parallel = false;
    std::string part_col;
    long long jobs = 0;
    std::string target_kind = "duckdb";   // duckdb | parquet | table
    std::string dest, partition_by;
};

// One delta target, as zcl_erpl_rev_delta=>register takes it.
struct SyncState {
    std::string target, method, source_from, keys, chg_col, wm_kind, wm_value;
    // The TIMS half of a DATETIME pair. Absent from the skeleton path for its
    // whole life, which is how a DATETIME target could register cleanly,
    // replicate one batch and then silently stop.
    std::string time_col;
    long long safety_secs = 120;
    long long safety_units = 0;   // counter kinds; a duration means nothing there
    std::string cadence = "nightly";
    std::string extra;
    bool log_enabled = false;             // keep a per-target change log
    std::string load_type_default;        // D | F | I | L; F and L are one-shot
    bool allow_empty_reload = false;      // let a reload empty the target
};

// The register call's fields, in one ordered list, as {ABAP field name, ABAP
// literal}. Both writers of the register surface render from this: the
// S_DEVELOP-free driver path and the generated-ABAP path.
//
// One list because there were two, and they drifted: the generated skeleton
// omitted time_col and safety_units entirely, so a field added to the state
// arrived at the server empty and the feature that needed it simply did
// nothing -- no compile error, no runtime error.
std::vector<std::pair<std::string, std::string>> RegisterFields(const SyncState &s);

// Each renderer returns complete, deployable ABAP with `nonce` woven into its
// result lines. They throw UnsafeValue if any parameter cannot be embedded.
std::string RenderReplicate(const ReplicateParams &p, const std::string &nonce);
std::string RenderSyncRegister(const SyncState &s, const std::string &nonce);
std::string RenderSyncRun(const std::string &target, const std::string &nonce);
std::string RenderSchedule(long long minutes, bool remove, const std::string &nonce);

// Counts the periodic ERPL_REV_DELTA jobs actually present in TBTCO. Scheduling
// happens in a background job, so "submitted" is not evidence that anything was
// scheduled -- this is what turns it into evidence.
std::string RenderJobCheck(const std::string &nonce);

// ---------------------------------------------------------------------------
// Deploying one of them
// ---------------------------------------------------------------------------

// Creates a uniquely-named class in $TMP, activates it, and deletes it again
// when it goes out of scope -- on every path, including an exception.
//
// $TMP is hardcoded and not configurable: a throwaway object in a real package
// would need a transport request and would pollute it.
class TempClassrun {
public:
    // `nonce` must be the same one the source was rendered with: the class
    // name is baked into that source, so a second nonce here would deploy a
    // class under a name the source does not declare.
    TempClassrun(const adt::Conn &conn, const std::string &kind_letter,
                 const std::string &nonce, bool keep);
    ~TempClassrun();
    TempClassrun(const TempClassrun &) = delete;
    TempClassrun &operator=(const TempClassrun &) = delete;

    // Writes and activates `source`. Returns an empty string on success, or a
    // human-readable reason.
    std::string Deploy(const std::string &source);

    // Runs it. `out` receives the console output whatever happens.
    std::string Run(std::string &out);

    const std::string &name() const { return name_; }
    // The exact command to remove it by hand, for when cleanup fails.
    std::string DeleteHint() const;

private:
    adt::Conn conn_;
    std::string name_;
    bool keep_;
    bool created_ = false;
};

} // namespace erpl_rev::abapgen
