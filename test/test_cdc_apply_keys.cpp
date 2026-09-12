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

#include <string>
#include <vector>

#include "cycle.hpp"
#include "duckdb_bridge.hpp"

using namespace erpl_rev;

namespace {
// A target, a keys-only shadow batch, and the re-read images for it.
// The three relations, typed the way production really types them -- and they
// are NOT all the same, which is the whole point.
//
// This fixture used to build t/klog/kimg out of INTEGER and VARCHAR only. Every
// coercion then degenerated to CAST(x AS x), so the date/time branches were
// unreachable and thirteen cases stayed green over a mode that had never
// completed a cycle anywhere.
//
//   t, kimg  what `replicate` produces: DATS -> DATE, TIMS -> TIME,
//            DEC -> DECIMAL, NUMC/CHAR -> VARCHAR
//   klog     what `replicate_native` produces from the HANA shadow log: every
//            column NVARCHAR, so the keys arrive as SAP-raw text ('20240101')
//
// Retyping the log side "to match" would be tidier and would stop exercising
// the text-parsing path, which is the one that must keep working for
// DELETE_ONLY and IMAGE_IUD. The asymmetry is the test.
const std::vector<std::string> kKeys = {"mandt", "carrid", "fldate"};

void SetupKeysTarget(DuckDbBridge &db) {
    db.Execute("CREATE TABLE t(mandt VARCHAR, carrid VARCHAR, fldate DATE, "
               "price DECIMAL(23,2), dep TIME, note VARCHAR, "
               "PRIMARY KEY(mandt,carrid,fldate))");
    db.Execute("INSERT INTO t VALUES "
               "('100','LH',DATE '2024-01-01',100.00,TIME '08:00:00','old-a'),"
               "('100','LH',DATE '2024-01-02',200.00,TIME '09:00:00','old-b'),"
               "('100','LH',DATE '2024-01-03',300.00,TIME '10:00:00','old-c')");
    db.CdcRegister("t", "T", "mandt,carrid,fldate", "HANA", "KEYS_IUD", "ZCDC_T_LOG");
    db.CdcSetStatus("t", "SEEDED");
    // The shadow log: keys, op, sequence. No row image -- that is the point.
    db.Execute("CREATE TABLE klog(mandt VARCHAR, carrid VARCHAR, fldate VARCHAR, "
               "\"_op\" VARCHAR, \"_seq\" BIGINT, \"_ts\" VARCHAR)");
}

void SetupKeysImages(DuckDbBridge &db) {
    db.Execute("CREATE TABLE kimg(mandt VARCHAR, carrid VARCHAR, fldate DATE, "
               "price DECIMAL(23,2), dep TIME, note VARCHAR)");
}
}  // namespace

TEST_CASE("cdc_keys: images supply the row values the shadow log does not carry",
          "[bridge][cdc][keys]") {
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000'),"
               "('100','LH','20240104','I',2,'20240115100000')");
    // Re-read of the source for the net I/U keys.
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','new-a'),"
               "('100','LH',DATE '2024-01-04',400.00,TIME '11:00:00','new-d')");

    auto r = db.CdcApply("t", "klog", kKeys, "kimg");
    REQUIRE(r.applied);
    CHECK(r.upd == 1);
    CHECK(r.ins == 1);
    CHECK(db.Query("SELECT note FROM t WHERE fldate=DATE '2024-01-01'").rows[0]
          == R"({"note":"new-a"})");
    CHECK(db.Query("SELECT note FROM t WHERE fldate=DATE '2024-01-04'").rows[0]
          == R"({"note":"new-d"})");
    // Typed, not text that happens to render the same.
    CHECK(db.Query("SELECT typeof(fldate) AS t FROM t LIMIT 1").rows[0] == R"({"t":"DATE"})");
    CHECK(db.Query("SELECT typeof(dep) AS t FROM t LIMIT 1").rows[0] == R"({"t":"TIME"})");
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
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000'),"
               "('100','LH','20240102','U',2,'20240115100000')");
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','new-a')");  // 01-02 gone

    auto r = db.CdcApply("t", "klog", kKeys, "kimg");
    REQUIRE(r.applied);
    CHECK(db.Query("SELECT count(*) AS c FROM t WHERE fldate=DATE '2024-01-02'").rows[0]
          == R"({"c":0})");
    CHECK(db.Query("SELECT note FROM t WHERE fldate=DATE '2024-01-01'").rows[0]
          == R"({"note":"new-a"})");
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
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240103','U',1,'20240115090000'),"
               "('100','LH','20240103','D',2,'20240115100000'),('100','LH','20240101','U',3,'20240115110000')");
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-03',300.00,TIME '10:00:00','stale-c'),"
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','new-a')");

    auto r = db.CdcApply("t", "klog", kKeys, "kimg");
    REQUIRE(r.applied);
    CHECK(db.Query("SELECT count(*) AS c FROM t WHERE fldate=DATE '2024-01-03'").rows[0]
          == R"({"c":0})");
    CHECK(db.Query("SELECT note FROM t WHERE fldate=DATE '2024-01-01'").rows[0]
          == R"({"note":"new-a"})");
    CHECK(r.del == 1);
}

TEST_CASE("cdc_keys: both staging tables and the position survive a rollback",
          "[bridge][cdc][keys]") {
    // A failed apply must leave the cycle exactly replayable: the position
    // unmoved and BOTH staging tables still there.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000')");
    // The image carries a value that cannot be cast into the target column.
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','not-a-number')");
    // Force the failure by making the target column type incompatible.
    db.Execute("ALTER TABLE t ALTER note TYPE INTEGER USING 0");

    REQUIRE_THROWS(db.CdcApply("t", "klog", kKeys, "kimg"));
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
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240102','D',1,'20240115090000')");
    SetupKeysImages(db);   // empty: nothing to re-read

    auto r = db.CdcApply("t", "klog", kKeys, "kimg");
    REQUIRE(r.applied);
    CHECK(r.del == 1);
    CHECK(db.Query("SELECT count(*) AS c FROM t").rows[0] == R"({"c":2})");
}

TEST_CASE("cdc_keys: an empty shadow batch leaves everything alone", "[bridge][cdc][keys]") {
    DuckDbBridge db;
    SetupKeysTarget(db);
    SetupKeysImages(db);

    auto r = db.CdcApply("t", "klog", kKeys, "kimg");
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

// A RAW/LRAW column maps to BLOB (zcl_erpl_rev_typemap.abap:44, drift.cpp:40),
// but the shadow log types every column NVARCHAR (cdc_dialect.cpp:69), and HANA
// renders a binary column into that as hex text. Casting that text straight to
// BLOB reinterprets the characters: 'A1B2C3' becomes the six ASCII bytes of the
// word, not the three bytes it spells. The value is wrong and nothing says so --
// the row applies, the count is right, and the payload is silently corrupt.
//
// Typed with a real BLOB column on purpose. An all-VARCHAR fixture degenerates
// every coercion to CAST(x AS x) and cannot see this at all.
TEST_CASE("cdc_image: a RAW column arrives as bytes, not as the text of its hex",
          "[bridge][cdc][keys]") {
    DuckDbBridge db;
    db.Execute("CREATE TABLE t(id INTEGER PRIMARY KEY, payload BLOB)");
    db.Execute("INSERT INTO t VALUES (1, unhex('0000'))");
    db.CdcRegister("t", "T", "id", "HANA", "IMAGE_IUD", "ZCDC_T_LOG");
    db.CdcSetStatus("t", "SEEDED");
    db.Execute("CREATE TABLE ilog(id VARCHAR, payload VARCHAR, "
               "\"_op\" VARCHAR, \"_seq\" BIGINT)");
    db.Execute("INSERT INTO ilog VALUES ('1','A1B2C3','U',1),('2','FFFE','I',2)");

    auto r = db.CdcApply("t", "ilog", {"id"});
    REQUIRE(r.applied);

    // Length first: it is the assertion that fails loudest on the old behaviour
    // (six bytes instead of three) and needs no hex round trip to read.
    CHECK(db.Query("SELECT octet_length(payload) AS n FROM t WHERE id=1").rows[0] ==
          R"({"n":3})");
    CHECK(db.Query("SELECT hex(payload) AS h FROM t WHERE id=1").rows[0] ==
          R"({"h":"A1B2C3"})");
    CHECK(db.Query("SELECT hex(payload) AS h FROM t WHERE id=2").rows[0] ==
          R"({"h":"FFFE"})");
}

TEST_CASE("cdc_image: a NULL RAW column stays NULL", "[bridge][cdc][keys]") {
    // unhex(NULL) is NULL, but only if the NULL survives the projection -- an
    // empty-string coalesce on the way in would turn an absent value into a
    // zero-length BLOB, which is a different value.
    DuckDbBridge db;
    db.Execute("CREATE TABLE t(id INTEGER PRIMARY KEY, payload BLOB)");
    db.Execute("INSERT INTO t VALUES (1, unhex('AA'))");
    db.CdcRegister("t", "T", "id", "HANA", "IMAGE_IUD", "ZCDC_T_LOG");
    db.CdcSetStatus("t", "SEEDED");
    db.Execute("CREATE TABLE ilog(id VARCHAR, payload VARCHAR, "
               "\"_op\" VARCHAR, \"_seq\" BIGINT)");
    db.Execute("INSERT INTO ilog VALUES ('1',NULL,'U',1)");

    auto r = db.CdcApply("t", "ilog", {"id"});
    REQUIRE(r.applied);
    CHECK(db.Query("SELECT payload IS NULL AS n FROM t WHERE id=1").rows[0] ==
          R"({"n":true})");
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
               "log_enabled) VALUES ('t','CDC','T','mandt,carrid,fldate',true)");
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000'),"
               "('100','LH','20240104','I',2,'20240115100000'),('100','LH','20240102','D',3,'20240115110000')");
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','new-a'),"
               "('100','LH',DATE '2024-01-04',400.00,TIME '11:00:00','new-d')");

    auto r = db.CdcApply("t", "klog", kKeys, "kimg");
    REQUIRE(r.applied);

    const std::string log = cycle::ChangeLogName("t");
    CHECK(db.Query("SELECT count(*) AS c FROM " + log).rows[0] == R"({"c":3})");
    // The same alphabet the watermark tier writes, so one reader serves both.
    CHECK(db.Query("SELECT count(*) AS c FROM " + log + " WHERE _op='D'").rows[0] ==
          R"({"c":1})");
}

TEST_CASE("cdc_keys: a key that vanished before the re-read is logged as a delete",
          "[bridge][cdc][keys]") {
    // The log's whole contract is that replaying it reproduces the target. A
    // net-I/U key the re-read could not find is DELETED from the target -- that
    // is the mirror race, and it is deliberate -- but it was written to the log
    // nowhere: the I/U append reads the images, where it is absent, and the
    // delete append read only the shadow log's net-'D' set, where its op is
    // 'U'. The row left the target and no subscriber was ever told.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, "
               "log_enabled) VALUES ('t','CDC','T','mandt,carrid,fldate',true)");
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240102','U',1,'20240115090000')");
    SetupKeysImages(db);   // the re-read found nothing

    auto r = db.CdcApply("t", "klog", kKeys, "kimg");
    REQUIRE(r.applied);

    CHECK(db.Query("SELECT count(*) AS c FROM t WHERE fldate=DATE '2024-01-02'").rows[0]
          == R"({"c":0})");
    const std::string log = cycle::ChangeLogName("t");
    CHECK(db.Query("SELECT count(*) AS c FROM " + log + " WHERE _op='D'").rows[0] ==
          R"({"c":1})");
}

TEST_CASE("cdc_keys: a delete for a key the target never held is not logged",
          "[bridge][cdc][keys]") {
    // The other direction, and the same invariant read backwards: the log must
    // not record a change the target never received. A trigger row can name a
    // key the target never had -- a seed whose snapshot post-dates the delete,
    // with the trigger row queued below the seeded position. Nothing is deleted
    // and res.del counts nothing, but the log gained a 'D' anyway, so the count
    // the sibling test asserts stopped matching what actually happened.
    DuckDbBridge db;
    SetupKeysTarget(db);   // target holds 1, 2, 3
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, "
               "log_enabled) VALUES ('t','CDC','T','mandt,carrid,fldate',true)");
    // A key the target never held.
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240109','D',1,'20240115090000')");
    SetupKeysImages(db);

    auto r = db.CdcApply("t", "klog", kKeys, "kimg");
    REQUIRE(r.applied);
    CHECK(r.del == 0);

    const std::string log = cycle::ChangeLogName("t");
    // The number of delete records must equal the number of deletes.
    CHECK(db.Query("SELECT count(*) AS c FROM " + log + " WHERE _op='D'").rows[0] ==
          R"({"c":0})");
}

TEST_CASE("cdc_keys: an apply that cannot succeed parks the target instead of looping",
          "[bridge][cdc][keys]") {
    // The rollback leaves the position unmoved and the staging table has a fixed
    // name the next cycle recreates, so a batch that cannot be applied re-staged
    // and re-failed every cycle forever -- with the reason stored nowhere and
    // the shadow log growing without bound. The tick planner skips a target that
    // is not ACTIVE or SEEDED, so recording the failure is what stops the loop.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, "
               "log_enabled) VALUES ('t','CDC','T','mandt,carrid,fldate',true)");
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000')");
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','new-a')");
    // A log table whose shape the append cannot satisfy.
    db.Execute("CREATE TABLE " + cycle::ChangeLogName("t") + "(nothing INTEGER)");

    CHECK_THROWS(db.CdcApply("t", "klog", kKeys, "kimg"));

    CHECK(db.CdcGet("t").status == "ERROR");
    CHECK(db.Query("SELECT count(*) AS c FROM _erpl_rev_cdc WHERE target='t' "
                   "AND error IS NOT NULL").rows[0] == R"({"c":1})");
    // Rolled back: the position did not move, so nothing was lost.
    CHECK(db.CdcGet("t").position == 0);
}

TEST_CASE("cdc_keys: a rolled-back apply leaves no change log behind",
          "[bridge][cdc][keys]") {
    // Both tiers make the same promise now: the log appears on a target's first
    // SUCCESSFUL cycle. The trigger tier used to provision before opening its
    // transaction, so a failed apply left an empty log table for a cycle that
    // never happened -- and the readers' "no table yet" handling then meant two
    // different things depending on which tier had touched the target.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, "
               "log_enabled) VALUES ('t','CDC','T','mandt,carrid,fldate',true)");
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000')");
    // No images table at all: the apply fails inside the transaction.
    CHECK_THROWS(db.CdcApply("t", "klog", kKeys, "no_such_images"));

    CHECK(db.Query("SELECT count(*) AS c FROM duckdb_tables() WHERE table_name='" +
                   cycle::ChangeLogName("t") + "'").rows[0] == R"({"c":0})");
}

TEST_CASE("cdc_keys: the log carries the engine's verdict and a real run id",
          "[bridge][cdc][keys]") {
    // Both tiers write one shared log, so a subscriber must be able to read it
    // one way. The trigger says what happened at the SOURCE; what a sink needs
    // is whether the key was already in the target -- a seeded target replaying
    // an old 'I' for a key it already holds would make a subscriber conflict on
    // insert. And _run_id was hard-coded 0, so anything joining the log to the
    // run statistics dropped every trigger-tier row.
    DuckDbBridge db;
    SetupKeysTarget(db);   // target holds 1, 2, 3
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, "
               "log_enabled) VALUES ('t','CDC','T','mandt,carrid,fldate',true)");
    // The source calls both an insert; only key 4 is new to the target.
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','I',1,'20240115090000'),"
               "('100','LH','20240104','I',2,'20240115100000')");
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','a2'),"
               "('100','LH',DATE '2024-01-04',400.00,TIME '11:00:00','d')");

    REQUIRE(db.CdcApply("t", "klog", kKeys, "kimg").applied);

    const std::string log = cycle::ChangeLogName("t");
    CHECK(db.Query("SELECT _op AS o FROM " + log + " WHERE fldate=DATE '2024-01-01'").rows[0]
          == R"({"o":"U"})");
    CHECK(db.Query("SELECT _op AS o FROM " + log + " WHERE fldate=DATE '2024-01-04'").rows[0]
          == R"({"o":"I"})");
    // The run id joins to a real statistics row.
    CHECK(db.Query("SELECT count(*) AS c FROM " + log + " l JOIN _erpl_rev_run_stats r "
                   "ON r.run_id = l._run_id").rows[0] == R"({"c":2})");
}

TEST_CASE("cdc_keys: a target column the images do not carry is refused, not emptied",
          "[bridge][cdc][keys]") {
    // The keys-mode apply is delete-then-insert, so a target column absent from
    // the re-read is set to NULL for every changed key -- not left stale, and
    // with no error. That is silent data loss on an ordinary trigger: a source
    // column dropped or retyped leaves the intersection and takes the target's
    // data with it.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000')");
    // The re-read produced everything except `dep`.
    db.Execute("CREATE TABLE kimg(mandt VARCHAR, carrid VARCHAR, fldate DATE, "
               "price DECIMAL(23,2), note VARCHAR)");
    db.Execute("INSERT INTO kimg VALUES ('100','LH',DATE '2024-01-01',150.00,'new-a')");

    REQUIRE_THROWS(db.CdcApply("t", "klog", kKeys, "kimg"));
    // Refused whole: the row still holds what it held.
    CHECK(db.Query("SELECT note FROM t WHERE fldate=DATE '2024-01-01'").rows[0]
          == R"({"note":"old-a"})");
    CHECK(db.CdcGet("t").position == 0);
}

TEST_CASE("cdc_keys: the change log records the shape a subscriber reads by",
          "[bridge][cdc][keys]") {
    // Counts alone were asserted, so nothing pinned what a sink actually reads:
    // the op alphabet, a commit timestamp, and a run id that joins. All three
    // were unreachable behind an apply that could not bind.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys, "
               "log_enabled) VALUES ('t','CDC','T','mandt,carrid,fldate',true)");
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000'),"
               "('100','LH','20240102','D',2,'20240115100000')");
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','new-a')");

    REQUIRE(db.CdcApply("t", "klog", kKeys, "kimg").applied);

    const std::string log = cycle::ChangeLogName("t");
    // Nothing outside the alphabet both tiers share.
    CHECK(db.Query("SELECT count(*) AS c FROM " + log +
                   " WHERE _op NOT IN ('I','U','D')").rows[0] == R"({"c":0})");
    CHECK(db.Query("SELECT count(*) AS c FROM " + log +
                   " WHERE _commit_ts IS NULL").rows[0] == R"({"c":0})");
    CHECK(db.Query("SELECT count(*) AS c FROM " + log +
                   " WHERE _run_id IS NULL OR _run_id = 0").rows[0] == R"({"c":0})");
    // The logged values are typed, not the log's raw text.
    CHECK(db.Query("SELECT typeof(fldate) AS t FROM " + log + " LIMIT 1").rows[0]
          == R"({"t":"DATE"})");
}

TEST_CASE("cdc_keys: replaying the same batch changes nothing", "[bridge][cdc][keys]") {
    // The position advanced on the first apply, so the second sees an empty
    // batch and must be a no-op. Idempotency is what makes a crashed cycle safe
    // to simply re-run, and nothing exercised it.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000')");
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','new-a')");

    REQUIRE(db.CdcApply("t", "klog", kKeys, "kimg").applied);
    const auto after = db.Query("SELECT count(*) AS c FROM t").rows[0];
    CHECK(db.CdcGet("t").position == 1);

    // The cycle re-stages under the same names; with the batch consumed it is empty.
    db.Execute("CREATE TABLE klog(mandt VARCHAR, carrid VARCHAR, fldate VARCHAR, "
               "\"_op\" VARCHAR, \"_seq\" BIGINT, \"_ts\" VARCHAR)");
    SetupKeysImages(db);
    auto r2 = db.CdcApply("t", "klog", kKeys, "kimg");
    CHECK_FALSE(r2.applied);
    CHECK(db.CdcGet("t").position == 1);
    CHECK(db.Query("SELECT count(*) AS c FROM t").rows[0] == after);
    CHECK(db.Query("SELECT note FROM t WHERE fldate=DATE '2024-01-01'").rows[0]
          == R"({"note":"new-a"})");
}

TEST_CASE("cdc_keys: a trigger cycle is visible on the operator surface",
          "[bridge][cdc][keys]") {
    // The apply wrote _erpl_rev_run_stats and _erpl_rev_cdc and stopped there.
    // Every operator surface -- `top`, `sync ls`, the Prometheus gauges, the
    // ALV report -- reads erpl_rev_targets, which is built from
    // _erpl_rev_delta_state. So a trigger target replicated correctly and
    // reported "IDLE, never run, 0 rows" for as long as it existed, and the
    // comment above the run-stats insert claimed one view answered for both
    // tiers while the view it named read a table this path never touched.
    //
    // An operator cannot monitor a tier that does not appear in the monitor.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO _erpl_rev_delta_state (target, method, source_from, keys) "
               "VALUES ('t','CDC','T','mandt,carrid,fldate')");
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000')");
    SetupKeysImages(db);
    db.Execute("INSERT INTO kimg VALUES "
               "('100','LH',DATE '2024-01-01',150.00,TIME '08:30:00','new-a')");

    REQUIRE(db.CdcApply("t", "klog", kKeys, "kimg").applied);

    // Ran, so it has a lag rather than "never"...
    CHECK(db.Query("SELECT count(*) AS c FROM erpl_rev_targets "
                   "WHERE target='t' AND lag_seconds IS NOT NULL").rows[0] == R"({"c":1})");
    // ...and the row count it moved is the one an operator sees.
    CHECK(db.Query("SELECT last_rows AS c FROM erpl_rev_targets WHERE target='t'").rows[0]
          == R"({"c":1})");
}

TEST_CASE("cdc_keys: a failed cycle reaches the operator surface, not only the registry",
          "[bridge][cdc][keys]") {
    // The apply's catch has always recorded the reason in _erpl_rev_cdc and
    // stopped the planner rescheduling the target. Both correct. Both
    // invisible: erpl_rev_targets -- which `top`, `sync ls`, the Prometheus
    // gauges and the ABAP ALV report are ALL built from -- comes from
    // _erpl_rev_delta_state, and this path never touched it.
    //
    // So a target whose every cycle was failing reported IDLE, never run, 0
    // rows, no error, to four surfaces at once. The registry knew; the operator
    // was told nothing.
    DuckDbBridge db;
    SetupKeysTarget(db);
    db.Execute("INSERT INTO _erpl_rev_delta_state "
               "(target, method, source_from, keys, cadence, status, fail_count) "
               "VALUES ('t','CDC','SFLIGHT','mandt,carrid,fldate','micro:2','IDLE',0)");
    db.Execute("INSERT INTO klog VALUES ('100','LH','20240101','U',1,'20240115090000')");

    // Fails for a reason the engine refuses on purpose: the images are missing
    // a column the target carries.
    db.Execute("CREATE TABLE kimg(mandt VARCHAR, carrid VARCHAR, fldate DATE, "
               "price DECIMAL(23,2), note VARCHAR)");
    db.Execute("INSERT INTO kimg VALUES ('100','LH',DATE '2024-01-01',150.00,'new-a')");
    REQUIRE_THROWS(db.CdcApply("t", "klog", kKeys, "kimg"));

    // The registry, as before.
    CHECK(db.Query("SELECT status FROM _erpl_rev_cdc WHERE target='t'").rows[0]
          == R"({"status":"ERROR"})");

    // And now the row every operator surface is built from.
    const auto v = db.Query("SELECT status, fail_count, is_healthy FROM erpl_rev_targets "
                            "WHERE target='t'");
    REQUIRE(v.rows.size() == 1);
    CHECK(v.rows[0].find(R"("status":"ERROR")") != std::string::npos);
    CHECK(v.rows[0].find(R"("fail_count":1)") != std::string::npos);
    CHECK(v.rows[0].find(R"("is_healthy":false)") != std::string::npos);

    // The reason, not just the fact. A status with no reason is a status
    // nobody can act on, which is what sent this to a live bisection the first
    // time it happened.
    const auto err = db.Query("SELECT last_error FROM erpl_rev_targets WHERE target='t'")
                         .rows[0];
    INFO("last_error was: " << err);
    CHECK(err.find("dep") != std::string::npos);
}
