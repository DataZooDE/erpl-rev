// KEYS_IUD apply: the shadow log carries keys only, and the row images come
// from a second staging table the cycle filled by re-reading the source.
//
// The characterization half of this tier is already implemented -- the coalesce
// to a net op per key, the single transaction, the monotonic position -- so
// those are pinned as C-* cases rather than pretended to be new.
//
// The genuinely new part is the two-table apply, and it has two failure modes
// that a plausible implementation gets wrong while passing the obvious test.
// Both are written here before the implementation exists.

#include <catch2/catch_test_macros.hpp>

#include "cycle.hpp"
#include "duckdb_bridge.hpp"

using namespace erpl_rev;

namespace {
// A target, a keys-only shadow batch, and the re-read images for it.
void SetupKeysTarget(DuckDbBridge &db) {
    db.Execute("CREATE TABLE t(id INTEGER PRIMARY KEY, v VARCHAR)");
    db.Execute("INSERT INTO t VALUES (1,'old-a'),(2,'old-b'),(3,'old-c')");
    db.CdcRegister("t", "T", "id", "HANA", "KEYS_IUD", "ZCDC_T_LOG");
    db.CdcSetStatus("t", "SEEDED");
    // The shadow log: keys, op, sequence. No row image -- that is the point.
    db.Execute("CREATE TABLE klog(id INTEGER, \"_op\" VARCHAR, \"_seq\" BIGINT)");
}
}  // namespace

TEST_CASE("cdc_keys: images supply the row values the shadow log does not carry",
          "[bridge][cdc][keys]") {
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES (1,'U',1),(4,'I',2)");
    // Re-read of the source for the net I/U keys.
    db.Execute("CREATE TABLE kimg(id INTEGER, v VARCHAR)");
    db.Execute("INSERT INTO kimg VALUES (1,'new-a'),(4,'new-d')");

    auto r = db.CdcApply("t", "klog", {"id"}, "kimg");
    REQUIRE(r.applied);
    CHECK(r.upd == 1);
    CHECK(r.ins == 1);
    CHECK(db.Query("SELECT v FROM t WHERE id=1").rows[0] == R"({"v":"new-a"})");
    CHECK(db.Query("SELECT v FROM t WHERE id=4").rows[0] == R"({"v":"new-d"})");
    CHECK(db.CdcGet("t").position == 2);
}

TEST_CASE("cdc_keys: a key that vanished between the shadow read and the re-read is deleted",
          "[bridge][cdc][keys]") {
    // The race that a plausible implementation gets wrong.
    //
    // The trigger logged an insert or update for key 2. By the time the cycle
    // re-read the source, the row had been deleted, so it is absent from the
    // images. Upserting only what the images contain leaves the stale old row in
    // the target forever -- the target now disagrees with the source and no
    // later cycle will ever revisit that key, because nothing will change it
    // again.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES (1,'U',1),(2,'U',2)");
    db.Execute("CREATE TABLE kimg(id INTEGER, v VARCHAR)");
    db.Execute("INSERT INTO kimg VALUES (1,'new-a')");   // key 2 is gone

    auto r = db.CdcApply("t", "klog", {"id"}, "kimg");
    REQUIRE(r.applied);
    CHECK(db.Query("SELECT count(*) AS c FROM t WHERE id=2").rows[0] == R"({"c":0})");
    CHECK(db.Query("SELECT v FROM t WHERE id=1").rows[0] == R"({"v":"new-a"})");
    // It left as a delete, so it is counted as one.
    CHECK(r.del == 1);
}

TEST_CASE("cdc_keys: an image whose net op is a delete is not resurrected",
          "[bridge][cdc][keys]") {
    // The mirror-image race. Key 3 was updated and then deleted inside the same
    // batch, so its net op is D -- but the re-read ran between those two events
    // and captured a row image for it. Reading the images without joining them
    // back to the net-I/U key set would re-insert a row the source no longer has.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES (3,'U',1),(3,'D',2),(1,'U',3)");
    db.Execute("CREATE TABLE kimg(id INTEGER, v VARCHAR)");
    db.Execute("INSERT INTO kimg VALUES (3,'stale-c'),(1,'new-a')");

    auto r = db.CdcApply("t", "klog", {"id"}, "kimg");
    REQUIRE(r.applied);
    CHECK(db.Query("SELECT count(*) AS c FROM t WHERE id=3").rows[0] == R"({"c":0})");
    CHECK(db.Query("SELECT v FROM t WHERE id=1").rows[0] == R"({"v":"new-a"})");
    CHECK(r.del == 1);
}

TEST_CASE("cdc_keys: both staging tables and the position survive a rollback",
          "[bridge][cdc][keys]") {
    // A failed apply must leave the cycle exactly replayable: the position
    // unmoved and BOTH staging tables still there.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES (1,'U',1)");
    // The image carries a value that cannot be cast into the target column.
    db.Execute("CREATE TABLE kimg(id INTEGER, v VARCHAR)");
    db.Execute("INSERT INTO kimg VALUES (1,'ok')");
    // Force the failure by making the target column type incompatible.
    db.Execute("ALTER TABLE t ALTER v TYPE INTEGER USING 0");

    REQUIRE_THROWS(db.CdcApply("t", "klog", {"id"}, "kimg"));
    CHECK(db.CdcGet("t").position == 0);
    CHECK(db.Query("SELECT count(*) AS c FROM duckdb_tables() WHERE table_name='klog'").rows[0]
          == R"({"c":1})");
    CHECK(db.Query("SELECT count(*) AS c FROM duckdb_tables() WHERE table_name='kimg'").rows[0]
          == R"({"c":1})");
}

TEST_CASE("cdc_keys: deletes still come from the log, not from the images",
          "[bridge][cdc][keys]") {
    // A delete carries no image by definition -- the row is gone. The delete
    // path must therefore keep reading the shadow log.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES (2,'D',1)");
    db.Execute("CREATE TABLE kimg(id INTEGER, v VARCHAR)");   // empty: nothing to re-read

    auto r = db.CdcApply("t", "klog", {"id"}, "kimg");
    REQUIRE(r.applied);
    CHECK(r.del == 1);
    CHECK(db.Query("SELECT count(*) AS c FROM t").rows[0] == R"({"c":2})");
}

TEST_CASE("cdc_keys: an empty shadow batch leaves everything alone", "[bridge][cdc][keys]") {
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("CREATE TABLE kimg(id INTEGER, v VARCHAR)");

    auto r = db.CdcApply("t", "klog", {"id"}, "kimg");
    CHECK_FALSE(r.applied);
    CHECK(db.CdcGet("t").position == 0);
    CHECK(db.Query("SELECT count(*) AS c FROM t").rows[0] == R"({"c":3})");
}

TEST_CASE("cdc_keys: image mode still works with no images table", "[bridge][cdc][keys]") {
    // C-*: characterization. The existing IMAGE_IUD path is unchanged -- the
    // images argument defaults to empty and the row values come from the log.
    DuckDbBridge db;
    db.Execute("CREATE TABLE t(id INTEGER PRIMARY KEY, v VARCHAR)");
    db.Execute("INSERT INTO t VALUES (1,'a')");
    db.CdcRegister("t", "T", "id", "HANA", "IMAGE_IUD", "ZCDC_T_LOG");
    db.CdcSetStatus("t", "SEEDED");
    db.Execute("CREATE TABLE ilog(id INTEGER, v VARCHAR, \"_op\" VARCHAR, \"_seq\" BIGINT)");
    db.Execute("INSERT INTO ilog VALUES (1,'from-log','U',1)");

    auto r = db.CdcApply("t", "ilog", {"id"});
    REQUIRE(r.applied);
    CHECK(db.Query("SELECT v FROM t WHERE id=1").rows[0] == R"({"v":"from-log"})");
}

TEST_CASE("cdc_keys: a log-enabled trigger target actually gets a change log",
          "[bridge][cdc][keys]") {
    // The trigger tier had no log provisioner at all. It probed for the table
    // and, finding none, silently wrote nothing -- and nothing else in the tree
    // creates a log for a CDC target, so a log-enabled trigger target had no
    // log, forever, and said so nowhere. Every subscription on such a target
    // published an empty stream and reported success.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, "
               "log_enabled) VALUES ('t','CDC','T','id',true)");
    db.Execute("INSERT INTO klog VALUES (1,'U',1),(4,'I',2),(2,'D',3)");
    db.Execute("CREATE TABLE kimg(id INTEGER, v VARCHAR)");
    db.Execute("INSERT INTO kimg VALUES (1,'new-a'),(4,'new-d')");

    auto r = db.CdcApply("t", "klog", {"id"}, "kimg");
    REQUIRE(r.applied);

    const std::string log = cycle::ChangeLogName("t");
    CHECK(db.Query("SELECT count(*) AS c FROM " + log).rows[0] == R"({"c":3})");
    // The same alphabet the watermark tier writes, so one reader serves both.
    CHECK(db.Query("SELECT count(*) AS c FROM " + log + " WHERE _op='D'").rows[0] ==
          R"({"c":1})");
}
