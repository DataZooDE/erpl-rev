// The cycle contract: BEGIN_CYCLE / CYCLE_COMMIT.
//
// One round trip opens a cycle -- allocating the run id, computing the read
// bounds, claiming the target and naming a staging table. One transaction closes
// it: merge the stage into the target, append the change log, advance the
// watermark, finish the stats row, drop the stage.
//
// The load-bearing invariant is that wm_value advances ONLY inside the commit,
// after the merge. That is what makes a crashed cycle replayable from an unmoved
// watermark, and its orphaned stage free to discard.

#include <catch2/catch_test_macros.hpp>
#include <duckdb.hpp>

#include <string>

#include "control_schema.hpp"
#include "cycle.hpp"

using namespace erpl_rev;

namespace {
constexpr int64_t kReadStart = 1788609600;   // 2026-09-05 12:00:00 UTC

std::string Scalar(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    REQUIRE_FALSE(r->HasError());
    if (r->RowCount() == 0 || r->GetValue(0, 0).IsNull()) return "";
    return r->GetValue(0, 0).ToString();
}

void Exec(duckdb::Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    INFO(sql);
    REQUIRE_FALSE(r->HasError());
}

// A registered watermark target with three rows already replicated.
void Setup(duckdb::Connection &con, bool log_enabled = false) {
    schema::Migrate(con, "test");
    Exec(con, "CREATE TABLE zdelta_wm(id INTEGER PRIMARY KEY, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO zdelta_wm VALUES (1,'a','20260905090000'),(2,'b','20260905090000')");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_secs, "
              " cadence, status, log_enabled) VALUES "
              "('zdelta_wm','WATERMARK','ZDELTA_WM','id','CHANGED_AT','NUMTS',"
              "'20260905100000',120,'micro:5','IDLE'," +
              std::string(log_enabled ? "true" : "false") + ")");
}

// What a reader would have staged: two changed rows.
void StageRows(duckdb::Connection &con, const std::string &stage) {
    Exec(con, "CREATE TABLE " + stage + "(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO " + stage + " VALUES (2,'b2','20260905110000'),(3,'c','20260905110500')");
}
}  // namespace

TEST_CASE("cycle: begin allocates a run id and claims the target", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);

    CHECK(b.run_id > 0);
    // The run id exists NOW, during the cycle -- it used to be a sequence default
    // consumed only when the stats row was written, i.e. after the cycle ended,
    // so nothing during the run could be stamped with it.
    CHECK(Scalar(con, "SELECT active_run_id FROM _erpl_rev_delta_state") ==
          std::to_string(b.run_id));
    CHECK(Scalar(con, "SELECT status FROM _erpl_rev_delta_state") == "RUNNING");
    // An open stats row, so a crashed cycle is visible rather than absent.
    CHECK(Scalar(con, "SELECT status FROM _erpl_rev_run_stats WHERE run_id=" +
                          std::to_string(b.run_id)) == "RUNNING");
    // The stage is named for the run, so an orphan identifies itself.
    CHECK(b.stage_table.find(std::to_string(b.run_id)) != std::string::npos);
}

TEST_CASE("cycle: begin returns bounds, not SQL text", "[cycle]") {
    // The server sends values; ABAP composes the predicate. Shipping SQL across
    // the boundary would invert the driver's value-only posture and freeze one
    // rendering into the tests.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    CHECK(b.bounds.floor == "20260905095800");    // stored wm minus 120s
    CHECK(b.bounds.ceiling == "20260905115800");  // read start minus 120s
    CHECK(b.chg_col == "CHANGED_AT");
}

TEST_CASE("cycle: commit merges, advances to the ceiling and drops the stage", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, b.stage_table);
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {2});

    CHECK(r.upd == 1);
    CHECK(r.ins == 1);
    CHECK(Scalar(con, "SELECT v FROM zdelta_wm WHERE id=2") == "b2");
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "3");
    // The watermark is the CEILING, never the maximum of what arrived.
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "20260905115800");
    CHECK(Scalar(con, "SELECT status FROM _erpl_rev_delta_state") == "IDLE");
    CHECK(Scalar(con, "SELECT active_run_id FROM _erpl_rev_delta_state") == "");
    CHECK(Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name='" +
                          b.stage_table + "'") == "0");
    CHECK(Scalar(con, "SELECT status FROM _erpl_rev_run_stats WHERE run_id=" +
                          std::to_string(b.run_id)) == "SUCCESS");
}

TEST_CASE("cycle: a stale run id is refused and changes nothing", "[cycle]") {
    // The fencing token. The lease is advisory and can expire under a healthy but
    // slow cycle; this cannot. A late commit from a cycle whose target was
    // reclaimed must not merge its stage or move the watermark.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto first = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, first.stage_table);
    // A second cycle reclaims the target (as the planner would after a stale lease).
    const auto second = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart + 60);

    REQUIRE_THROWS(cycle::Commit(con, "zdelta_wm", first.run_id, {2}));
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "20260905100000");
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");
    CHECK(Scalar(con, "SELECT active_run_id FROM _erpl_rev_delta_state") ==
          std::to_string(second.run_id));
}

TEST_CASE("cycle: a failed commit leaves the watermark and the stage untouched", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    // A stage whose column cannot be merged into the target.
    Exec(con, "CREATE TABLE " + b.stage_table + "(id VARCHAR, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES ('not-an-int','x','20260905110000')");

    REQUIRE_THROWS(cycle::Commit(con, "zdelta_wm", b.run_id, {1}));
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "20260905100000");
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");
    // The stage survives, so the failure is diagnosable and the cycle replayable.
    CHECK(Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name='" +
                          b.stage_table + "'") == "1");
}

TEST_CASE("cycle: begin discards a previous run's orphaned stage", "[cycle]") {
    // A cycle that died after staging leaves exactly one identifiable orphan.
    // It is free to discard because the watermark never moved.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto dead = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, dead.stage_table);   // ... and then nothing commits it

    const auto next = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart + 60);
    CHECK(next.run_id != dead.run_id);
    CHECK(Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name='" +
                          dead.stage_table + "'") == "0");
}

TEST_CASE("cycle: the change log records one row per applied change", "[cycle][log]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con, /*log_enabled=*/true);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, b.stage_table);
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {2});

    const std::string log = cycle::ChangeLogName("zdelta_wm");
    CHECK(r.logged == 2);
    CHECK(Scalar(con, "SELECT count(*) FROM " + log) == "2");
    // The op is the engine's verdict, not the source's claim: id=2 existed, id=3
    // did not.
    CHECK(Scalar(con, "SELECT _op FROM " + log + " WHERE id=2") == "U");
    CHECK(Scalar(con, "SELECT _op FROM " + log + " WHERE id=3") == "I");
    CHECK(Scalar(con, "SELECT _run_id FROM " + log + " WHERE id=3") == std::to_string(b.run_id));
    // Monotonic within the transaction that wrote them.
    CHECK(Scalar(con, "SELECT count(DISTINCT _seq) FROM " + log) == "2");
}

TEST_CASE("cycle: no log rows when the apply fails", "[cycle][log]") {
    // The append lives in the same transaction as the merge, so a log that
    // records changes the target never received is not possible.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con, /*log_enabled=*/true);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id VARCHAR, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES ('nope','x','20260905110000')");

    REQUIRE_THROWS(cycle::Commit(con, "zdelta_wm", b.run_id, {1}));
    const std::string log = cycle::ChangeLogName("zdelta_wm");
    const auto exists = Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name='" +
                                        log + "'");
    if (exists == "1") CHECK(Scalar(con, "SELECT count(*) FROM " + log) == "0");
}

TEST_CASE("cycle: logging off writes no log at all", "[cycle][log]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con, /*log_enabled=*/false);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, b.stage_table);
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {2});

    CHECK(r.logged == 0);
    CHECK(Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name='" +
                          cycle::ChangeLogName("zdelta_wm") + "'") == "0");
}

TEST_CASE("cycle: replaying the log reproduces the mirror", "[cycle][log]") {
    // Invariant: the log is not a side note, it is the same information.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con, /*log_enabled=*/true);
    // Start from an empty target so the log is the complete history.
    Exec(con, "DELETE FROM zdelta_wm");

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, b.stage_table);
    cycle::Commit(con, "zdelta_wm", b.run_id, {2});

    const std::string log = cycle::ChangeLogName("zdelta_wm");
    Exec(con, "CREATE TABLE replay AS SELECT * FROM zdelta_wm LIMIT 0");
    Exec(con, "INSERT INTO replay SELECT id, v, changed_at FROM " + log +
              " WHERE _op IN ('I','U') QUALIFY row_number() OVER "
              "(PARTITION BY id ORDER BY _seq DESC)=1");

    CHECK(Scalar(con, "SELECT count(*) FROM replay") ==
          Scalar(con, "SELECT count(*) FROM zdelta_wm"));
    CHECK(Scalar(con, "SELECT count(*) FROM ("
                      "SELECT * FROM replay EXCEPT SELECT * FROM zdelta_wm)") == "0");
}

TEST_CASE("cycle: a counter watermark takes its ceiling from the staged rows", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    Exec(con, "CREATE TABLE docs(id INTEGER PRIMARY KEY, docnr BIGINT)");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_units, "
              " cadence, status) VALUES "
              "('docs','WATERMARK','DOCS','id','DOCNR','INT','1000',50,'micro:5','IDLE')");

    const auto b = cycle::Begin(con, "docs", LoadType::Delta, kReadStart);
    CHECK(b.bounds.floor == "950");
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, docnr BIGINT)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES (1,19000),(2,20000)");
    cycle::Commit(con, "docs", b.run_id, {2});

    // max(20000) - 50 units of overlap.
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "19950");
}

TEST_CASE("cycle: an init-without-data run seeds and transfers nothing", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::InitOnly, kReadStart);
    CHECK_FALSE(b.plan.read_rows);
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {0});

    CHECK(r.ins == 0);
    CHECK(r.upd == 0);
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");   // untouched
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "20260905115800");
}

TEST_CASE("cycle: a repair run does not move the watermark", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Full, kReadStart);
    StageRows(con, b.stage_table);
    cycle::Commit(con, "zdelta_wm", b.run_id, {2});

    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "3");        // data repaired
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "20260905100000");
}

TEST_CASE("cycle: the stage name is collision-safe across target names", "[cycle]") {
    // Stage and log names are derived from a customer-chosen target name. Two
    // targets sharing one stage would have them merging into each other's data.
    CHECK(cycle::StageName("MY-TAB", 1) != cycle::StageName("MY_TAB", 1));
    CHECK(cycle::ChangeLogName("MY-TAB") != cycle::ChangeLogName("MY_TAB"));
    // ...but a run id still separates two runs of the same target.
    CHECK(cycle::StageName("T", 1) != cycle::StageName("T", 2));
}

TEST_CASE("cycle: a first cycle creates the target it merges into", "[cycle]") {
    // The delta engine is documented as self-creating: registering a target and
    // running it is supposed to be enough. The old direct-merge path created the
    // table as a side effect of replicating into it, so staging has to do the
    // same -- otherwise a target can only ever be started by a separate full
    // load, which is not what the docs promise.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_secs, "
              " cadence, status) VALUES "
              "('fresh','WATERMARK','ZDELTA_WM','id','CHANGED_AT','NUMTS','',120,'manual','IDLE')");

    const auto b = cycle::Begin(con, "fresh", LoadType::Delta, kReadStart);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, v VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES (1,'a'),(2,'b')");
    cycle::Commit(con, "fresh", b.run_id, {2});

    CHECK(Scalar(con, "SELECT count(*) FROM fresh") == "2");
    CHECK(Scalar(con, "SELECT v FROM fresh WHERE id=2") == "b");
    // ...and the second cycle merges into it rather than recreating it.
    const auto b2 = cycle::Begin(con, "fresh", LoadType::Delta, kReadStart + 60);
    Exec(con, "CREATE TABLE " + b2.stage_table + "(id INTEGER, v VARCHAR)");
    Exec(con, "INSERT INTO " + b2.stage_table + " VALUES (2,'b2'),(3,'c')");
    cycle::Commit(con, "fresh", b2.run_id, {2});
    CHECK(Scalar(con, "SELECT count(*) FROM fresh") == "3");
    CHECK(Scalar(con, "SELECT v FROM fresh WHERE id=2") == "b2");
}
