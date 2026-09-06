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
#include <catch2/matchers/catch_matchers_string.hpp>
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
    // The first cycle's lease ages out -- it really is gone -- so a second cycle
    // may reclaim the target. That is the only circumstance in which it may.
    Exec(con, "UPDATE _erpl_rev_delta_state SET lease_ts = now() - INTERVAL '2' HOUR");
    const auto second = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart + 60);

    REQUIRE_THROWS(cycle::Commit(con, "zdelta_wm", first.run_id, {2}));
    // A failed commit records the failure, so the run stops being "in flight"
    // and its stage becomes sweepable.
    CHECK(Scalar(con, "SELECT status FROM _erpl_rev_run_stats WHERE run_id=" +
                          std::to_string(first.run_id)) == "ERROR");
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

TEST_CASE("cycle: begin leaves a LIVE cycle's stage alone", "[cycle]") {
    // A cycle that is still running is not an orphan. Its stage can be under an
    // INSERT that takes far longer than the advisory lease, so a second Begin
    // must not drop it -- doing so surfaced as an opaque "table does not exist"
    // blamed on SAP, rather than as the clean fencing refusal.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto live = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, live.stage_table);          // ...still being written

    // A second Begin must be REFUSED, not granted. The earlier version of this
    // test asserted the opposite -- it documented ownership theft as correct
    // behaviour, which is how the missing compare-and-swap survived review.
    REQUIRE_THROWS(cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart + 60));
    CHECK(Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name='" +
                          live.stage_table + "'") == "1");
    // ...and the live cycle can still commit, because nothing took it.
    REQUIRE_NOTHROW(cycle::Commit(con, "zdelta_wm", live.run_id, {2}));
}

TEST_CASE("cycle: begin discards a previous run's orphaned stage", "[cycle]") {
    // A cycle that died after staging leaves exactly one identifiable orphan.
    // It is free to discard because the watermark never moved.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto dead = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, dead.stage_table);   // ... and then the process dies

    // Deliberately NOT marking the run finished by hand. The earlier version of
    // this test did, and in doing so executed a statement no production path
    // executes -- which hid the fact that nothing ever leaves RUNNING, so every
    // dead cycle pinned its stage forever. Age it instead, which is exactly what
    // really happens.
    Exec(con, "UPDATE _erpl_rev_run_stats SET ts = now() - INTERVAL '2' HOUR "
              "WHERE run_id=" + std::to_string(dead.run_id));
    // The lease ages with it -- both are evidence of the same dead cycle, and
    // the claim will not be granted while either still looks alive.
    Exec(con, "UPDATE _erpl_rev_delta_state SET lease_ts = now() - INTERVAL '2' HOUR");

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
    // The two timestamps that make latency measurable after the fact: when the
    // source says it changed, and when we wrote it.
    // Compared against the same parse rather than a literal string: the column is
    // TIMESTAMPTZ, so its rendering depends on the session timezone and asserting
    // on the text would pass or fail by where the machine is.
    // A NUMTS change column comes from ABAP's GET TIME STAMP, which is UTC, so
    // it is read as UTC rather than as local wall-clock. Getting that wrong
    // shifts every latency percentile by the machine's offset -- which is
    // exactly what the first stress run showed, as a flat +7204s.
    CHECK(Scalar(con, "SELECT _commit_ts = (try_strptime('20260905110500','%Y%m%d%H%M%S') "
                      "AT TIME ZONE 'UTC') FROM " + log + " WHERE id=3") == "true");
    CHECK_FALSE(Scalar(con, "SELECT _applied_at FROM " + log + " WHERE id=3").empty());
    // Monotonic, which is what a subscription reader orders by -- and what
    // "count(DISTINCT _seq) == 2" did NOT establish: two distinct numbers are
    // distinct in either order, so a reversed or randomly assigned sequence
    // passed that assertion while making every offset-based reader wrong.
    // Compared in SQL, not by string: _seq is a number, and "10" sorts before
    // "9" as text.
    CHECK(Scalar(con, "SELECT (SELECT _seq FROM " + log + " WHERE id=2) < "
                      "(SELECT _seq FROM " + log + " WHERE id=3)") == "true");

    // And monotonic ACROSS cycles, not just within one: the next cycle's rows
    // must all sort after this cycle's, or a reader that stopped at the last
    // seq it saw would skip them.
    const auto b2 = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart + 600);
    Exec(con, "CREATE TABLE " + b2.stage_table + "(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO " + b2.stage_table + " VALUES (4,'d','20260905115900')");
    cycle::Commit(con, "zdelta_wm", b2.run_id, {1});
    CHECK(Scalar(con, "SELECT (SELECT min(_seq) FROM " + log + " WHERE id=4) > "
                      "(SELECT max(_seq) FROM " + log + " WHERE id<>4)") == "true");
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
    // Width-preserving: the predicate compares text, so the floor keeps the
    // stored watermark's width or it sorts wrong against it.
    CHECK(b.bounds.floor == "0950");
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, docnr BIGINT)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES (1,19000),(2,20000)");
    cycle::Commit(con, "docs", b.run_id, {2});

    // max(20000) - 50 units of overlap, at the source's width.
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

    // A repair is a real reload: the target is replaced by what the run staged,
    // so the row that the reload did not carry is correctly gone. Only the
    // watermark stays where it was -- that is this case's subject.
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");
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

TEST_CASE("cycle: a target reclaimed mid-commit is rolled back, not reported as done",
          "[cycle]") {
    // The pre-check at the top of Commit catches the common case. This is the
    // narrow one it cannot: the target is reclaimed AFTER that check, so the
    // fencing UPDATE matches zero rows. A zero-row UPDATE is not an error in
    // DuckDB, so without checking its result the cycle would merge, log, drop
    // its stage and report SUCCESS carrying a watermark that was never stored.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, b.stage_table);

    // Simulate the reclaim landing after Commit's pre-check: the row now belongs
    // to a different run, exactly as a second Begin would leave it.
    Exec(con, "UPDATE _erpl_rev_delta_state SET active_run_id = " +
                  std::to_string(b.run_id + 999) + " WHERE target='zdelta_wm'");

    REQUIRE_THROWS(cycle::Commit(con, "zdelta_wm", b.run_id, {2}));
    // Nothing of the cycle survives: not the merge, not the watermark.
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "20260905100000");
}

TEST_CASE("cycle: a counter watermark never moves backwards", "[cycle]") {
    // The safety overlap deliberately re-reads rows BELOW the stored watermark.
    // If a cycle happens to stage only those -- nothing new arrived -- then
    // max(staged) - safety_units lands below where the watermark already was,
    // and storing it walks the target backwards, widening the replay window a
    // little further on every quiet cycle.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    Exec(con, "CREATE TABLE docs(id INTEGER PRIMARY KEY, docnr BIGINT)");
    Exec(con, "INSERT INTO docs VALUES (1,980),(2,990)");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_units, "
              " cadence, status) VALUES "
              "('docs','WATERMARK','DOCS','id','DOCNR','INT','1000',50,'manual','IDLE')");

    const auto b = cycle::Begin(con, "docs", LoadType::Delta, kReadStart);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, docnr BIGINT)");
    // Only overlap rows: both below the stored watermark of 1000.
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES (1,980),(2,990)");
    cycle::Commit(con, "docs", b.run_id, {2});

    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "1000");
}

TEST_CASE("cycle: a first cycle is all-or-nothing, target creation included",
          "[cycle]") {
    // Commit is documented as ONE transaction. Creating the target outside it
    // meant a failure in between left user-visible rows in a table the watermark
    // says were never replicated -- and a retry then re-applies against a target
    // that already holds them.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_secs, "
              " cadence, status) VALUES "
              "('newt','WATERMARK','T','id','CHANGED_AT','NUMTS','',120,'manual','IDLE')");

    const auto b = cycle::Begin(con, "newt", LoadType::Delta, kReadStart);
    // A stage the commit cannot finish: the fence is stolen after Begin.
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, v VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES (1,'a')");
    Exec(con, "UPDATE _erpl_rev_delta_state SET active_run_id=" +
                  std::to_string(b.run_id + 999) + " WHERE target='newt'");

    REQUIRE_THROWS(cycle::Commit(con, "newt", b.run_id, {1}));
    // The target must not exist: nothing was committed, so nothing is visible.
    CHECK(Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name='newt'") == "0");
}

TEST_CASE("cycle: the change log follows the target when drift adds a column",
          "[cycle][log]") {
    // The log is created once from the target's shape. When the structure
    // watchdog later appends a column to the target, the next commit names that
    // column in its log INSERT -- and without reconciling, the statement fails
    // and the target never commits again. A logged target would be permanently
    // wedged by the very feature meant to keep it current.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con, /*log_enabled=*/true);

    auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, b.stage_table);
    cycle::Commit(con, "zdelta_wm", b.run_id, {2});

    // Drift: a new source column appears and is added to the target.
    Exec(con, "ALTER TABLE zdelta_wm ADD COLUMN zznew VARCHAR");

    b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart + 60);
    Exec(con, "CREATE TABLE " + b.stage_table +
                  "(id INTEGER, v VARCHAR, changed_at VARCHAR, zznew VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES (4,'d','20260905113000','new')");
    REQUIRE_NOTHROW(cycle::Commit(con, "zdelta_wm", b.run_id, {1}));

    const auto log = cycle::ChangeLogName("zdelta_wm");
    CHECK(Scalar(con, "SELECT zznew FROM " + log + " WHERE id=4") == "new");
}

TEST_CASE("cycle: a NULL key component does not duplicate on every cycle", "[cycle]") {
    // `t.k = s.k` is UNKNOWN when either side is NULL, so a NULL-keyed row never
    // matches its own copy in the target: the merge inserts it again, the log
    // records it as an insert again, and the row multiplies once per cycle --
    // forever, because the safety overlap keeps re-reading it.
    //
    // A NULL key is nearly always a modelling mistake, but it must not corrupt
    // the target when it happens.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    Exec(con, "CREATE TABLE nk(bukrs VARCHAR, belnr VARCHAR, v VARCHAR)");
    Exec(con, "INSERT INTO nk VALUES ('1000', NULL, 'first')");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_secs, "
              " cadence, status) VALUES "
              "('nk','WATERMARK','NK','bukrs,belnr','CHANGED_AT','NUMTS','20260905100000',120,"
              "'manual','IDLE')");

    for (int cycle_no = 0; cycle_no < 3; ++cycle_no) {
        const auto b = cycle::Begin(con, "nk", LoadType::Delta, kReadStart + cycle_no * 60);
        Exec(con, "CREATE TABLE " + b.stage_table + "(bukrs VARCHAR, belnr VARCHAR, v VARCHAR)");
        Exec(con, "INSERT INTO " + b.stage_table + " VALUES ('1000', NULL, 'updated')");
        cycle::Commit(con, "nk", b.run_id, {1});
    }

    // One source row, one target row -- not four.
    CHECK(Scalar(con, "SELECT count(*) FROM nk") == "1");
    CHECK(Scalar(con, "SELECT v FROM nk") == "updated");
}

TEST_CASE("cycle: a NULL key that MATCHES is logged as an update, not an insert",
          "[cycle][log]") {
    // With null-safe matching a NULL key can genuinely match an existing row.
    // Deriving _op by testing that key column for NULL would then call every
    // such match an insert -- so a replay of the log would insert rows the
    // target already had.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    Exec(con, "CREATE TABLE nk2(bukrs VARCHAR, belnr VARCHAR, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO nk2 VALUES ('1000', NULL, 'first', '20260905100000')");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_secs, "
              " cadence, status, log_enabled) VALUES "
              "('nk2','WATERMARK','NK2','bukrs,belnr','CHANGED_AT','NUMTS','20260905100000',120,"
              "'manual','IDLE',true)");

    const auto b = cycle::Begin(con, "nk2", LoadType::Delta, kReadStart);
    Exec(con, "CREATE TABLE " + b.stage_table +
                  "(bukrs VARCHAR, belnr VARCHAR, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES ('1000', NULL, 'updated', "
              "'20260905113000')");
    cycle::Commit(con, "nk2", b.run_id, {1});

    CHECK(Scalar(con, "SELECT _op FROM " + cycle::ChangeLogName("nk2")) == "U");
}

TEST_CASE("cycle: a failed commit counts against the target, a lost race does not",
          "[cycle]") {
    // fail_count is what the planner's backoff and parking read. Nothing
    // incremented it, so both were dead code and a broken target retried at full
    // micro cadence forever.
    //
    // The distinction matters: a cycle that lost a race to another cycle is not
    // evidence the TARGET is broken, and backing it off would punish a healthy
    // target for being popular.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    // A commit that genuinely fails: the stage cannot merge into the target.
    auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id VARCHAR, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES ('not-an-int','x','20260905110000')");
    REQUIRE_THROWS(cycle::Commit(con, "zdelta_wm", b.run_id, {1}));
    CHECK(Scalar(con, "SELECT fail_count FROM _erpl_rev_delta_state") == "1");

    // A cycle that merely lost the target does not count against it.
    b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart + 60);
    StageRows(con, b.stage_table);
    Exec(con, "UPDATE _erpl_rev_delta_state SET active_run_id=" +
                  std::to_string(b.run_id + 999) + " WHERE target='zdelta_wm'");
    REQUIRE_THROWS(cycle::Commit(con, "zdelta_wm", b.run_id, {2}));
    CHECK(Scalar(con, "SELECT fail_count FROM _erpl_rev_delta_state") == "1");
}

TEST_CASE("cycle: a wall-clock target refuses to run without SAP's clock", "[cycle]") {
    // DATS and TIMS are wall-clock in SAP's timezone. Without sap_now the server
    // silently fell back to its OWN clock, so on a system where the two differ
    // the ceiling -- and therefore the stored watermark -- jumped forward by the
    // offset, and everything committed in that window fell below the next floor
    // and was lost permanently.
    //
    // Guessing is the wrong response to a missing clock. Refusing is loud, and
    // the cycle simply runs again once the caller sends one.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    schema::Migrate(con, "test");
    Exec(con, "CREATE TABLE dt(id INTEGER, d DATE, t TIME)");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, time_col, wm_kind, wm_value, "
              " safety_secs, cadence, status) VALUES "
              "('dt','WATERMARK','DT','id','ERDAT','ERZET','DATETIME','20260905100000',120,"
              "'manual','IDLE')");

    REQUIRE_THROWS(cycle::Begin(con, "dt", LoadType::Delta, kReadStart, /*sap_now=*/""));
    // ...and with SAP's clock it starts normally.
    REQUIRE_NOTHROW(cycle::Begin(con, "dt", LoadType::Delta, kReadStart, "20260905120000"));
}

TEST_CASE("cycle: the watermark never moves backwards, whatever the clock does",
          "[cycle]") {
    // A clock that goes backwards -- an NTP correction, or a DST fall-back on a
    // wall-clock DATETIME target -- would otherwise rewind the watermark and
    // re-read a window that was already applied. Harmless in itself (the merge
    // is idempotent) but it compounds: each quiet cycle walks it back further.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    StageRows(con, b.stage_table);
    cycle::Commit(con, "zdelta_wm", b.run_id, {2});
    const auto after = Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state");

    // A cycle whose clock reads an hour EARLIER than the last one.
    b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart - 3600);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    cycle::Commit(con, "zdelta_wm", b.run_id, {0});

    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == after);
}

TEST_CASE("cycle: F is a real reload -- source deletions disappear from the target",
          "[cycle]") {
    // The repair an operator runs BECAUSE the target drifted. Upserting without
    // truncating leaves every row the source has since deleted, which is exactly
    // the drift they were trying to fix -- so F "succeeded" and changed nothing
    // about the problem.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);
    // The target has a row the source no longer has.
    Exec(con, "INSERT INTO zdelta_wm VALUES (99,'deleted-at-source','20260905090000')");
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "3");

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Full, kReadStart);
    CHECK(b.plan.truncate_target);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES (1,'a','20260905110000'),"
              "(2,'b','20260905110000')");
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {2});

    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm WHERE id=99") == "0");
    CHECK(r.del == 1);   // reported, not silently zero
    // ...and a repair still does not move the delta position.
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "20260905100000");
}

TEST_CASE("cycle: an ordinary delta cycle never truncates", "[cycle]") {
    // The other half of the same rule: D must leave rows it did not read alone,
    // or every delta cycle would empty the target down to its own batch.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    CHECK_FALSE(b.plan.truncate_target);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO " + b.stage_table + " VALUES (3,'c','20260905110000')");
    cycle::Commit(con, "zdelta_wm", b.run_id, {1});

    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "3");
}

// --- the fence, on its own ---------------------------------------------------
//
// The compare-and-swap that stores the watermark is the last thing standing
// between a reclaimed cycle and a target it no longer owns. Inside Commit its
// failing branch is only reachable by winning a race against another cycle
// between the pre-check and the transaction -- so it was, in practice,
// untested. As a named operation it is reachable directly.

TEST_CASE("cycle: the fence stores the watermark for the run that owns the target", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);
    Exec(con, "UPDATE _erpl_rev_delta_state SET active_run_id=77, status='RUNNING'");

    cycle::AdvanceWatermarkFenced(con, "zdelta_wm", 77, "20260905130000", 5);

    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == "20260905130000");
    CHECK(Scalar(con, "SELECT status FROM _erpl_rev_delta_state") == "IDLE");
    // Released, so the next cycle can claim it.
    CHECK(Scalar(con, "SELECT active_run_id FROM _erpl_rev_delta_state").empty());
    CHECK(Scalar(con, "SELECT rows_applied FROM _erpl_rev_delta_state") == "5");
}

TEST_CASE("cycle: the fence refuses a run that no longer owns the target", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);
    const auto before = Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state");
    // Reclaimed: run 77 did the work, run 78 owns the target now.
    Exec(con, "UPDATE _erpl_rev_delta_state SET active_run_id=78, status='RUNNING'");

    CHECK_THROWS_WITH(cycle::AdvanceWatermarkFenced(con, "zdelta_wm", 77, "20260905130000", 5),
                      Catch::Matchers::ContainsSubstring("lost ownership"));

    // The point of the throw: no watermark from a cycle that was displaced, and
    // the new owner's claim is intact.
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == before);
    CHECK(Scalar(con, "SELECT active_run_id FROM _erpl_rev_delta_state") == "78");
}

TEST_CASE("cycle: the fence refuses a target that is not registered at all", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    // Zero rows updated for a different reason, and it must still be a throw:
    // reporting SUCCESS for a watermark stored nowhere is the failure mode,
    // whatever made the WHERE miss.
    CHECK_THROWS_WITH(cycle::AdvanceWatermarkFenced(con, "no_such_target", 1, "20260905130000", 0),
                      Catch::Matchers::ContainsSubstring("lost ownership"));
}

// --- the empty batch ---------------------------------------------------------
//
// The commonest cycle in production, by a wide margin: a micro-cadence target
// where nothing changed. It had no test at all, and it is the one cycle whose
// watermark advance matters most -- a quiet target that does not advance its
// floor re-reads an ever-widening range until it is reading the whole table.

TEST_CASE("cycle: a cycle that reads nothing still advances the watermark", "[cycle]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con, /*log_enabled=*/true);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    // No stage table: the reader found nothing and created none.
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {0});

    CHECK(r.ins == 0);
    CHECK(r.upd == 0);
    CHECK(r.logged == 0);
    // The point. Standing still here means the next cycle re-reads from the old
    // floor, and the range grows with every quiet tick.
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") ==
          b.bounds.next_watermark);
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");
    CHECK(Scalar(con, "SELECT status FROM _erpl_rev_delta_state") == "IDLE");
    CHECK(Scalar(con, "SELECT active_run_id FROM _erpl_rev_delta_state").empty());
    CHECK(Scalar(con, "SELECT status FROM _erpl_rev_run_stats WHERE run_id=" +
                          std::to_string(b.run_id)) == "SUCCESS");
    // Nothing changed, so the log must record nothing -- an empty cycle that
    // wrote a row would make every subscriber's offset move for no data.
    CHECK(Scalar(con, "SELECT count(*) FROM " + cycle::ChangeLogName("zdelta_wm")) == "0");
}

TEST_CASE("cycle: an empty stage table is not the same as no stage table", "[cycle]") {
    // The reader created its stage and then had nothing to put in it. Same
    // outcome as above -- and the stage must still be dropped, or the next
    // cycle's orphan sweep is doing work that should never have been needed.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con, /*log_enabled=*/true);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Delta, kReadStart);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {0});

    CHECK(r.ins == 0);
    CHECK(r.upd == 0);
    CHECK(Scalar(con, "SELECT wm_value FROM _erpl_rev_delta_state") == b.bounds.next_watermark);
    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");
    CHECK(Scalar(con, "SELECT count(*) FROM duckdb_tables() WHERE table_name=" +
                      std::string("'") + b.stage_table + "'") == "0");
}

TEST_CASE("cycle: a full load that read nothing at all does not empty the target",
          "[cycle]") {
    // F truncates before it loads, and that is what the operator asked for. But
    // a read that produced no staging table at all did not decide the source is
    // empty -- it produced nothing: a short read that reported success, a
    // connection dropped between packages. Truncating on that evidence deletes
    // the customer's replicated table and reports SUCCESS. Keeping the previous
    // contents is recoverable and visible as rows_read=0; deleting them is not.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Full, kReadStart);
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {0});

    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "2");
    CHECK(r.del == 0);
}

TEST_CASE("cycle: a full load from a genuinely empty source does empty the target",
          "[cycle]") {
    // The other half, and the reason the rule above is about the STAGE rather
    // than about the row count: here the reader ran, created its stage and
    // found nothing. The source is empty, so the replica must be too --
    // otherwise F silently stops being a reload the moment a table is
    // truncated at the source.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Setup(con);

    const auto b = cycle::Begin(con, "zdelta_wm", LoadType::Full, kReadStart);
    Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    const auto r = cycle::Commit(con, "zdelta_wm", b.run_id, {0});

    CHECK(Scalar(con, "SELECT count(*) FROM zdelta_wm") == "0");
    CHECK(r.del == 2);
}
