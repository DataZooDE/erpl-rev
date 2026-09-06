// Publishing and subscriptions.
//
// A subscription reads a target's change log from an offset, publishes what it
// finds, and advances -- atomically, so a failed publish cannot leave the offset
// claiming work that never landed.
//
// The SQL generation is here rather than in ABAP because a subscription publish
// touches no SAP data at all: routing it through an RFC round trip would be a
// trip to SAP to ask DuckDB to copy one of its own tables, and it could not be
// in the same transaction as the offset advance. zcl_erpl_rev_util=>publish
// keeps its own path for the ad-hoc/GUI case.

#include <catch2/catch_test_macros.hpp>
#include <duckdb.hpp>


#include "control_schema.hpp"
#include "cycle.hpp"
#include "publish.hpp"
#include "temp_path.hpp"

using namespace erpl_rev;

namespace {
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

// A target with a change log holding five changes.
//
// Built by running REAL cycles rather than by hand-writing the log DDL. The
// hand-built version carried a _changed_at column the product has never
// created, so a retention bug that threw on every production target passed CI
// for as long as it existed. A fixture that invents its own schema cannot catch
// a mismatch with the real one.
void SetupLog(duckdb::Connection &con) {
    schema::Migrate(con, "test");
    Exec(con, "CREATE TABLE t(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "INSERT INTO _erpl_rev_delta_state "
              "(target, method, source_from, keys, chg_col, wm_kind, wm_value, safety_secs, "
              " cadence, status, log_enabled) VALUES "
              "('t','WATERMARK','T','id','CHANGED_AT','NUMTS','20260101000000',0,"
              "'manual','IDLE',true)");

    // Three cycles: two inserts, then an update plus an insert, then an update.
    const char *batches[] = {
        "(1,'a','20260905120000'),(2,'b','20260905120000')",
        "(1,'a2','20260905120100'),(3,'c','20260905120100')",
        "(2,'b2','20260905120200')",
    };
    for (const auto *rows : batches) {
        const auto b = cycle::Begin(con, "t", LoadType::Delta, 1788609600);
        Exec(con, "CREATE TABLE " + b.stage_table + "(id INTEGER, v VARCHAR, changed_at VARCHAR)");
        Exec(con, "INSERT INTO " + b.stage_table + " VALUES " + std::string(rows));
        cycle::Commit(con, "t", b.run_id, {2});
    }
}
}  // namespace

TEST_CASE("publish: parquet and attached-table SQL", "[publish]") {
    Sink s;
    s.kind = SinkKind::Parquet;
    s.dest = "/tmp/out.parquet";
    CHECK(PublishSql(s, "src").find("COPY (SELECT * FROM src) TO '/tmp/out.parquet'") == 0);

    s.partition_by = "bukrs";
    const auto part = PublishSql(s, "src");
    CHECK(part.find("PARTITION_BY (bukrs)") != std::string::npos);
    // Without this a re-publish onto an existing dataset directory fails.
    CHECK(part.find("OVERWRITE_OR_IGNORE") != std::string::npos);

    s = Sink{};
    s.kind = SinkKind::Table;
    s.dest = "lake.main.t";
    s.mode = SinkMode::Append;
    CHECK(PublishSql(s, "src") == "INSERT INTO lake.main.t SELECT * FROM src;");
    s.mode = SinkMode::Full;
    CHECK(PublishSql(s, "src").find("DROP TABLE IF EXISTS lake.main.t") == 0);
}

TEST_CASE("publish: an unknown sink kind is refused, not silently skipped",
          "[publish]") {
    Sink s;
    s.kind = static_cast<SinkKind>(99);
    s.dest = "x";
    CHECK_THROWS(PublishSql(s, "src"));
}

TEST_CASE("subscription: advance publishes only what is past the offset", "[publish]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CreateSubscription(con, "s1", "t", "TABLE:sink1:APPEND");
    Exec(con, "CREATE TABLE sink1(id INTEGER, v VARCHAR, changed_at VARCHAR)");

    const auto r1 = Advance(con, "s1");
    CHECK(r1.published == 5);
    CHECK(Scalar(con, "SELECT count(*) FROM sink1") == "5");
    CHECK(Scalar(con, "SELECT \"offset\" FROM _erpl_rev_subscription WHERE name='s1'") == "5");

    // Nothing new: a second advance is a no-op, not a re-publish.
    const auto r2 = Advance(con, "s1");
    CHECK(r2.published == 0);
    CHECK(Scalar(con, "SELECT count(*) FROM sink1") == "5");
}

TEST_CASE("subscription: two subscriptions advance independently", "[publish]") {
    // The point of the change log: read the source once, fan out to many sinks,
    // each at its own pace.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CreateSubscription(con, "fast", "t", "TABLE:sink_fast:APPEND");
    CreateSubscription(con, "slow", "t", "TABLE:sink_slow:APPEND");
    Exec(con, "CREATE TABLE sink_fast(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "CREATE TABLE sink_slow(id INTEGER, v VARCHAR, changed_at VARCHAR)");

    Advance(con, "fast");
    CHECK(Scalar(con, "SELECT \"offset\" FROM _erpl_rev_subscription WHERE name='fast'") == "5");
    CHECK(Scalar(con, "SELECT \"offset\" FROM _erpl_rev_subscription WHERE name='slow'") == "0");
    CHECK(Scalar(con, "SELECT count(*) FROM sink_slow") == "0");
}

TEST_CASE("subscription: a failed publish leaves the offset unmoved", "[publish]") {
    // Otherwise the subscription claims to have delivered rows that never landed,
    // and no later advance will ever revisit them.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CreateSubscription(con, "broken", "t", "TABLE:does_not_exist:APPEND");

    CHECK_THROWS(Advance(con, "broken"));
    CHECK(Scalar(con, "SELECT \"offset\" FROM _erpl_rev_subscription WHERE name='broken'") == "0");
}

TEST_CASE("subscription: a parquet sink that cannot be written leaves the offset unmoved",
          "[publish]") {
    // The same guarantee as above, on the OTHER sink path. A table sink fails
    // inside DuckDB's catalogue; a parquet sink fails in the filesystem, at COPY
    // time, and nothing in the code makes the two share a failure route -- so
    // covering only the table sink left the file path free to advance an offset
    // past rows that were never written to a file anybody can read.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CreateSubscription(con, "brokenfile", "t",
                       "PARQUET:/nonexistent-erpl-rev-dir/does/not/exist/out.parquet:FULL");

    CHECK_THROWS(Advance(con, "brokenfile"));
    CHECK(Scalar(con, "SELECT \"offset\" FROM _erpl_rev_subscription WHERE name='brokenfile'") ==
          "0");

    // And it is recoverable: pointed somewhere writable, the same subscription
    // delivers the rows it never lost.
    Exec(con, "UPDATE _erpl_rev_subscription SET sink_spec='PARQUET:" +
                  erpl_rev_test::TmpPath("erpl_rev_pubfail.parquet") +
                  ":FULL' WHERE name='brokenfile'");
    const auto r = Advance(con, "brokenfile");
    CHECK(r.published == 5);
}

TEST_CASE("subscription: key deduplication keeps the last write per key", "[publish]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CreateSubscription(con, "dedup", "t", "TABLE:sink_d:APPEND");
    Exec(con, "UPDATE _erpl_rev_subscription SET dedup_keys='id' WHERE name='dedup'");
    Exec(con, "CREATE TABLE sink_d(id INTEGER, v VARCHAR, changed_at VARCHAR)");

    const auto r = Advance(con, "dedup");
    // Five log rows, three distinct keys.
    CHECK(r.published == 3);
    CHECK(Scalar(con, "SELECT v FROM sink_d WHERE id=1") == "a2");
    CHECK(Scalar(con, "SELECT v FROM sink_d WHERE id=2") == "b2");
}

TEST_CASE("subscription: replay from an earlier offset reproduces the sink",
          "[publish]") {
    // Recovery of previous deltas, served from the log -- never by re-reading SAP.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CreateSubscription(con, "r", "t", "TABLE:sink_r:APPEND");
    Exec(con, "CREATE TABLE sink_r(id INTEGER, v VARCHAR, changed_at VARCHAR)");

    Advance(con, "r");
    const auto first = Scalar(con, "SELECT count(*) FROM sink_r");

    Exec(con, "DELETE FROM sink_r");
    Exec(con, "UPDATE _erpl_rev_subscription SET \"offset\"=0 WHERE name='r'");
    Advance(con, "r");
    CHECK(Scalar(con, "SELECT count(*) FROM sink_r") == first);
}

TEST_CASE("retention: rows are kept until every subscription has passed them",
          "[publish]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CreateSubscription(con, "fast", "t", "TABLE:sf:APPEND");
    CreateSubscription(con, "slow", "t", "TABLE:sl:APPEND");
    Exec(con, "CREATE TABLE sf(id INTEGER, v VARCHAR, changed_at VARCHAR)");
    Exec(con, "CREATE TABLE sl(id INTEGER, v VARCHAR, changed_at VARCHAR)");

    Advance(con, "fast");   // fast is at 5, slow still at 0

    // The low-water mark is the SLOWEST subscription. Pruning to the fastest
    // would delete rows the slow one has not read -- silent data loss for it.
    const auto pruned = Retain(con, "t", /*window_secs=*/0);
    CHECK(pruned == 0);
    CHECK(Scalar(con, "SELECT count(*) FROM " + cycle::ChangeLogName("t")) == "5");

    Advance(con, "slow");
    CHECK(Retain(con, "t", 0) == 5);
    CHECK(Scalar(con, "SELECT count(*) FROM " + cycle::ChangeLogName("t")) == "0");
}

TEST_CASE("retention: a target with no subscriptions keeps only the window",
          "[publish]") {
    // Otherwise an unsubscribed log grows without bound.
    //
    // This asserted only that a ZERO window deletes everything -- which says
    // nothing about a window, and left the case the name promises untested:
    // that with no subscriber the window is the ONLY thing holding rows back.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    const std::string log = cycle::ChangeLogName("t");

    // Inside the window, nothing goes, subscriber or not.
    CHECK(Retain(con, "t", 3600) == 0);
    CHECK(Scalar(con, "SELECT count(*) FROM " + log) == "5");

    // Age two rows out of it, and exactly those two go -- not all five, and not
    // none. A retention that deleted by count or ignored the window would pass
    // the old assertion and fail this one.
    Exec(con, "UPDATE " + log + " SET _applied_at = now() - INTERVAL '2 hours' WHERE _seq <= 2");
    CHECK(Retain(con, "t", 3600) == 2);
    CHECK(Scalar(con, "SELECT count(*) FROM " + log) == "3");
    CHECK(Scalar(con, "SELECT min(_seq) FROM " + log) == "3");

    // And with the window closed, the rest follows: nothing is subscribed, so
    // nothing is holding them.
    CHECK(Retain(con, "t", 0) == 3);
    CHECK(Scalar(con, "SELECT count(*) FROM " + log) == "0");
}

TEST_CASE("retention: the window protects recent rows", "[publish]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    // Everything was just written, so an hour-long window keeps all of it.
    CHECK(Retain(con, "t", 3600) == 0);
}

TEST_CASE("retention: a window actually works against a real log", "[publish]") {
    // The regression this exists for: Retain() filtered on a column the change
    // log has never had, so windowed retention threw on every production target
    // while the suite stayed green -- because the fixture invented the schema
    // the code wanted. This asserts against a log built by real cycles.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);

    const auto before = Scalar(con, "SELECT count(*) FROM " + cycle::ChangeLogName("t"));
    CHECK(before != "0");

    // A generous window protects everything just written...
    CHECK(Retain(con, "t", 3600) == 0);
    CHECK(Scalar(con, "SELECT count(*) FROM " + cycle::ChangeLogName("t")) == before);

    // ...and a zero window, with no subscriptions to wait for, prunes it.
    CHECK(Retain(con, "t", 0) > 0);
}
