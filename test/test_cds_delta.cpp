// Delta-enabled CDS views.
//
// A CDS view can declare how it should be extracted incrementally. Reading those
// annotations means a target configures itself instead of an operator restating
// what the model already says -- and getting it subtly wrong.
//
// The annotation VALUES arrive as data (an ABAP DDIC/annotation read), not as
// DDL source text. Parsing the source would need developer authorisations on the
// customer system, which is exactly the dependency the pre-deployed command
// driver exists to remove.

#include <catch2/catch_test_macros.hpp>

#include "cds_delta.hpp"

using namespace erpl_rev::cds;

TEST_CASE("cds_delta: byElement gives a watermark on that element", "[cds]") {
    Annotations a;
    a.entity = "ZERPL_C_FLIGHTS";
    a.values["Analytics.dataExtraction.enabled"] = "true";
    a.values["Analytics.dataExtraction.delta.byElement.name"] = "LAST_CHANGED_AT";
    a.element_types["LAST_CHANGED_AT"] = "TIMESTAMPL";

    const auto d = Derive(a);
    REQUIRE(d.kind == DeltaKind::Watermark);
    CHECK(d.chg_col == "LAST_CHANGED_AT");
    CHECK(d.wm_kind == "TIMESTAMPL");
    CHECK(d.trigger_targets.empty());
}

TEST_CASE("cds_delta: the element's DDIC type picks the watermark kind", "[cds]") {
    // Getting this wrong is the D4 bug all over again: a DATS element treated as
    // a timestamp silently compares as a number.
    Annotations a;
    a.entity = "V";
    a.values["Analytics.dataExtraction.delta.byElement.name"] = "ERDAT";
    a.element_types["ERDAT"] = "DATS";
    CHECK(Derive(a).wm_kind == "DATE");

    a.element_types["ERDAT"] = "DEC";
    CHECK(Derive(a).wm_kind == "INT");
}

TEST_CASE("cds_delta: a maximum delay becomes the safety window", "[cds]") {
    // The model states how late a record may arrive; that is exactly what the
    // safety offset is for, so it should not have to be configured twice.
    Annotations a;
    a.entity = "V";
    a.values["Analytics.dataExtraction.delta.byElement.name"] = "CHANGED_AT";
    a.values["Analytics.dataExtraction.delta.byElement.maxDelayInSeconds"] = "900";
    a.element_types["CHANGED_AT"] = "TIMESTAMPL";
    CHECK(Derive(a).safety_secs == 900);
}

TEST_CASE("cds_delta: changeDataCapture yields a trigger target per base table",
          "[cds]") {
    Annotations a;
    a.entity = "ZERPL_C_ORDERS";
    a.values["Analytics.dataExtraction.delta.changeDataCapture.automatic"] = "true";
    a.mappings = {{"VBAK", "MANDT,VBELN"}, {"VBAP", "MANDT,VBELN,POSNR"}};

    const auto d = Derive(a);
    REQUIRE(d.kind == DeltaKind::Trigger);
    REQUIRE(d.trigger_targets.size() == 2);
    CHECK(d.trigger_targets[0].table == "VBAK");
    CHECK(d.trigger_targets[0].keys == "MANDT,VBELN");
    // KEYS_IUD, because the values are re-read through the VIEW -- the whole
    // point of a CDS delta is that the view's projection is what lands.
    CHECK(d.trigger_targets[0].mode == "KEYS_IUD");
    CHECK(d.trigger_targets[1].table == "VBAP");
}

TEST_CASE("cds_delta: a capture mapping with no base tables is refused", "[cds]") {
    // Silently registering nothing would look like a working delta-enabled view
    // that never delivers a change.
    Annotations a;
    a.entity = "V";
    a.values["Analytics.dataExtraction.delta.changeDataCapture.automatic"] = "true";
    CHECK_THROWS(Derive(a));
}

TEST_CASE("cds_delta: a base table with no key mapping is refused", "[cds]") {
    // Triggers log keys; without knowing the key there is nothing to log and
    // nothing to re-read by.
    Annotations a;
    a.entity = "V";
    a.values["Analytics.dataExtraction.delta.changeDataCapture.automatic"] = "true";
    a.mappings = {{"VBAK", ""}};
    CHECK_THROWS(Derive(a));
}

TEST_CASE("cds_delta: a view with no delta annotation gets no delta", "[cds]") {
    // It is still replicable by full load or snapshot; it just does not
    // configure itself, and must not be guessed at.
    Annotations a;
    a.entity = "PLAIN_VIEW";
    const auto d = Derive(a);
    CHECK(d.kind == DeltaKind::None);
    CHECK(d.chg_col.empty());
}

TEST_CASE("cds_delta: extraction disabled is honoured", "[cds]") {
    // The model explicitly says not to extract this. Deriving a delta anyway
    // would override a decision someone made on purpose.
    Annotations a;
    a.entity = "V";
    a.values["Analytics.dataExtraction.enabled"] = "false";
    a.values["Analytics.dataExtraction.delta.byElement.name"] = "CHANGED_AT";
    a.element_types["CHANGED_AT"] = "TIMESTAMPL";
    CHECK_THROWS(Derive(a));
}

TEST_CASE("cds_delta: annotation names are matched case-insensitively", "[cds]") {
    // ADT and the DDIC APIs disagree about case; the same view must derive the
    // same delta either way.
    Annotations a;
    a.entity = "V";
    a.values["ANALYTICS.DATAEXTRACTION.DELTA.BYELEMENT.NAME"] = "CHANGED_AT";
    a.element_types["changed_at"] = "TIMESTAMPL";
    const auto d = Derive(a);
    CHECK(d.kind == DeltaKind::Watermark);
    CHECK(d.chg_col == "CHANGED_AT");
}
