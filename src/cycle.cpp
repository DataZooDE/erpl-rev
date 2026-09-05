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
    bool log_enabled = false;
};

State LoadState(duckdb::Connection &con, const std::string &target) {
    auto r = con.Query(
        "SELECT method, source_from, keys, coalesce(chg_col,''), coalesce(wm_kind,'NUMTS'), "
        "coalesce(wm_value,''), coalesce(safety_secs,120), coalesce(time_col,''), "
        "coalesce(safety_units,0), coalesce(log_enabled,false) "
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
    return s;
}

}  // namespace

std::string ChangeLogName(const std::string &target) {
    return "_erpl_rev_log_" + Lower(sqlname::UniqueToken(target));
}

std::string StageName(const std::string &target, long long run_id) {
    return Lower(sqlname::UniqueToken(target)) + "__stg_" + std::to_string(run_id);
}

BeginResult Begin(duckdb::Connection &con, const std::string &target, LoadType load_type,
                  int64_t read_start_epoch) {
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
    out.bounds = wm::ComputeBounds(spec, read_start_epoch);

    // Allocate the run id NOW, so everything the cycle writes can carry it. It
    // used to exist only as a sequence default on the stats table, consumed when
    // that row was written -- which is after the cycle is over.
    out.run_id = std::stoll(Scalar(con, "SELECT nextval('_erpl_rev_run_seq')"));
    out.stage_table = StageName(target, out.run_id);

    // Whatever a previous run left behind is safe to drop: its watermark never
    // moved, so its read is simply repeated.
    for (const auto &orphan : [&] {
             std::vector<std::string> v;
             auto r = con.Query("SELECT table_name FROM duckdb_tables() WHERE table_name LIKE " +
                                Lit(Lower(sqlname::UniqueToken(target)) + "__stg_%"));
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
         "load_type, wm_from, wm_to) VALUES (" + std::to_string(out.run_id) + "," + Lit(target) +
             "," + Lit(st.source_from) + ",'DELTA'," + Lit(st.method) + ",'RUNNING'," +
             Lit(LoadTypeCode(load_type)) + "," + Lit(st.wm_value) + "," +
             Lit(out.bounds.next_watermark) + ")",
         "open stats row");

    Exec(con,
         "UPDATE _erpl_rev_delta_state SET status='RUNNING', lease_ts=now(), active_run_id=" +
             std::to_string(out.run_id) + " WHERE target=" + Lit(target),
         "claim target");

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
    if (active != std::to_string(run_id))
        throw std::runtime_error(
            "cycle: run " + std::to_string(run_id) + " no longer owns " + target +
            " (active run is " + active + "); refusing to commit a reclaimed cycle");

    CommitResult res;
    const bool have_stage =
        Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name=" + Lit(stage)) == "1";

    const auto keys = SplitCsv(st.keys);
    std::string key_join;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) key_join += " AND ";
        key_join += "t." + keys[i] + " = s." + keys[i];
    }

    // The columns to move: target columns the stage also has.
    std::vector<std::string> cols;
    if (have_stage) {
        auto tc = con.Query("SELECT * FROM " + target + " LIMIT 0");
        auto sc = con.Query("SELECT * FROM " + stage + " LIMIT 0");
        if (tc->HasError() || sc->HasError())
            throw std::runtime_error("cycle: describe failed");
        std::vector<std::string> sset;
        for (duckdb::idx_t c = 0; c < sc->ColumnCount(); ++c) sset.push_back(Lower(sc->names[c]));
        for (duckdb::idx_t c = 0; c < tc->ColumnCount(); ++c) {
            const auto n = Lower(tc->names[c]);
            for (const auto &s : sset)
                if (s == n) { cols.push_back(n); break; }
        }
    }

    std::string collist, sellist, setlist;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) { collist += ","; sellist += ","; }
        collist += cols[i];
        sellist += "s." + cols[i];
        bool is_key = false;
        for (const auto &k : keys) if (k == cols[i]) is_key = true;
        if (!is_key) {
            if (!setlist.empty()) setlist += ",";
            setlist += cols[i] + " = s." + cols[i];
        }
    }

    const bool will_merge = have_stage && !cols.empty() && counts.rows_read >= 0 &&
                            !keys.empty();

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
    }

    // The new watermark. For a clock-based kind it is the ceiling computed at
    // begin; for a counter it is cut from the rows actually staged, which is
    // knowable only now.
    std::string new_wm = st.wm_value;
    {
        wm::WatermarkSpec spec;
        spec.kind = wm::ParseKind(st.wm_kind.empty() ? "NUMTS" : st.wm_kind);
        spec.chg_col = st.chg_col;
        spec.time_col = st.time_col;
        spec.wm_value = st.wm_value;
        spec.safety_secs = st.safety_secs;
        spec.safety_units = st.safety_units;

        auto rt = con.Query("SELECT coalesce(load_type,'D') FROM _erpl_rev_run_stats WHERE run_id=" +
                            std::to_string(run_id));
        const std::string lt = rt->HasError() || rt->RowCount() == 0
                                   ? "D"
                                   : rt->GetValue(0, 0).ToString();
        const auto plan = PlanLoad(ParseLoadType(lt));

        if (plan.advance_watermark || plan.seed_watermark) {
            if (spec.kind == wm::WmKind::Int) {
                std::string staged_max;
                if (have_stage && !st.chg_col.empty())
                    staged_max = Scalar(con, "SELECT max(" + Lower(st.chg_col) + ") FROM " + stage);
                new_wm = wm::CeilingFromStagedMax(spec, staged_max);
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
            }
        }
    }
    res.new_watermark = new_wm;

    const bool want_log = st.log_enabled && will_merge;
    const std::string log = ChangeLogName(target);

    Exec(con, "BEGIN", "begin");
    try {
        if (want_log) {
            Exec(con, "CREATE TABLE IF NOT EXISTS " + log + " AS SELECT * FROM " + target +
                          " LIMIT 0", "create change log");
            for (const auto &c : {std::string("_seq BIGINT"), std::string("_op VARCHAR"),
                                  std::string("_run_id BIGINT"),
                                  std::string("_changed_at TIMESTAMPTZ"),
                                  std::string("_commit_ts TIMESTAMPTZ")}) {
                const auto col = c.substr(0, c.find(' '));
                const auto have = Scalar(con, "SELECT count(*) FROM duckdb_columns() "
                                              "WHERE table_name=" + Lit(log) +
                                                  " AND column_name=" + Lit(col));
                if (have == "0") Exec(con, "ALTER TABLE " + log + " ADD COLUMN " + c, "log column");
            }
            Exec(con, "CREATE SEQUENCE IF NOT EXISTS " + log + "_seq START 1", "log sequence");

            // Derived before the merge: 'U' if the key is already in the target,
            // 'I' otherwise. That is the engine's verdict about what it is about
            // to do, not the source's claim about what happened.
            auto r = con.Query(
                "INSERT INTO " + log + " (" + collist +
                ", _seq, _op, _run_id, _changed_at, _commit_ts) SELECT " + sellist +
                ", nextval('" + log + "_seq'), CASE WHEN EXISTS (SELECT 1 FROM " + target +
                " t WHERE " + key_join + ") THEN 'U' ELSE 'I' END, " + std::to_string(run_id) +
                ", now(), now() FROM " + stage + " s");
            if (r->HasError())
                throw std::runtime_error("cycle: change-log append failed: " + r->GetError());
            res.logged = res.ins + res.upd;
        }

        if (will_merge) {
            std::string m = "MERGE INTO " + target + " AS t USING " + stage + " AS s ON " +
                            key_join;
            if (!setlist.empty()) m += " WHEN MATCHED THEN UPDATE SET " + setlist;
            m += " WHEN NOT MATCHED THEN INSERT (" + collist + ") VALUES (" + sellist + ")";
            Exec(con, m, "merge stage");
        }

        Exec(con,
             "UPDATE _erpl_rev_delta_state SET wm_value=" + Lit(new_wm) +
                 ", status='IDLE', last_run_ts=now(), active_run_id=NULL, fail_count=0, "
                 "rows_applied=" + std::to_string(res.ins + res.upd) +
                 " WHERE target=" + Lit(target) + " AND active_run_id=" + std::to_string(run_id),
             "advance watermark");

        Exec(con,
             "UPDATE _erpl_rev_run_stats SET status='SUCCESS', rows_read=" +
                 std::to_string(counts.rows_read) + ", rows_ins=" + std::to_string(res.ins) +
                 ", rows_upd=" + std::to_string(res.upd) + ", rows_del=" + std::to_string(res.del) +
                 ", wm_to=" + Lit(new_wm) + ", duration_ms=0 WHERE run_id=" +
                 std::to_string(run_id),
             "finish stats row");

        if (have_stage) Exec(con, "DROP TABLE " + stage, "drop stage");
        Exec(con, "COMMIT", "commit");
    } catch (...) {
        con.Query("ROLLBACK");
        throw;
    }
    return res;
}

}  // namespace cycle
}  // namespace erpl_rev
