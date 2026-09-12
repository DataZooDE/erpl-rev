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
        // read_from casts _SEQ to INTEGER and _TS to text for the ABAP ADBC
        // reader, which chokes on HANA's native BIGINT and TIMESTAMP host types.
        // _TS is carried, not dropped: it is when the change COMMITTED in SAP, so
        // it is the only thing that makes trigger-tier latency measurable rather
        // than merely estimated from cycle durations.
        REQUIRE(p.read_from ==
                R"((SELECT "MANDT","CARRID","CONNID","FLDATE","_OP",)"
                R"(CAST("_SEQ" AS INTEGER) AS "_SEQ",)"
                R"(TO_VARCHAR("_TS", 'YYYYMMDDHH24MISS') AS "_TS" )"
                R"(FROM "ZCDC_SFLIGHT_LOG") AS LOGREAD)");
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
    spec.mode = CdcMode::ImageIud;
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
    spec.mode = CdcMode::ImageIud;
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
    spec.mode = CdcMode::ImageIud;
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

// ---------------------------------------------------------------------------
// KEYS_IUD. NOT the default -- DELETE_ONLY is (control_schema.cpp's v3
    // migration, and every provisioning entry point). KEYS_IUD is what a caller
    // asks for when it wants inserts and updates as well as deletes.
//
// The shadow log carries key + op + sequence only; the cycle re-reads the source
// for the row values. Cheaper on the write path of a wide hot table than logging
// a full row image on every change, which is what IMAGE_IUD (the old FULL_IUD)
// does and remains available for.
// ---------------------------------------------------------------------------

TEST_CASE("cdc_dialect: KEYS_IUD logs keys only but triggers on all three DML", "[cdc][keys]") {
    HanaDialect d;
    CdcSpec s;
    s.source = "SFLIGHT";
    s.keys = {"MANDT", "CARRID", "CONNID", "FLDATE"};
    s.columns = {"MANDT", "CARRID", "CONNID", "FLDATE", "PRICE", "SEATSMAX"};
    s.mode = CdcMode::KeysIud;
    const auto p = d.Plan(s);

    // Keys only -- this is the whole point of the mode.
    CHECK(p.log_cols == s.keys);
    // ...but it must still fire on inserts and updates, unlike DELETE_ONLY.
    CHECK(p.trigger_names.size() == 3);
}

TEST_CASE("cdc_dialect: IMAGE_IUD still logs the whole row", "[cdc][keys]") {
    HanaDialect d;
    CdcSpec s;
    s.source = "SFLIGHT";
    s.keys = {"MANDT", "CARRID"};
    s.columns = {"MANDT", "CARRID", "PRICE"};
    s.mode = CdcMode::ImageIud;
    const auto p = d.Plan(s);
    CHECK(p.log_cols == s.columns);
    CHECK(p.trigger_names.size() == 3);
}

TEST_CASE("cdc_dialect: the three modes differ only where they should", "[cdc][keys]") {
    // A regression guard for the rename: DELETE_ONLY and KEYS_IUD log the same
    // columns and differ in how many triggers they install; KEYS_IUD and
    // IMAGE_IUD install the same triggers and differ in what they log.
    HanaDialect d;
    CdcSpec base;
    base.source = "T";
    base.keys = {"ID"};
    base.columns = {"ID", "V"};

    auto del = base; del.mode = CdcMode::DeleteOnly;
    auto key = base; key.mode = CdcMode::KeysIud;
    auto img = base; img.mode = CdcMode::ImageIud;

    const auto pd = d.Plan(del), pk = d.Plan(key), pi = d.Plan(img);

    CHECK(pd.log_cols == pk.log_cols);
    CHECK(pd.trigger_names.size() == 1);
    CHECK(pk.trigger_names.size() == pi.trigger_names.size());
    CHECK(pk.log_cols != pi.log_cols);
}

TEST_CASE("cdc_dialect: KEYS_IUD emits the net-key query, other modes do not",
          "[cdc][keys]") {
    // Coalescing to a net op per key belongs on the server: the ABAP executor
    // should never have to reason about what a batch of interleaved changes
    // means, and a key whose net op is a delete must not be re-read at all.
    HanaDialect d;
    CdcSpec s;
    s.source = "SFLIGHT";
    s.keys = {"MANDT", "CARRID"};
    s.columns = {"MANDT", "CARRID", "PRICE"};

    s.mode = CdcMode::KeysIud;
    const auto k = d.Plan(s);
    REQUIRE_FALSE(k.netkeys_sql.empty());
    CHECK(k.netkeys_sql.find("row_number()") != std::string::npos);
    CHECK(k.netkeys_sql.find("mandt,carrid") != std::string::npos);
    // Only net inserts and updates -- a net delete has nothing to re-read.
    CHECK(k.netkeys_sql.find("('i','u')") != std::string::npos);

    // The DuckDB staging table is a PER-CYCLE caller concern, like %POS% and
    // %CONF%, so it travels as a placeholder. It used to be rendered here as
    // "<log_table>__cdclog" while ABAP staged into "<target>__cdclog": the two
    // never agreed, so every KEYS_IUD cycle failed on a table that did not
    // exist. Nothing caught it because no test exercised KEYS_IUD end to end.
    CHECK(k.netkeys_sql.find("%STG%") != std::string::npos);
    CHECK(k.netkeys_sql.find("ZCDC_SFLIGHT_LOG__cdclog") == std::string::npos);

    s.mode = CdcMode::ImageIud;
    CHECK(d.Plan(s).netkeys_sql.empty());
    s.mode = CdcMode::DeleteOnly;
    CHECK(d.Plan(s).netkeys_sql.empty());
}

// A binary column cannot share the others' NVARCHAR log column. HANA does not
// render bytes as text on the way in -- it passes them through and NVARCHAR
// rejects them ("invalid CESU-8 encoding for Unicode string"), which fails the
// trigger, and a failed AFTER trigger fails the INSERT that fired it. The
// customer's own table then refuses every write until the triggers are dropped.
//
// Verified against a live HANA before these were written, with plain DDL and no
// erpl-rev in the picture: NVARCHAR log column -> the INSERT fails; VARBINARY
// log column -> it succeeds and BINTOHEX reads it back as uppercase hex.
TEST_CASE("cdc_dialect: a binary column gets a binary log column", "[cdc][keys]") {
    CdcSpec s;
    s.source = "ZDELTA_WM";
    s.keys = {"CLIENT", "ID"};
    s.columns = {"CLIENT", "ID", "NAME", "XGUID"};
    s.binary_columns = {"XGUID"};
    s.mode = CdcMode::ImageIud;
    const CdcPlan p = HanaDialect().Plan(s);

    std::string ddl;
    for (const auto &d : p.provision_ddl)
        if (d.find("CREATE COLUMN TABLE") != std::string::npos) ddl = d;
    REQUIRE_FALSE(ddl.empty());
    CHECK(Contains(ddl, "\"XGUID\" VARBINARY"));
    // and the text columns are untouched
    CHECK(Contains(ddl, "\"NAME\" NVARCHAR"));
    CHECK_FALSE(Contains(ddl, "\"XGUID\" NVARCHAR"));
}

TEST_CASE("cdc_dialect: a binary column is hex-encoded on the way out", "[cdc][keys]") {
    // Everything between the log and the server is text: the ABAP ADBC reader
    // binds a string and the BXML payload carries characters. A VARBINARY column
    // that is not encoded cannot cross that, so both read paths wrap it.
    CdcSpec s;
    s.source = "ZDELTA_WM";
    s.keys = {"CLIENT", "ID"};
    s.columns = {"CLIENT", "ID", "NAME", "XGUID"};
    s.binary_columns = {"XGUID"};
    s.mode = CdcMode::ImageIud;
    const CdcPlan p = HanaDialect().Plan(s);

    CHECK(Contains(p.read_sql, "BINTOHEX(\"XGUID\")"));
    CHECK(Contains(p.read_from, "BINTOHEX(\"XGUID\")"));
    // A text column must NOT be wrapped -- BINTOHEX of an NVARCHAR is not the
    // identity, and wrapping everything would corrupt every other column.
    CHECK_FALSE(Contains(p.read_sql, "BINTOHEX(\"NAME\")"));
}

TEST_CASE("cdc_dialect: a binary KEY column is covered in every mode", "[cdc][keys]") {
    // DELETE_ONLY and KEYS_IUD log the keys only -- so a table keyed on a RAW
    // column breaks their triggers exactly as an image column breaks IMAGE_IUD's.
    // Scoping the binary handling to IMAGE_IUD would leave that whole case broken.
    for (auto mode : {CdcMode::DeleteOnly, CdcMode::KeysIud}) {
        CdcSpec s;
        s.source = "ZGUIDKEY";
        s.keys = {"CLIENT", "GUID"};
        s.binary_columns = {"GUID"};
        s.mode = mode;
        const CdcPlan p = HanaDialect().Plan(s);

        std::string ddl;
        for (const auto &d : p.provision_ddl)
            if (d.find("CREATE COLUMN TABLE") != std::string::npos) ddl = d;
        INFO("mode " << static_cast<int>(mode));
        CHECK(Contains(ddl, "\"GUID\" VARBINARY"));
        CHECK(Contains(p.read_from, "BINTOHEX(\"GUID\")"));
    }
}
