#include <catch2/catch_test_macros.hpp>

#include "cdc_dialect.hpp"

#include <string>

using namespace erpl_rev;

namespace {
bool Contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}
// Find the one provision/teardown statement that contains a marker.
std::string Find(const std::vector<std::string> &v, const std::string &marker) {
    for (const auto &s : v)
        if (Contains(s, marker)) return s;
    return {};
}
}  // namespace

TEST_CASE("CDC dialect: HANA delete-only plan", "[cdc][dialect]") {
    HanaDialect d;
    CdcSpec spec;
    spec.source = "SFLIGHT";
    spec.keys = {"MANDT", "CARRID", "CONNID", "FLDATE"};
    spec.mode = CdcMode::DeleteOnly;
    CdcPlan p = d.Plan(spec);

    SECTION("names are in the customer namespace, derived from the source") {
        REQUIRE(p.log_table == "ZCDC_SFLIGHT_LOG");
        REQUIRE(p.seq_name == "ZCDC_SFLIGHT_SEQ");
        REQUIRE(p.trigger_names.size() == 1);
        REQUIRE(p.trigger_names[0] == "ZCDC_SFLIGHT_D");
    }
    SECTION("provision: sequence, log table with key+op+seq+ts, one AFTER DELETE trigger") {
        REQUIRE(p.provision_ddl.size() == 3);
        REQUIRE(Contains(p.provision_ddl[0], R"(CREATE SEQUENCE "ZCDC_SFLIGHT_SEQ")"));
        const std::string &tbl = p.provision_ddl[1];
        REQUIRE(Contains(tbl, R"(CREATE COLUMN TABLE "ZCDC_SFLIGHT_LOG")"));
        REQUIRE(Contains(tbl, R"("MANDT" NVARCHAR(255))"));
        REQUIRE(Contains(tbl, R"("FLDATE" NVARCHAR(255))"));
        REQUIRE(Contains(tbl, R"("_OP" NVARCHAR(1))"));
        REQUIRE(Contains(tbl, R"("_SEQ" BIGINT)"));
        REQUIRE(Contains(tbl, R"("_TS" TIMESTAMP)"));
        const std::string &trg = p.provision_ddl[2];
        REQUIRE(Contains(trg, R"(CREATE TRIGGER "ZCDC_SFLIGHT_D" AFTER DELETE ON "SFLIGHT")"));
        REQUIRE(Contains(trg, "REFERENCING OLD ROW AS oldr FOR EACH ROW"));
        REQUIRE(Contains(trg, R"(:oldr."CARRID")"));
        REQUIRE(Contains(trg, R"('D',"ZCDC_SFLIGHT_SEQ".NEXTVAL,CURRENT_TIMESTAMP)"));
    }
    SECTION("read is position-incremental and ordered; prune is bounded") {
        REQUIRE(p.read_sql ==
                R"(SELECT "MANDT","CARRID","CONNID","FLDATE","_OP","_SEQ" )"
                R"(FROM "ZCDC_SFLIGHT_LOG" WHERE "_SEQ" > %POS% ORDER BY "_SEQ")");
        REQUIRE(p.prune_sql == R"(DELETE FROM "ZCDC_SFLIGHT_LOG" WHERE "_SEQ" <= %CONF%)");
        // read_from excludes _TS and casts _SEQ to INTEGER for the ABAP ADBC reader.
        REQUIRE(p.read_from ==
                R"((SELECT "MANDT","CARRID","CONNID","FLDATE","_OP",)"
                R"(CAST("_SEQ" AS INTEGER) AS "_SEQ" FROM "ZCDC_SFLIGHT_LOG") AS LOGREAD)");
    }
    SECTION("teardown drops exactly what provision created") {
        REQUIRE(p.teardown_ddl.size() == 3);
        REQUIRE(p.teardown_ddl[0] == R"(DROP TRIGGER "ZCDC_SFLIGHT_D")");
        REQUIRE(p.teardown_ddl[1] == R"(DROP TABLE "ZCDC_SFLIGHT_LOG")");
        REQUIRE(p.teardown_ddl[2] == R"(DROP SEQUENCE "ZCDC_SFLIGHT_SEQ")");
    }
}

TEST_CASE("CDC dialect: HANA full-IUD plan has three triggers", "[cdc][dialect]") {
    HanaDialect d;
    CdcSpec spec;
    spec.source = "MARA";
    spec.keys = {"MANDT", "MATNR"};
    spec.mode = CdcMode::FullIud;
    CdcPlan p = d.Plan(spec);

    REQUIRE(p.trigger_names.size() == 3);
    // delete + insert + update, each tagging the right op and the right row image.
    REQUIRE(Contains(Find(p.provision_ddl, "AFTER DELETE"), R"(REFERENCING OLD ROW AS oldr)"));
    const std::string ins = Find(p.provision_ddl, "AFTER INSERT");
    REQUIRE(Contains(ins, R"(REFERENCING NEW ROW AS newr)"));
    REQUIRE(Contains(ins, R"('I',)"));
    const std::string upd = Find(p.provision_ddl, "AFTER UPDATE");
    REQUIRE(Contains(upd, R"(REFERENCING NEW ROW AS newr)"));
    REQUIRE(Contains(upd, R"('U',)"));
    // teardown drops all three triggers + table + sequence.
    REQUIRE(p.teardown_ddl.size() == 5);
}

TEST_CASE("CDC dialect: full-IUD with columns logs the full row image", "[cdc][dialect]") {
    HanaDialect d;
    CdcSpec spec;
    spec.source = "ZDELTA_WM";
    spec.keys = {"CLIENT", "ID"};
    spec.columns = {"CLIENT", "ID", "NAME", "VAL", "CHANGED_AT"};
    spec.mode = CdcMode::FullIud;
    CdcPlan p = d.Plan(spec);
    // the log table + insert trigger + read carry the data columns, not just keys.
    REQUIRE(Contains(p.provision_ddl[1], R"("NAME" NVARCHAR)"));
    REQUIRE(Contains(p.provision_ddl[1], R"("VAL" NVARCHAR)"));
    const std::string ins = Find(p.provision_ddl, "AFTER INSERT");
    REQUIRE(Contains(ins, R"(:newr."NAME")"));
    REQUIRE(Contains(ins, R"(:newr."CHANGED_AT")"));
    REQUIRE(Contains(p.read_from, R"("NAME","VAL","CHANGED_AT")"));
}

TEST_CASE("CDC dialect: name overrides + custom key length", "[cdc][dialect]") {
    HanaDialect d;
    CdcSpec spec;
    spec.source = "/BIC/AZZ";
    spec.keys = {"K"};
    spec.log_table = "ZMY_LOG";
    spec.seq_name = "ZMY_SEQ";
    spec.key_len = 90;
    CdcPlan p = d.Plan(spec);
    REQUIRE(p.log_table == "ZMY_LOG");
    REQUIRE(p.seq_name == "ZMY_SEQ");
    REQUIRE(Contains(p.provision_ddl[1], R"("K" NVARCHAR(90))"));
    // default trigger prefix still derives from the (sanitised) source.
    REQUIRE(p.trigger_names[0] == "ZCDC__BIC_AZZ_D");
}

TEST_CASE("CDC dialect: all generated objects are in the customer namespace", "[cdc][dialect]") {
    HanaDialect d;
    CdcSpec spec;
    spec.source = "SFLIGHT";
    spec.keys = {"MANDT", "CARRID"};
    spec.mode = CdcMode::FullIud;
    spec.columns = {"MANDT", "CARRID", "CONNID"};
    CdcPlan p = d.Plan(spec);
    // The log table, sequence and every trigger live in the customer Z-namespace, so
    // provisioning can never create or drop a SAP-owned object (ADR-0004 compliance).
    REQUIRE(p.log_table.rfind("ZCDC_", 0) == 0);
    REQUIRE(p.seq_name.rfind("ZCDC_", 0) == 0);
    for (const auto &t : p.trigger_names) REQUIRE(t.rfind("ZCDC_", 0) == 0);
}

TEST_CASE("CDC dialect: AnyDB refuses in v1; bad spec throws", "[cdc][dialect]") {
    AnyDbDialect any;
    CdcSpec spec;
    spec.source = "T000";
    spec.keys = {"MANDT"};
    REQUIRE_THROWS(any.Plan(spec));
    REQUIRE(MakeDialect("HANA")->Name() == "HANA");
    REQUIRE(MakeDialect("ORACLE")->Name() == "ANYDB");

    HanaDialect d;
    CdcSpec nokeys;
    nokeys.source = "X";
    REQUIRE_THROWS(d.Plan(nokeys));        // no keys
    CdcSpec nosrc;
    nosrc.keys = {"K"};
    REQUIRE_THROWS(d.Plan(nosrc));         // no source
}
