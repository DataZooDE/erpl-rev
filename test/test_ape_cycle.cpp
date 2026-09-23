// ApeCycle APE_FULL: decoded package -> staging -> snapshot merge.
//
// One FULL scan is a staging build followed by a set-based reconcile: every
// package lands in <target>__apesnap, and the last batch reconciles it onto
// the target (upsert present + delete absent = CURR semantics, the same merge
// the SNAPSHOT method uses). A row carrying a D operation inside a FULL scan
// is refused: its image is keys-only, so upserting it would plant a
// half-NULL row instead of failing loudly.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "ape_cycle.hpp"
#include "duckdb_bridge.hpp"

using namespace erpl_rev::ape;
using Catch::Matchers::ContainsSubstring;

namespace {

// A data envelope in the engine's wire shape (see test_ape_envelope.cpp):
// Fields describe, Body carries CSV, message.lastBatch ends the scan.
std::string Pkg(const std::string &fields, const std::string &body, bool last) {
    return std::string("{\"Encoding\":\"csv\",\"Attributes\":{") +
           "\"message.batchIndex\":0,\"message.lastBatch\":" +
           (last ? "true" : "false") + ",\"ABAP\":{\"Fields\":[" + fields +
           "]}},\"Body\":\"" + body + "\"}";
}

const char *kFields =
    "{\"Name\":\"RID\",\"Type\":\"CHAR10\",\"Kind\":\"C\",\"Length\":10,\"Decimals\":0},"
    "{\"Name\":\"DESCR\",\"Type\":\"CHAR40\",\"Kind\":\"C\",\"Length\":40,\"Decimals\":0},"
    "{\"Name\":\"AMOUNT\",\"Type\":\"S_PRICE\",\"Kind\":\"P\",\"Length\":8,\"Decimals\":2},"
    "{\"Name\":\"/1DH/OPERATION\",\"Type\":\"CHAR1\",\"Kind\":\"C\",\"Length\":1,\"Decimals\":0}";

void MakeTarget(erpl_rev::DuckDbBridge &db) {
    db.Execute("CREATE TABLE t(rid VARCHAR PRIMARY KEY, descr VARCHAR, "
               "amount DECIMAL(10,2))");
    db.Execute("INSERT INTO t VALUES ('1','a',10.00),('2','b',20.00),"
               "('3','c',30.00)");
}

std::string Cell(erpl_rev::DuckDbBridge &db, const std::string &sql) {
    auto r = db.Query(sql);
    REQUIRE(r.row_count == 1);
    return r.rows[0];
}

}  // namespace

TEST_CASE("ape_cycle: FULL scan upserts present and deletes absent", "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    // id=1 unchanged, id=2 changed, id=4 new, id=3 gone. Blank operation =
    // initial-load after-image (BRD BR-3).
    auto a = ApeApplyFullPackage(
        db, "t", Pkg(kFields, "1,a,10.00,\\n2,B,20.00,\\n4,d,40.00,\\n", false), 0);
    CHECK(a.rows_staged == 3);
    CHECK_FALSE(a.last_batch);
    auto c = ApeFinalizeFull(db, "t", {"rid"});
    CHECK(c.ins == 1);
    CHECK(c.upd == 2);
    CHECK(c.del == 1);
    CHECK(Cell(db, "SELECT count(*) AS c FROM t") == R"({"c":3})");
    CHECK(Cell(db, "SELECT descr AS d FROM t WHERE rid='2'") == R"({"d":"B"})");
    CHECK(Cell(db, "SELECT count(*) AS c FROM t WHERE rid='3'") == R"({"c":0})");
}

TEST_CASE("ape_cycle: rows map by name, case-insensitively, in any order",
          "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    // Fields reordered and lowercased: values must still land in the right
    // target columns (BRD FR-7). Position-based mapping would swap descr and
    // amount here.
    const char *reordered =
        "{\"Name\":\"amount\",\"Type\":\"S_PRICE\",\"Kind\":\"P\",\"Length\":8,\"Decimals\":2},"
        "{\"Name\":\"rid\",\"Type\":\"CHAR10\",\"Kind\":\"C\",\"Length\":10,\"Decimals\":0},"
        "{\"Name\":\"descr\",\"Type\":\"CHAR40\",\"Kind\":\"C\",\"Length\":40,\"Decimals\":0}";
    auto a = ApeApplyFullPackage(
        db, "t", Pkg(reordered, "99.50,9,nine\\n", true), 0);
    CHECK(a.rows_staged == 1);
    CHECK(a.last_batch);
    CHECK(Cell(db, "SELECT descr AS d FROM t__apesnap WHERE rid='9'") ==
          R"({"d":"nine"})");
}

TEST_CASE("ape_cycle: a short row is refused with package and row identity",
          "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    try {
        ApeApplyFullPackage(
            db, "t", Pkg(kFields, "1,a,10.00,\\n2,b\\n", false), 0);
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("row 1"));
    }
    // The failed package stages nothing.
    CHECK(Cell(db, "SELECT count(*) AS c FROM t__apesnap") == R"({"c":0})");
}

TEST_CASE("ape_cycle: a D operation inside FULL is refused, not upserted",
          "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    // A delete image is keys-only: upserting it would plant descr=NULL,
    // amount=NULL over a live row. A FULL scan must never carry one.
    try {
        ApeApplyFullPackage(db, "t", Pkg(kFields, "2,,,D\\n", false), 0);
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("D"));
    }
    CHECK(Cell(db, "SELECT descr AS d FROM t WHERE rid='2'") == R"({"d":"b"})");
}

TEST_CASE("ape_cycle: a package missing a target column is refused", "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    // No DESCR field: staging NULLs would silently wipe the column on merge.
    const char *narrow =
        "{\"Name\":\"RID\",\"Type\":\"CHAR10\",\"Kind\":\"C\",\"Length\":10,\"Decimals\":0},"
        "{\"Name\":\"AMOUNT\",\"Type\":\"S_PRICE\",\"Kind\":\"P\",\"Length\":8,\"Decimals\":2}";
    try {
        ApeApplyFullPackage(db, "t", Pkg(narrow, "1,10.00\\n", false), 0);
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("DESCR"));
    }
}

TEST_CASE("ape_cycle: control envelope with lastBatch ends an empty scan",
          "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    auto a = ApeApplyFullPackage(
        db, "t",
        "{\"Encoding\":\"csv\",\"Attributes\":{\"message.batchIndex\":0,"
        "\"message.lastBatch\":true}}",
        0);
    CHECK(a.rows_staged == 0);
    CHECK(a.last_batch);
    // An empty FULL scan reconciles to an empty target (SNAPSHOT semantics).
    auto c = ApeFinalizeFull(db, "t", {"rid"});
    CHECK(c.del == 3);
    CHECK(Cell(db, "SELECT count(*) AS c FROM t") == R"({"c":0})");
}

TEST_CASE("ape_cycle: packages accumulate across batches", "[ape]") {
    erpl_rev::DuckDbBridge db;
    db.Execute("CREATE TABLE t(rid VARCHAR PRIMARY KEY, descr VARCHAR, "
               "amount DECIMAL(10,2))");
    CHECK(ApeApplyFullPackage(db, "t", Pkg(kFields, "1,a,1.00,\\n", false), 0)
              .rows_staged == 1);
    auto a = ApeApplyFullPackage(
        db, "t", Pkg(kFields, "2,b,2.00,\\n3,c,3.00,\\n", true), 1);
    CHECK(a.rows_staged == 2);
    CHECK(a.last_batch);
    auto c = ApeFinalizeFull(db, "t", {"rid"});
    CHECK(c.ins == 3);
    CHECK(Cell(db, "SELECT count(*) AS c FROM t") == R"({"c":3})");
}

TEST_CASE("ape_cycle: unknown target is refused with its name", "[ape]") {
    erpl_rev::DuckDbBridge db;
    try {
        ApeApplyFullPackage(db, "nope", Pkg(kFields, "1,a,1.00,\\n", false), 0);
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("nope"));
    }
}

namespace {

// Minimal prod-shaped state for DELTA tests: the registry row plus the
// package spill (v9). Same table/column names as the migration owns.
void MakeDeltaState(erpl_rev::DuckDbBridge &db, const std::string &target) {
    // Every bridge ctor migrates the full prod schema (v9 owns the spill
    // table and spill_batch): seed the registration row, nothing else.
    db.Execute("INSERT INTO _erpl_rev_delta_state(target, method, source_from, keys, "
               "spill_batch) VALUES ('" + target + "', 'APE_DELTA', 'ZERPL_APE_D', 'RID', -1)");
}

long long Spilled(erpl_rev::DuckDbBridge &db, const std::string &target) {
    auto r = db.Query("SELECT count(*) AS c FROM _erpl_rev_ape_spill WHERE target='" +
                      target + "'");
    REQUIRE(r.row_count == 1);
    return std::atoll(r.rows[0].c_str() + r.rows[0].find_last_of(":\"") + 1);
}

}  // namespace

TEST_CASE("ape_cycle: DELTA merges upserts and deletes by operation", "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    MakeDeltaState(db, "t");
    // id=1 updated, id=3 re-upserted (U = after-image), id=2 deleted (D,
    // keys-only image). Change semantics per protocol §13.
    auto c = ApeApplyDeltaPackage(
        db, "t", {"rid"},
        Pkg(kFields, "1,A,10.00,U\\n3,c,30.00,U\\n2,,,D\\n", false), 0);
    CHECK(c.rows_applied == 3);
    CHECK(c.upserted == 2);
    CHECK(c.deleted == 1);
    CHECK(Cell(db, "SELECT descr AS d FROM t WHERE rid='1'") == R"({"d":"A"})");
    CHECK(Cell(db, "SELECT count(*) AS c FROM t WHERE rid='2'") == R"({"c":0})");
    CHECK(Cell(db, "SELECT count(*) AS c FROM t") == R"({"c":2})");
    // Spilled before merging: the crash copy survives the merge.
    CHECK(Spilled(db, "t") == 1);
}

TEST_CASE("ape_cycle: DELTA treats a blank operation as an after-image",
          "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    MakeDeltaState(db, "t");
    // The engine would never send this (replication always carries U/D),
    // but a blank must upsert (BR-3), never silently skip.
    auto c = ApeApplyDeltaPackage(
        db, "t", {"rid"}, Pkg(kFields, "9,nine,9.00,\\n", false), 0);
    CHECK(c.upserted == 1);
    CHECK(Cell(db, "SELECT count(*) AS c FROM t WHERE rid='9'") == R"({"c":1})");
}

TEST_CASE("ape_cycle: DELTA without an operation column is refused", "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    MakeDeltaState(db, "t");
    const char *no_op =
        "{\"Name\":\"RID\",\"Type\":\"CHAR10\",\"Kind\":\"C\",\"Length\":10,\"Decimals\":0},"
        "{\"Name\":\"DESCR\",\"Type\":\"CHAR40\",\"Kind\":\"C\",\"Length\":40,\"Decimals\":0}";
    try {
        ApeApplyDeltaPackage(db, "t", {"rid"}, Pkg(no_op, "1,a\\n", false), 0);
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("OPERATION"));
    }
    CHECK(Spilled(db, "t") == 0);
}

TEST_CASE("ape_cycle: recover replays a spilled batch after a crash", "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    MakeDeltaState(db, "t");
    // Batch 0 applied cleanly (spilled + merged, position advanced).
    CHECK(ApeApplyDeltaPackage(db, "t", {"rid"},
                               Pkg(kFields, "1,A,10.00,U\\n", false), 0)
              .rows_applied == 1);
    // Crash between spill and merge of batch 1: the spill row exists, the
    // merge never ran, spill_batch still 0. (A real kill lands here; the
    // test plants the same row directly instead of killing the process.)
    db.Execute("INSERT INTO _erpl_rev_ape_spill VALUES ('t', 1, '" +
               std::string("x") + "', now())");
    db.Execute("UPDATE _erpl_rev_ape_spill SET payload='" +
               Pkg(kFields, "4,d,40.00,U\\n", false) +
               "' WHERE target='t' AND batch_index=1");
    auto c = ApeRecover(db, "t", {"rid"});
    CHECK(c.rows_applied == 1);
    CHECK(c.upserted == 1);
    CHECK(Cell(db, "SELECT count(*) AS c FROM t WHERE rid='4'") == R"({"c":1})");
    // Replayed and discarded: a second recover is a no-op, and the next
    // ordinary cycle resumes without re-delivery.
    CHECK(Spilled(db, "t") == 0);
    auto c2 = ApeRecover(db, "t", {"rid"});
    CHECK(c2.rows_applied == 0);
}

TEST_CASE("ape_cycle: a restarted batch counter is refused, never silently lost",
          "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    MakeDeltaState(db, "t");
    // Prior cycle merged batch 0 (spilled + merged, position 0).
    CHECK(ApeApplyDeltaPackage(db, "t", {"rid"},
                               Pkg(kFields, "1,A,10.00,U\\n", false), 0)
              .rows_applied == 1);
    // A new cycle that restarts its counter at 0 re-sends index 0 with new
    // content. Silently overwriting the spill and re-merging would strand a
    // kill between spill and merge: RECOVER replays batch_index >
    // spill_batch = 0, i.e. nothing. Refuse loudly instead.
    try {
        ApeApplyDeltaPackage(db, "t", {"rid"},
                             Pkg(kFields, "9,nine,9.00,U\\n", false), 0);
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("batch"));
    }
    CHECK(Cell(db, "SELECT count(*) AS c FROM t WHERE rid='9'") == R"({"c":0})");
}

TEST_CASE("ape_cycle: purge starts a new spill generation", "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    MakeDeltaState(db, "t");
    CHECK(ApeApplyDeltaPackage(db, "t", {"rid"},
                               Pkg(kFields, "1,A,10.00,U\\n", false), 0)
              .rows_applied == 1);
    CHECK(ApeApplyDeltaPackage(db, "t", {"rid"},
                               Pkg(kFields, "2,B,20.00,U\\n", false), 1)
              .rows_applied == 1);
    // Clean cycle end: the spill is garbage AND the position returns to -1,
    // so the next cycle counts 0, 1, ... again without colliding with the
    // generation the purge just discarded.
    ApePurge(db, "t");
    CHECK(Spilled(db, "t") == 0);
    CHECK(Cell(db, "SELECT spill_batch AS s FROM _erpl_rev_delta_state "
                   "WHERE target='t'") == R"({"s":-1})");
    // A kill between spill and merge of the new generation's batch 0 replays
    // it: position -1 < 0. (Planted directly, like the recover test above.)
    db.Execute("INSERT INTO _erpl_rev_ape_spill VALUES ('t', 0, 'x', now())");
    db.Execute("UPDATE _erpl_rev_ape_spill SET payload='" +
               Pkg(kFields, "9,nine,9.00,U\\n", false) +
               "' WHERE target='t' AND batch_index=0");
    auto c = ApeRecover(db, "t", {"rid"});
    CHECK(c.rows_applied == 1);
    CHECK(Cell(db, "SELECT count(*) AS c FROM t WHERE rid='9'") == R"({"c":1})");
}

TEST_CASE("ape_cycle: DELTA control envelopes spill nothing", "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeTarget(db);
    MakeDeltaState(db, "t");
    // An empty replication poll (no lastBatch ever comes): no spill, no
    // merge, no position advance.
    auto c = ApeApplyDeltaPackage(
        db, "t", {"rid"},
        "{\"Encoding\":\"csv\",\"Attributes\":{\"message.batchIndex\":4,"
        "\"message.lastBatch\":false}}",
        4);
    CHECK(c.rows_applied == 0);
    CHECK(Spilled(db, "t") == 0);
}

TEST_CASE("ape_cycle: DELTA on an unknown target is refused", "[ape]") {
    erpl_rev::DuckDbBridge db;
    MakeDeltaState(db, "nope");
    try {
        ApeApplyDeltaPackage(db, "missing", {"rid"},
                             Pkg(kFields, "1,a,1.00,U\\n", false), 0);
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("missing"));
    }
}
