#include "cycle.hpp"

#include <stdexcept>
#include <vector>

#include "sql_name.hpp"

namespace erpl_rev {
namespace cycle {

namespace {

void Exec(duckdb::Connection &con, const std::string &sql, const char *what) {
    auto r = con.Query(sql);
    if (r->HasError())
        throw std::runtime_error(std::string("cycle: ") + what + " failed: " + r->GetError());
}

std::string Scalar(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    if (r->HasError()) throw std::runtime_error("cycle: query failed: " + r->GetError());
    if (r->RowCount() == 0 || r->GetValue(0, 0).IsNull()) return "";
    return r->GetValue(0, 0).ToString();
}

std::string Lit(const std::string &v) {
    std::string out = "'";
    for (char c : v) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}

std::vector<std::string> SplitCsv(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string Lower(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

struct State {
    std::string method, source_from, keys, chg_col, time_col, wm_kind, wm_value;
    long long safety_secs = 120, safety_units = 0;
    long long max_cycle_secs = 3600;
    bool log_enabled = false;
    bool allow_empty_reload = false;
};

State LoadState(duckdb::Connection &con, const std::string &target) {
    auto r = con.Query(
        "SELECT method, source_from, keys, coalesce(chg_col,''), coalesce(wm_kind,'NUMTS'), "
        "coalesce(wm_value,''), coalesce(safety_secs,120), coalesce(time_col,''), "
        "coalesce(safety_units,0), coalesce(log_enabled,false), "
        "coalesce(allow_empty_reload,false), "
        "coalesce(max_cycle_secs,3600) "
        "FROM _erpl_rev_delta_state WHERE target=" + Lit(target));
    if (r->HasError()) throw std::runtime_error("cycle: state read failed: " + r->GetError());
    if (r->RowCount() == 0) throw std::runtime_error("cycle: no delta registration for " + target);
    State s;
    s.method = r->GetValue(0, 0).ToString();
    s.source_from = r->GetValue(1, 0).ToString();
    s.keys = r->GetValue(2, 0).ToString();
    s.chg_col = r->GetValue(3, 0).ToString();
    s.wm_kind = r->GetValue(4, 0).ToString();
    s.wm_value = r->GetValue(5, 0).ToString();
    s.safety_secs = r->GetValue(6, 0).GetValue<int64_t>();
    s.time_col = r->GetValue(7, 0).ToString();
    s.safety_units = r->GetValue(8, 0).GetValue<int64_t>();
    s.log_enabled = r->GetValue(9, 0).GetValue<bool>();
    s.allow_empty_reload = r->GetValue(10, 0).GetValue<bool>();
    s.max_cycle_secs = r->GetValue(11, 0).GetValue<int64_t>();
    return s;
}

}  // namespace

// Provision the per-target change log: the table, its control columns and its
// sequence, reconciled against the target's shape. Extracted because the CDC
// tier had no provisioner at all -- it probed for the table and silently wrote
// nothing when it was missing, so a log-enabled trigger target never got a log
// and never noticed.
void EnsureChangeLog(duckdb::Connection &con, const std::string &target,
                     const std::vector<std::string> &cols) {
    const std::string log = ChangeLogName(target);
    // Provision the log ONCE, not on every commit. This block used to run
    // its CREATE, five duckdb_columns() probes and a CREATE SEQUENCE every
    // cycle: a flat ~40ms whatever the row count, which on a 1000-row
    // cycle was six times the cost of the work itself. Measured by
    // bench_cycle before anyone had to notice it in production.
    const bool log_exists =
        Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name=" + Lit(log))
        == "1";
    if (log_exists) {
        // Reconcile with the target. The log is created once from the
        // target's shape, so a column the structure watchdog later adds
        // exists on the target and not here -- and the next INSERT names
        // it, fails, and that target never commits again. Cheap: a
        // catalogue probe per column, only for logged targets.
        for (const auto &c : cols) {
            const auto have = Scalar(con, "SELECT count(*) FROM duckdb_columns() "
                                          "WHERE table_name=" + Lit(log) +
                                              " AND lower(column_name)=" + Lit(c));
            if (have == "0") {
                const auto type = Scalar(con, "SELECT data_type FROM duckdb_columns() "
                                              "WHERE table_name=" + Lit(Lower(target)) +
                                                  " AND lower(column_name)=" + Lit(c));
                if (!type.empty())
                    Exec(con, "ALTER TABLE " + log + " ADD COLUMN " + c + " " + type,
                         "widen change log");
            }
        }
    } else {
        Exec(con, "CREATE TABLE " + log + " AS SELECT * FROM " + target + " LIMIT 0",
             "create change log");
        // _commit_ts is when the SOURCE says the row changed; _applied_at
        // is when erpl-rev wrote it. Two columns, not one: the difference
        // between them IS the replication latency, and a single column
        // filled from whichever clock was nearest measures nothing.
        for (const auto &c : {std::string("_seq BIGINT"), std::string("_op VARCHAR"),
                              std::string("_run_id BIGINT"),
                              std::string("_commit_ts TIMESTAMPTZ"),
                              std::string("_applied_at TIMESTAMPTZ")})
            Exec(con, "ALTER TABLE " + log + " ADD COLUMN " + c, "log column");
        Exec(con, "CREATE SEQUENCE IF NOT EXISTS " + log + "_seq START 1", "log sequence");
    }
}

void AdvanceWatermarkFenced(duckdb::Connection &con, const std::string &target,
                            long long run_id, const std::string &new_watermark,
                            long long rows_applied) {
    auto up = con.Query("UPDATE _erpl_rev_delta_state SET wm_value=" + Lit(new_watermark) +
                        ", status='IDLE', last_run_ts=now(), active_run_id=NULL, fail_count=0, "
                        "rows_applied=" + std::to_string(rows_applied) + " WHERE target=" +
                        Lit(target) + " AND active_run_id=" + std::to_string(run_id));
    if (up->HasError())
        throw std::runtime_error("cycle: advance watermark failed: " + up->GetError());
    // DuckDB reports the affected count as the single result value. Zero means
    // the WHERE did not match -- somebody else owns the target now.
    const auto changed =
        up->RowCount() > 0 && !up->GetValue(0, 0).IsNull() ? up->GetValue(0, 0).GetValue<int64_t>()
                                                           : 0;
    if (changed == 0)
        throw std::runtime_error("cycle: run " + std::to_string(run_id) + " lost ownership of " +
                                 target +
                                 " while committing; rolling back rather than reporting a "
                                 "watermark that was never stored");
}

std::string ChangeLogName(const std::string &target) {
    return "_erpl_rev_log_" + Lower(sqlname::UniqueToken(target));
}

std::string StageName(const std::string &target, long long run_id) {
    return Lower(sqlname::UniqueToken(target)) + "__stg_" + std::to_string(run_id);
}

BeginResult Begin(duckdb::Connection &con, const std::string &target, LoadType load_type,
                  int64_t read_start_epoch, const std::string &sap_now) {
    const State st = LoadState(con, target);

    BeginResult out;
    out.plan = PlanLoad(load_type);
    out.chg_col = st.chg_col;
    out.time_col = st.time_col;
    out.keys = st.keys;
    out.source_from = st.source_from;

    wm::WatermarkSpec spec;
    spec.kind = wm::ParseKind(st.wm_kind);
    spec.chg_col = st.chg_col;
    spec.time_col = st.time_col;
    // A run that reads everything has no floor by definition, whatever is stored.
    spec.wm_value = out.plan.apply_floor ? st.wm_value : std::string();
    spec.safety_secs = st.safety_secs;
    spec.safety_units = st.safety_units;
    // A DATS or TIMS column is wall-clock in SAP's timezone. The server's own
    // clock is the wrong ruler for it, so ABAP passes its own and the bounds are
    // computed against that. For the UTC-based timestamp kinds the server clock
    // is right and is used as-is.
    int64_t read_start = read_start_epoch;
    int64_t skew = 0;
    const bool sap_clock = spec.kind == wm::WmKind::Date || spec.kind == wm::WmKind::Datetime;

    // A wall-clock kind CANNOT fall back to the server's clock. Where the two
    // differ -- and on the reference system they differ by two hours -- the
    // ceiling and therefore the stored watermark jump forward by the offset, and
    // everything committed in that window falls below the next floor and is lost
    // permanently. Refusing is loud and costs one cycle; guessing is silent and
    // costs data.
    if (sap_clock && sap_now.size() < 14)
        throw std::runtime_error(
            "cycle: " + target + " has wm_kind " + wm::KindName(spec.kind) +
            ", whose change column is wall-clock in SAP's timezone, so the cycle needs "
            "SAP's own clock in sap_now. Refusing rather than substituting the server's, "
            "which would move the watermark by the offset between them.");

    if (sap_now.size() >= 14) {
        try {
            const int64_t sap_epoch = wm::ParseNumts(sap_now);
            // How far the server's clock is ahead of SAP's wall clock. For a
            // DATS/TIMS column that difference is what converts a wall-clock
            // value into a real instant.
            skew = read_start_epoch - sap_epoch;
            if (sap_clock) read_start = sap_epoch;
        } catch (...) {
            if (sap_clock)
                throw std::runtime_error(
                    "cycle: " + target + " received an unparseable sap_now ('" + sap_now +
                    "'); refusing rather than substituting the server's clock.");
            // For a UTC-based kind the server's clock is the right ruler anyway.
        }
    }
    out.bounds = wm::ComputeBounds(spec, read_start, out.plan.truncate_target);

    // Allocate the run id NOW, so everything the cycle writes can carry it. It
    // used to exist only as a sequence default on the stats table, consumed when
    // that row was written -- which is after the cycle is over.
    out.run_id = std::stoll(Scalar(con, "SELECT nextval('_erpl_rev_run_seq')"));
    out.stage_table = StageName(target, out.run_id);

    // Retire runs that are no longer alive, BEFORE deciding what is an orphan.
    //
    // The sweep below treats a RUNNING statistics row as proof its stage is in
    // use. Nothing else ever moves a row out of RUNNING -- only a successful
    // Commit does -- so without this every crashed or failed cycle would pin its
    // stage forever: roughly 1800 leaked tables an hour on a 2-second tick with
    // one failing target. A row older than the target's own cycle budget belongs
    // to a run that is not coming back.
    Exec(con,
         "UPDATE _erpl_rev_run_stats SET status='ABANDONED', "
         "error_text=coalesce(error_text,'no completion recorded; run presumed dead') "
         "WHERE target=" + Lit(target) + " AND status='RUNNING' AND ts < now() - INTERVAL '" +
             std::to_string(st.max_cycle_secs > 0 ? st.max_cycle_secs : 3600) + "' SECOND",
         "retire stale runs");

    // Drop what a DEAD run left behind -- and only that.
    //
    // The stage of a run that is still going is not an orphan. A healthy cycle
    // can be mid-INSERT into its stage for a long time (the ingest pipe waits up
    // to an hour), far longer than the advisory lease, so "the lease expired"
    // does not mean "the cycle is gone". Dropping that table out from under it
    // produced an opaque "table does not exist" failure attributed to SAP.
    //
    // A run is in flight if its statistics row is still RUNNING; those rows are
    // opened here and closed by Commit, so they are the authority on which run
    // ids are live.
    for (const auto &orphan : [&] {
             std::vector<std::string> v;
             auto r = con.Query(
                 // starts_with, not LIKE. An underscore is a single-character
                 // wildcard in LIKE and SAP names are full of them, so the
                 // pattern for a target named a_b also matched axb's stages --
                 // and the sweep dropped a live staging table belonging to a
                 // different target, which is the exact failure the paragraph
                 // above says it fixed.
                 "SELECT table_name FROM duckdb_tables() WHERE starts_with(table_name, " +
                 Lit(Lower(sqlname::UniqueToken(target)) + "__stg_") + ")" +
                 " AND table_name NOT IN (SELECT " +
                 Lit(Lower(sqlname::UniqueToken(target)) + "__stg_") +
                 " || CAST(run_id AS VARCHAR) FROM _erpl_rev_run_stats "
                 // Scoped to THIS target. Run ids are global, so without it the
                 // live runs of every other target were also excluded -- which
                 // errs toward leaking an orphan rather than dropping a live
                 // stage, but it made the exclusion mean something other than
                 // what it says, and paired with the LIKE wildcard above it took
                 // both bugs to produce the failure.
                 "WHERE status='RUNNING' AND target=" + Lit(target) + ")");
             if (!r->HasError())
                 for (size_t i = 0; i < r->RowCount(); ++i)
                     v.push_back(r->GetValue(0, i).ToString());
             return v;
         }())
        Exec(con, "DROP TABLE IF EXISTS " + orphan, "drop orphan stage");

    Exec(con,
         // wm_to carries the ceiling computed HERE, from this cycle's read start.
         // The commit must not recompute it: by then the clock has moved, and a
         // later ceiling would advance the watermark past rows this cycle never
         // read. It is the plan for the cycle, decided once.
         "INSERT INTO _erpl_rev_run_stats (run_id, target, source, run_type, method, status, "
         "load_type, wm_from, wm_to, clock_skew_secs) VALUES (" + std::to_string(out.run_id) + "," + Lit(target) +
             "," + Lit(st.source_from) + ",'DELTA'," + Lit(st.method) + ",'RUNNING'," +
             Lit(LoadTypeCode(load_type)) + "," + Lit(st.wm_value) + "," +
             Lit(out.bounds.next_watermark) + "," + std::to_string(skew) + ")",
         "open stats row");

    // Claim by COMPARE-AND-SWAP, not by assignment.
    //
    // An unconditional UPDATE let a manual `sync run`, a second daemon, or a
    // batch tick take a target from a cycle that was perfectly healthy -- and
    // since a cycle can legitimately read for far longer than any lease
    // (the ingest pipe waits up to an hour), "the lease looks old" is not
    // evidence the cycle is gone. The first cycle then did all of its work and
    // was refused at commit.
    //
    // A target is claimable only when nobody owns it, or when the owner's lease
    // has aged past that target's whole cycle budget.
    {
        auto claim = con.Query(
            "UPDATE _erpl_rev_delta_state SET status='RUNNING', lease_ts=now(), active_run_id=" +
            std::to_string(out.run_id) + " WHERE target=" + Lit(target) +
            " AND (active_run_id IS NULL OR lease_ts IS NULL OR lease_ts < now() - INTERVAL '" +
            std::to_string(st.max_cycle_secs > 0 ? st.max_cycle_secs : 3600) + "' SECOND)");
        if (claim->HasError())
            throw std::runtime_error("cycle: claim failed: " + claim->GetError());
        const auto got = claim->RowCount() > 0 && !claim->GetValue(0, 0).IsNull()
                             ? claim->GetValue(0, 0).GetValue<int64_t>()
                             : 0;
        if (got == 0) {
            // Undo what this run already recorded, so a refused Begin leaves no
            // trace to clean up later.
            con.Query("DELETE FROM _erpl_rev_run_stats WHERE run_id=" +
                      std::to_string(out.run_id));
            throw std::runtime_error(
                "cycle: " + target + " is already owned by a running cycle; "
                "not starting a second one");
        }
    }

    return out;
}

CommitResult Commit(duckdb::Connection &con, const std::string &target, long long run_id,
                    const CommitCounts &counts) {
    const State st = LoadState(con, target);
    const std::string stage = StageName(target, run_id);

    // Fencing. Checked before the transaction so the message is clean, and again
    // inside it as part of the UPDATE's WHERE so a concurrent claim cannot slip
    // between the two.
    const auto active = Scalar(con, "SELECT coalesce(active_run_id,-1) FROM _erpl_rev_delta_state "
                                    "WHERE target=" + Lit(target));
    if (active != std::to_string(run_id)) {
        // Record the outcome before refusing. A run that is turned away here is
        // just as dead as one that fails mid-transaction, and leaving its row
        // RUNNING would pin its staging table forever.
        con.Query("UPDATE _erpl_rev_run_stats SET status='ERROR', "
                  "error_text='target reclaimed by run " + active +
                  "' WHERE run_id=" + std::to_string(run_id) + " AND status='RUNNING'");
        // Not a fail_count bump: this run lost a race, the TARGET is not broken,
        // and backing off a healthy target because another cycle beat it to it
        // would be exactly the wrong response.
        throw std::runtime_error(
            "cycle: run " + std::to_string(run_id) + " no longer owns " + target +
            " (active run is " + active + "); refusing to commit a reclaimed cycle");
    }

    CommitResult res;
    // The try opens HERE, not at BEGIN. Real work happens before the transaction
    // -- describing the tables, counting, computing the watermark -- and a
    // failure in any of it used to escape uncaught, leaving the run RUNNING
    // forever (so its stage was never swept) and never counting against the
    // target (so backoff never engaged). Which is exactly how a broken target
    // retried at full cadence indefinitely.
    try {
    const bool have_stage =
        Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name=" + Lit(stage)) == "1";

    const auto keys = SplitCsv(st.keys);
    // IS NOT DISTINCT FROM, not "=". Plain equality is UNKNOWN when either side
    // is NULL, so a NULL-keyed row never matches its own copy in the target: the
    // merge inserts it again and the log records another insert, once per cycle,
    // forever -- because the safety overlap keeps re-reading it. A NULL key is
    // nearly always a modelling mistake, but it must not corrupt the target.
    std::string key_join;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) key_join += " AND ";
        key_join += "t." + keys[i] + " IS NOT DISTINCT FROM s." + keys[i];
    }

    // A first-ever cycle has no target yet: the delta engine is documented as
    // self-creating, and the old direct-merge path created it as a side effect
    // of replicating into it. Staging has to do the same, or a target can only
    // ever be started by a separate full load.
    const bool have_target =
        Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name=" + Lit(Lower(target)))
        == "1";
    // NOTE: the creation itself happens inside the commit transaction below.
    // Doing it here left user-visible rows in a table the watermark said were
    // never replicated, if anything failed in between -- and a retry then
    // re-applied against a target that already held them.

    // The columns to move: target columns the stage also has.
    std::vector<std::string> cols;
    if (have_stage) {
        auto sc = con.Query("SELECT * FROM " + stage + " LIMIT 0");
        if (sc->HasError())
            throw std::runtime_error("cycle: staging describe failed: " + sc->GetError());

        if (!have_target) {
            // First cycle: the target is about to be created FROM the stage, so
            // its columns are the stage's. Describing a table that does not
            // exist yet is not an error condition, it is the normal first run.
            for (duckdb::idx_t c = 0; c < sc->ColumnCount(); ++c)
                cols.push_back(Lower(sc->names[c]));
        } else {
            auto tc = con.Query("SELECT * FROM " + target + " LIMIT 0");
            if (tc->HasError())
                throw std::runtime_error("cycle: target describe failed: " + tc->GetError());
            std::vector<std::string> sset;
            for (duckdb::idx_t c = 0; c < sc->ColumnCount(); ++c)
                sset.push_back(Lower(sc->names[c]));
            for (duckdb::idx_t c = 0; c < tc->ColumnCount(); ++c) {
                const auto n = Lower(tc->names[c]);
                for (const auto &s : sset)
                    if (s == n) { cols.push_back(n); break; }
            }
        }
    }

    std::string collist, sellist, setlist;
    // The same columns read from the TARGET side, for the delete records a
    // reload writes: those rows exist only in the target.
    std::string tsellist;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) { collist += ","; sellist += ","; }
        collist += cols[i];
        sellist += "s." + cols[i];
        if (!tsellist.empty()) tsellist += ",";
        tsellist += "t." + cols[i];
        bool is_key = false;
        for (const auto &k : keys) if (k == cols[i]) is_key = true;
        if (!is_key) {
            if (!setlist.empty()) setlist += ",";
            setlist += cols[i] + " = s." + cols[i];
        }
    }

    // Nothing to merge when the target was just created FROM the stage -- the
    // rows are already there, and merging would be a no-op over itself.
    const bool will_merge = have_stage && have_target && !cols.empty() && !keys.empty();

    // The self-creating first cycle: the target does not exist yet and is about
    // to be created FROM the stage. No merge -- the rows are already there once
    // the CREATE runs -- but it is still a cycle that delivered rows, and a
    // log-enabled target must record them. It used to record nothing, and had no
    // log table at all, so a subscription created before a target's first cycle
    // silently began at the second batch.
    const bool will_create = have_stage && !have_target && !cols.empty();

    // Counts and log rows are derived BEFORE the merge, because both depend on
    // whether the key was already present -- which the merge is about to change.
    // MERGE ... RETURNING merge_action would express this in one statement, but
    // DuckDB 1.5.5 does not allow a MERGE as a subquery or in a CTE. Two
    // statements in one transaction are just as atomic, which is the property
    // that actually matters.
    if (will_merge) {
        auto cr = con.Query(
            "SELECT (SELECT count(*) FROM " + stage + " s WHERE EXISTS (SELECT 1 FROM " + target +
            " t WHERE " + key_join + ")) AS upd, (SELECT count(*) FROM " + stage +
            " s WHERE NOT EXISTS (SELECT 1 FROM " + target + " t WHERE " + key_join +
            ")) AS ins");
        if (cr->HasError()) throw std::runtime_error("cycle: count failed: " + cr->GetError());
        res.upd = cr->GetValue(0, 0).GetValue<int64_t>();
        res.ins = cr->GetValue(1, 0).GetValue<int64_t>();
    } else if (will_create) {
        // Everything a target's first cycle brings is new, by definition.
        res.ins = std::stoll(Scalar(con, "SELECT count(*) FROM " + stage));
        res.upd = 0;
    }

    // The new watermark. For a clock-based kind it is the ceiling computed at
    // begin; for a counter it is cut from the rows actually staged, which is
    // knowable only now.
    std::string new_wm = st.wm_value;
    LoadPlan load_plan;
    {
        wm::WatermarkSpec spec;
        spec.kind = wm::ParseKind(st.wm_kind.empty() ? "NUMTS" : st.wm_kind);
        spec.chg_col = st.chg_col;
        spec.time_col = st.time_col;
        spec.wm_value = st.wm_value;
        spec.safety_secs = st.safety_secs;
        spec.safety_units = st.safety_units;

        // Throw, do not default. The adjacent read of wm_to already refuses a
        // missing stats row -- and this is the read that decides whether the
        // target gets emptied, so it is the last one that should quietly guess.
        // A missing row means the run was not opened by Begin(), and silently
        // demoting F to D there turns a repair into a no-op the operator is
        // told succeeded.
        auto rt = con.Query("SELECT load_type FROM _erpl_rev_run_stats WHERE run_id=" +
                            std::to_string(run_id));
        if (rt->HasError() || rt->RowCount() == 0 || rt->GetValue(0, 0).IsNull())
            throw std::runtime_error("cycle: run " + std::to_string(run_id) +
                                     " has no recorded load type; it was not opened by Begin()");
        const std::string lt = rt->GetValue(0, 0).ToString();
        const auto plan = PlanLoad(ParseLoadType(lt));
        load_plan = plan;

        if (plan.advance_watermark || plan.seed_watermark) {
            if (spec.kind == wm::WmKind::Int) {
                std::string staged_max;
                if (have_stage && !st.chg_col.empty())
                    staged_max = Scalar(con, "SELECT max(" + Lower(st.chg_col) + ") FROM " + stage);
                new_wm = wm::CeilingFromStagedMax(spec, staged_max);
                // Never backwards. The overlap re-reads rows BELOW the stored
                // watermark on purpose, so a quiet cycle can stage only those --
                // and max(staged) - safety_units then lands behind where we
                // already were, widening the replay window a little more every
                // time. Compared as text, at equal width, because that is how
                // the predicate compares it.
                if (!st.wm_value.empty()) {
                    const std::string a = new_wm, bwm = st.wm_value;
                    const bool shorter = a.size() < bwm.size();
                    const bool lower = a.size() == bwm.size() && a < bwm;
                    if (shorter || lower) new_wm = st.wm_value;
                }
            } else {
                // The ceiling was fixed at begin, from this cycle's read start,
                // and stored on the stats row. Recomputing it here would use a
                // later clock and advance the watermark past rows the cycle
                // never read -- which is the exact bug the ceiling exists to
                // prevent.
                const auto planned = Scalar(con, "SELECT coalesce(wm_to,'') "
                                                 "FROM _erpl_rev_run_stats WHERE run_id=" +
                                                     std::to_string(run_id));
                if (planned.empty())
                    throw std::runtime_error(
                        "cycle: run " + std::to_string(run_id) +
                        " has no planned ceiling; it was not opened by Begin()");
                new_wm = planned;
                // Forward only. A clock that goes backwards -- an NTP
                // correction, or a DST fall-back on a wall-clock target --
                // would otherwise rewind the watermark and re-read a window
                // already applied. Idempotent, so harmless once; but each quiet
                // cycle would walk it back further.
                if (!st.wm_value.empty() && new_wm.size() == st.wm_value.size() &&
                    new_wm < st.wm_value)
                    new_wm = st.wm_value;
            }
        }
    }
    res.new_watermark = new_wm;

    // Provisioning the log and appending to it are separate decisions. The log
    // used to be created only by a cycle that had rows to append, so a
    // log-enabled target that had been quiet since registration had no log
    // table at all -- and a subscriber, or retention, met "table does not
    // exist" rather than an empty log. An empty log is a fact every reader can
    // handle; a missing one is an error every reader has to special-case.
    // The log is created from the TARGET's shape, so it needs a target and
    // nothing else -- in particular not a stage, which a quiet cycle has none
    // of. (The column reconcile below iterates the stage's columns and is
    // simply a no-op when there are none.)
    const bool want_log = st.log_enabled && (have_target || will_create);
    const bool append_log = want_log && (will_merge || will_create);
    const std::string log = ChangeLogName(target);

    // Staged data must be unique on the key. The stage used to carry a PRIMARY
    // KEY, which asserted this as a side effect; dropping that index for speed
    // dropped the assertion with it. Without it duplicates flow into the merge
    // and a key can multiply in the target -- the target's own PRIMARY KEY is
    // built best-effort and silently skipped when it cannot be. One grouped
    // scan of the stage, which is the small side.
    if (have_stage && !keys.empty()) {
        std::string kl;
        for (size_t i = 0; i < keys.size(); ++i) { if (i) kl += ","; kl += keys[i]; }
        auto d = con.Query("SELECT " + kl + ", count(*) AS n FROM " + stage + " GROUP BY " + kl +
                           " HAVING count(*) > 1 ORDER BY n DESC LIMIT 1");
        if (!d->HasError() && d->RowCount() > 0) {
            std::string key_text;
            for (duckdb::idx_t c = 0; c + 1 < d->ColumnCount(); ++c) {
                if (c) key_text += ",";
                key_text += d->GetValue(c, 0).ToString();
            }
            con.Query("UPDATE _erpl_rev_run_stats SET status='ERROR', error_text="
                      "'duplicate key in staged data' WHERE run_id=" + std::to_string(run_id));
            con.Query("UPDATE _erpl_rev_delta_state SET status='IDLE', active_run_id=NULL "
                      "WHERE target=" + Lit(target) + " AND active_run_id=" +
                      std::to_string(run_id));
            throw std::runtime_error(
                "cycle: the staged data for " + target + " has a duplicate key (" + key_text +
                " appears " + d->GetValue(d->ColumnCount() - 1, 0).ToString() +
                " times); the registered keys do not identify a row at the source");
        }
    }

    // A reload that staged nothing, against a target that is not empty, is
    // refused. See the migration for v7: the reader cannot distinguish "the
    // source is empty" from "a filter matched nothing", and only one of those
    // two readings is recoverable. Before BEGIN, so the message is clean.
    if (load_plan.truncate_target && have_target && !st.allow_empty_reload) {
        const auto staged = have_stage ? Scalar(con, "SELECT count(*) FROM " + stage) : "0";
        const auto held = Scalar(con, "SELECT count(*) FROM " + target);
        if (staged == "0" && held != "0") {
            con.Query("UPDATE _erpl_rev_run_stats SET status='ERROR', error_text="
                      "'reload staged no rows against a non-empty target' WHERE run_id=" +
                      std::to_string(run_id));
            con.Query("UPDATE _erpl_rev_delta_state SET status='IDLE', active_run_id=NULL "
                      "WHERE target=" + Lit(target) + " AND active_run_id=" +
                      std::to_string(run_id));
            throw std::runtime_error(
                "cycle: the reload of " + target + " staged no rows while the target holds " +
                held + "; refusing to empty it. If the source really is empty, set "
                "allow_empty_reload on this target and run it again.");
        }
    }

    Exec(con, "BEGIN", "begin");
    {
        if (have_stage && !have_target) {
            Exec(con, "CREATE TABLE " + target + " AS SELECT * FROM " + stage, "create target");
            if (!keys.empty()) {
                std::string kl;
                for (size_t i = 0; i < keys.size(); ++i) { if (i) kl += ","; kl += keys[i]; }
                // Best-effort: a source whose "keys" are not unique in the staged
                // slice should still replicate rather than fail the cycle.
                con.Query("ALTER TABLE " + target + " ADD PRIMARY KEY (" + kl + ")");
            }
        }
        if (want_log) EnsureChangeLog(con, target, cols);

        // A reload REPLACES the target. Upserting without this left every row
        // the source had since deleted -- the drift an operator runs F to
        // repair -- so the repair reported success and fixed nothing. Inside
        // the transaction, so a failure leaves the old contents intact rather
        // than an emptied target.
        //
        // After the log is provisioned, because the deletions have to be
        // recorded BEFORE the rows are gone.
        if (load_plan.truncate_target && have_target && have_stage) {
            const std::string gone_where = " t WHERE NOT EXISTS (SELECT 1 FROM " + stage +
                                           " s WHERE " + key_join + ")";
            // What the reload actually removes: target keys the stage does not
            // carry. The old arithmetic was `had - staged`, which is right only
            // when the stage is a subset of the target -- two overlapping key
            // sets of the same size reported zero deletions while rows really
            // disappeared.
            res.del = std::stoll(Scalar(con, "SELECT count(*) FROM " + target + gone_where));

            if (append_log && res.del > 0) {
                // Without this, a caught-up subscriber keeps every row the
                // source dropped: the target is repaired and the sink is not,
                // permanently and invisibly, which falsifies the one invariant
                // a change log exists to hold up -- that replaying it
                // reproduces the target.
                //
                // _commit_ts is NULL: a deletion inferred by comparing two
                // images has no source timestamp, and inventing one would put
                // fiction into the latency percentiles.
                auto d = con.Query("INSERT INTO " + log + " (" + collist +
                                   ", _seq, _op, _run_id, _commit_ts, _applied_at) SELECT " +
                                   tsellist + ", nextval('" + log + "_seq'), 'D', " +
                                   std::to_string(run_id) + ", NULL, now() FROM " + target +
                                   gone_where);
                if (d->HasError())
                    throw std::runtime_error("cycle: reload delete-log append failed: " +
                                             d->GetError());
                res.logged += res.del;
            }
            Exec(con, "DELETE FROM " + target, "truncate target for reload");
        }

        if (append_log) {
            // Derived before the merge: 'U' if the key is already in the target,
            // 'I' otherwise. That is the engine's verdict about what it is about
            // to do, not the source's claim about what happened.
            // LEFT JOIN, not a correlated EXISTS. Both express "was this key
            // already in the target", but DuckDB plans the subquery as a full
            // scan of the target while the join uses its index: measured at 30ms
            // vs 3ms for a TEN-row batch against a 1M-row target, because the
            // cost of the scan tracks the TARGET size, not the change set. On a
            // streaming tick that is the difference between the log costing more
            // than the cycle and costing almost nothing.
            std::string left_on;
            for (size_t i = 0; i < keys.size(); ++i) {
                if (i) left_on += " AND ";
                left_on += "t." + keys[i] + " IS NOT DISTINCT FROM s." + keys[i];
            }
            // The source's change time, parsed from the change column. SAP writes
            // timestamps as YYYYMMDDHHMMSS(.fffffff), which is not a format any
            // database parses by accident. NULL when the method has no such
            // value -- a snapshot row genuinely has no source timestamp, and
            // inventing one would make every latency percentile a fiction.
            // Timezone is not a detail here: it is the difference between a
            // latency figure and a two-hour lie.
            //
            // NUMTS/TIMESTAMPL come from ABAP's GET TIME STAMP, which is UTC by
            // definition, so they are read AT TIME ZONE 'UTC'. DATS and TIMS are
            // local wall-clock and are read as local. Getting this wrong is not
            // subtle once you look -- every percentile shifts by exactly the
            // offset -- but it is invisible if you only ever check that a number
            // came out.
            const auto skew_s = Scalar(con, "SELECT coalesce(clock_skew_secs,0) "
                                            "FROM _erpl_rev_run_stats WHERE run_id=" +
                                                std::to_string(run_id));
            const long long skew = skew_s.empty() ? 0 : std::stoll(skew_s);
            const std::string skew_add =
                skew == 0 ? "" : " + INTERVAL '" + std::to_string(skew) + "' SECOND";

            std::string commit_ts = "NULL";
            const auto col = Lower(st.chg_col);
            if (!st.chg_col.empty() &&
                (st.wm_kind == "NUMTS" || st.wm_kind == "TIMESTAMPL" || st.wm_kind.empty())) {
                commit_ts = "(try_strptime(substr(CAST(s." + col + " AS VARCHAR), 1, 14), "
                            "'%Y%m%d%H%M%S') AT TIME ZONE 'UTC')";
            } else if (!st.chg_col.empty() && st.wm_kind == "DATETIME" && !st.time_col.empty()) {
                // The pair, composed back into one value. Reading only the date
                // half would parse to midnight and report every change as up to
                // a day stale.
                //
                // strftime, not CAST: the staging table holds these as DuckDB
                // DATE and TIME, whose text form is '2026-09-05' and '20:31:39'.
                // Concatenating those gives something no SAP format string
                // parses, and the failure is silent -- try_strptime returns NULL
                // and the latency simply has no samples, which reads as "the
                // feature is off" rather than "the parse is wrong".
                // strftime handles the DATE half; it does NOT accept a bare
                // TIME, so the time half is formatted by stripping the colons
                // out of its text form. Both halves land as SAP writes them --
                // YYYYMMDD and HHMMSS -- which is what the parse expects.
                // ...plus the recorded SAP-to-server offset, which is what turns
                // a wall-clock value into an instant comparable with now().
                // AT TIME ZONE 'UTC', then the recorded offset. A parsed
                // wall-clock value is a naive TIMESTAMP, and letting it cast into
                // a TIMESTAMPTZ column picks up the SERVER's session zone -- which
                // is how a two-hour error appears in a latency figure without
                // anything looking wrong. Anchoring to UTC and adding the measured
                // SAP-to-server offset makes it independent of where either
                // machine happens to be.
                commit_ts = "((try_strptime(strftime(s." + col + ", '%Y%m%d') || "
                            "replace(substr(CAST(s." + Lower(st.time_col) +
                            " AS VARCHAR), 1, 8), ':', ''), '%Y%m%d%H%M%S') "
                            "AT TIME ZONE 'UTC')" + skew_add + ")";
            } else if (!st.chg_col.empty() && st.wm_kind == "DATE") {
                commit_ts = "((try_strptime(strftime(s." + col + ", '%Y%m%d'), '%Y%m%d') "
                            "AT TIME ZONE 'UTC')" + skew_add + ")";
            }
            // A counter watermark has no clock at all, so _commit_ts stays NULL
            // and latency is genuinely unmeasurable for it. Reported as "no
            // samples" rather than as zero.

            // On the self-creating first cycle every row is an insert, and the
            // join must not be asked: the target was created FROM the stage a
            // few statements ago, so it already holds every staged row and the
            // join would call all of them updates.
            //
            // A literal marker from the joined side, not a key column. With
            // null-safe matching a genuinely-NULL key can MATCH, and testing
            // that key for NULL would then report an update as an insert.
            // __erpl_matched is 1 exactly when the join found a row.
            const std::string op_expr =
                will_create ? "'I'" : "CASE WHEN t.__erpl_matched IS NULL THEN 'I' ELSE 'U' END";
            const std::string from_expr =
                will_create ? (" FROM " + stage + " s")
                            : (" FROM " + stage + " s LEFT JOIN (SELECT *, 1 AS __erpl_matched "
                                                  "FROM " + target + ") t ON " + left_on);
            auto r = con.Query("INSERT INTO " + log + " (" + collist +
                               ", _seq, _op, _run_id, _commit_ts, _applied_at) SELECT " + sellist +
                               ", nextval('" + log + "_seq'), " + op_expr + ", " +
                               std::to_string(run_id) + ", " + commit_ts + ", now()" + from_expr);
            if (r->HasError())
                throw std::runtime_error("cycle: change-log append failed: " + r->GetError());
            res.logged += res.ins + res.upd;
        }

        if (will_merge) {
            std::string m = "MERGE INTO " + target + " AS t USING " + stage + " AS s ON " +
                            key_join;
            if (!setlist.empty()) m += " WHEN MATCHED THEN UPDATE SET " + setlist;
            m += " WHEN NOT MATCHED THEN INSERT (" + collist + ") VALUES (" + sellist + ")";
            Exec(con, m, "merge stage");
        }

        // The fence. Inside the transaction, so losing the target here rolls
        // back the merge and the log with it.
        AdvanceWatermarkFenced(con, target, run_id, new_wm, res.ins + res.upd);

        // A truncating load type is one-shot by meaning -- L is "init, then
        // delta", F is "repair this once" -- but load_type_default is what the
        // tick planner hands the daemon, so a target left at either truncated
        // and reloaded on every due tick, unattended. Spent on success, in the
        // same transaction, so a failed reload stays scheduled.
        if (load_plan.truncate_target)
            Exec(con, "UPDATE _erpl_rev_delta_state SET load_type_default='D' WHERE target=" +
                          Lit(target) + " AND load_type_default IN ('F','L')",
                 "spend the one-shot load type");

        Exec(con,
             "UPDATE _erpl_rev_run_stats SET status='SUCCESS', rows_read=" +
                 std::to_string(counts.rows_read) + ", rows_ins=" + std::to_string(res.ins) +
                 ", rows_upd=" + std::to_string(res.upd) + ", rows_del=" + std::to_string(res.del) +
                 ", wm_to=" + Lit(new_wm) + ", duration_ms=0 WHERE run_id=" +
                 std::to_string(run_id),
             "finish stats row");

        if (have_stage) Exec(con, "DROP TABLE " + stage, "drop stage");
        Exec(con, "COMMIT", "commit");
        }
    } catch (...) {
        // Harmless when no transaction was open -- the failure may have happened
        // before BEGIN.
        con.Query("ROLLBACK");
        // Outside the rolled-back transaction on purpose: the run DID happen and
        // DID fail, and that has to survive the rollback. Otherwise the row stays
        // RUNNING forever and its staging table is never swept.
        con.Query("UPDATE _erpl_rev_run_stats SET status='ERROR' WHERE run_id=" +
                  std::to_string(run_id) + " AND status='RUNNING'");
        // This one IS the target failing, so it counts: the planner's backoff
        // reads fail_count, and without the increment a target that fails every
        // cycle retries at full cadence forever.
        con.Query("UPDATE _erpl_rev_delta_state SET fail_count = coalesce(fail_count,0) + 1, "
                  "status='ERROR', active_run_id=NULL WHERE target=" + Lit(target));
        throw;
    }
    return res;
}

}  // namespace cycle
}  // namespace erpl_rev
