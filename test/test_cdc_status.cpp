// Trigger status, derived rather than stored.
//
// The registry says which objects SHOULD exist; a catalogue probe says which do.
// The status is the comparison. A stored enum cannot notice a trigger dropped
// out of band -- by a system copy, a transport, or a DBA -- and that is exactly
// the case that matters, because a cycle running against a trigger set with a
// missing trigger advances its position past changes nothing captured.

#include <catch2/catch_test_macros.hpp>

#include "cdc_dialect.hpp"
#include "cdc_status.hpp"

using namespace erpl_rev::cdc;

namespace {
Expected Set() {
    Expected e;
    e.log_table = "ZCDC_SFLIGHT_LOG";
    e.seq_name = "ZCDC_SFLIGHT_SEQ";
    e.triggers = {"ZCDC_SFLIGHT_I", "ZCDC_SFLIGHT_U", "ZCDC_SFLIGHT_D"};
    return e;
}
}  // namespace

TEST_CASE("cdc_status: everything present is ACTIVE", "[cdcstatus]") {
    Probe p;
    p.tables = {"ZCDC_SFLIGHT_LOG"};
    p.sequences = {"ZCDC_SFLIGHT_SEQ"};
    p.enabled_triggers = {"ZCDC_SFLIGHT_I", "ZCDC_SFLIGHT_U", "ZCDC_SFLIGHT_D"};

    const auto s = Derive(Set(), p, /*deactivated=*/false);
    CHECK(s.status == Status::Active);
    CHECK(s.missing.empty());
}

TEST_CASE("cdc_status: a missing trigger is INCONSISTENT and is NAMED", "[cdcstatus]") {
    // "Something is wrong" is not actionable. The object is.
    Probe p;
    p.tables = {"ZCDC_SFLIGHT_LOG"};
    p.sequences = {"ZCDC_SFLIGHT_SEQ"};
    p.enabled_triggers = {"ZCDC_SFLIGHT_I", "ZCDC_SFLIGHT_D"};   // U was dropped

    const auto s = Derive(Set(), p, false);
    CHECK(s.status == Status::Inconsistent);
    REQUIRE(s.missing.size() == 1);
    CHECK(s.missing[0] == "ZCDC_SFLIGHT_U");
    CHECK(Explain(s).find("ZCDC_SFLIGHT_U") != std::string::npos);
}

TEST_CASE("cdc_status: a disabled trigger counts as missing", "[cdcstatus]") {
    // It exists in the catalogue and captures nothing, which is the worst of
    // both: present enough to look healthy, inert in fact.
    Probe p;
    p.tables = {"ZCDC_SFLIGHT_LOG"};
    p.sequences = {"ZCDC_SFLIGHT_SEQ"};
    p.enabled_triggers = {"ZCDC_SFLIGHT_I", "ZCDC_SFLIGHT_D"};
    p.disabled_triggers = {"ZCDC_SFLIGHT_U"};

    const auto s = Derive(Set(), p, false);
    CHECK(s.status == Status::Inconsistent);
    CHECK(s.missing[0] == "ZCDC_SFLIGHT_U");
}

TEST_CASE("cdc_status: a missing shadow table or sequence is INCONSISTENT",
          "[cdcstatus]") {
    Probe p;
    p.sequences = {"ZCDC_SFLIGHT_SEQ"};
    p.enabled_triggers = {"ZCDC_SFLIGHT_I", "ZCDC_SFLIGHT_U", "ZCDC_SFLIGHT_D"};
    CHECK(Derive(Set(), p, false).status == Status::Inconsistent);

    Probe q;
    q.tables = {"ZCDC_SFLIGHT_LOG"};
    q.enabled_triggers = {"ZCDC_SFLIGHT_I", "ZCDC_SFLIGHT_U", "ZCDC_SFLIGHT_D"};
    CHECK(Derive(Set(), q, false).status == Status::Inconsistent);
}

TEST_CASE("cdc_status: deliberately deactivated is DISABLED, not INCONSISTENT",
          "[cdcstatus]") {
    // Mass deactivation before an upgrade removes the triggers on purpose. That
    // must not read as breakage, or every upgrade raises a false alarm.
    Probe p;
    p.tables = {"ZCDC_SFLIGHT_LOG"};       // kept: the position lives with it
    p.sequences = {"ZCDC_SFLIGHT_SEQ"};

    const auto s = Derive(Set(), p, /*deactivated=*/true);
    CHECK(s.status == Status::Disabled);
}

TEST_CASE("cdc_status: a repair plan contains only what is missing", "[cdcstatus]") {
    // Re-running the whole provision DDL would recreate the shadow table and
    // reset the position, throwing away captured changes.
    Probe p;
    p.tables = {"ZCDC_SFLIGHT_LOG"};
    p.sequences = {"ZCDC_SFLIGHT_SEQ"};
    p.enabled_triggers = {"ZCDC_SFLIGHT_I"};

    const auto s = Derive(Set(), p, false);
    const std::vector<std::string> all = {
        "CREATE SEQUENCE ZCDC_SFLIGHT_SEQ ...",
        "CREATE TABLE ZCDC_SFLIGHT_LOG ...",
        "CREATE TRIGGER ZCDC_SFLIGHT_I ...",
        "CREATE TRIGGER ZCDC_SFLIGHT_U ...",
        "CREATE TRIGGER ZCDC_SFLIGHT_D ...",
    };
    const auto plan = RepairPlan(s, all);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].find("ZCDC_SFLIGHT_U") != std::string::npos);
    CHECK(plan[1].find("ZCDC_SFLIGHT_D") != std::string::npos);
    for (const auto &stmt : plan) {
        CHECK(stmt.find("CREATE TABLE") == std::string::npos);
        CHECK(stmt.find("CREATE SEQUENCE") == std::string::npos);
    }
}

TEST_CASE("cdc_status: orphans are objects the registry does not know", "[cdcstatus]") {
    // Left behind by a target that was removed, or by a system copy. They keep
    // firing and filling a shadow table nobody drains.
    Probe p;
    p.tables = {"ZCDC_SFLIGHT_LOG", "ZCDC_GONE_LOG"};
    p.sequences = {"ZCDC_SFLIGHT_SEQ", "ZCDC_GONE_SEQ"};
    p.enabled_triggers = {"ZCDC_SFLIGHT_I", "ZCDC_SFLIGHT_U", "ZCDC_SFLIGHT_D",
                          "ZCDC_GONE_I"};

    const auto orphans = FindOrphans({Set()}, p);
    CHECK(orphans.size() == 3);
    for (const auto &o : orphans) CHECK(o.find("GONE") != std::string::npos);
}

TEST_CASE("cdc_status: only ZCDC_ objects are ever considered orphans",
          "[cdcstatus]") {
    // The probe sees the whole schema. Dropping a customer object because it was
    // not in our registry would be catastrophic and entirely our fault.
    Probe p;
    p.tables = {"ZCDC_SFLIGHT_LOG", "MARA", "ZCUSTOMER_DATA"};
    p.sequences = {"ZCDC_SFLIGHT_SEQ"};
    p.enabled_triggers = {"ZCDC_SFLIGHT_I", "ZCDC_SFLIGHT_U", "ZCDC_SFLIGHT_D"};

    CHECK(FindOrphans({Set()}, p).empty());
}

TEST_CASE("cdc_status: the HANA probe only ever looks at ZCDC_ objects",
          "[cdcstatus]") {
    // The probe runs against a customer's production schema. It must be
    // impossible for it to return, and therefore for orphan cleanup to consider,
    // anything outside our namespace.
    erpl_rev::HanaDialect d;
    for (const auto &sql : {d.ProbeTablesSql(), d.ProbeSequencesSql(), d.ProbeTriggersSql()}) {
        CHECK(sql.find("ZCDC") != std::string::npos);
        CHECK(sql.find("CURRENT_SCHEMA") != std::string::npos);
    }
    // An invalid trigger has to be distinguishable from a valid one.
    CHECK(d.ProbeTriggersSql().find("IS_VALID") != std::string::npos);
}

TEST_CASE("cdc_status: AnyDB refuses to probe, as it refuses to plan", "[cdcstatus]") {
    erpl_rev::AnyDbDialect d;
    CHECK_THROWS(d.ProbeTablesSql());
    CHECK_THROWS(d.Plan({}));
}
