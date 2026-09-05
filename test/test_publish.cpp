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
void SetupLog(duckdb::Connection &con) {
    schema::Migrate(con, "test");
    Exec(con, "CREATE TABLE t(id INTEGER, v VARCHAR)");
    const auto log = cycle::ChangeLogName("t");
    Exec(con, "CREATE TABLE " + log + "(id INTEGER, v VARCHAR, _seq BIGINT, _op VARCHAR, "
              "_run_id BIGINT, _changed_at TIMESTAMPTZ, _commit_ts TIMESTAMPTZ)");
    Exec(con, "INSERT INTO " + log + " VALUES "
              "(1,'a',1,'I',10,now(),now()),(2,'b',2,'I',10,now(),now()),"
              "(1,'a2',3,'U',11,now(),now()),(3,'c',4,'I',11,now(),now()),"
              "(2,'b2',5,'U',12,now(),now())");
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
    Exec(con, "CREATE TABLE sink1(id INTEGER, v VARCHAR)");

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
    Exec(con, "CREATE TABLE sink_fast(id INTEGER, v VARCHAR)");
    Exec(con, "CREATE TABLE sink_slow(id INTEGER, v VARCHAR)");

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

TEST_CASE("subscription: key deduplication keeps the last write per key", "[publish]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CreateSubscription(con, "dedup", "t", "TABLE:sink_d:APPEND");
    Exec(con, "UPDATE _erpl_rev_subscription SET dedup_keys='id' WHERE name='dedup'");
    Exec(con, "CREATE TABLE sink_d(id INTEGER, v VARCHAR)");

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
    Exec(con, "CREATE TABLE sink_r(id INTEGER, v VARCHAR)");

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
    Exec(con, "CREATE TABLE sf(id INTEGER, v VARCHAR)");
    Exec(con, "CREATE TABLE sl(id INTEGER, v VARCHAR)");

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
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    CHECK(Retain(con, "t", 0) == 5);
}

TEST_CASE("retention: the window protects recent rows", "[publish]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    SetupLog(con);
    // Everything was just written, so an hour-long window keeps all of it.
    CHECK(Retain(con, "t", 3600) == 0);
}
