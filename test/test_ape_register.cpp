// APE registration validation (BRD FR-1/FR-2, NFR-2; HLD ADR-1/ADR-4).
//
// Pure and SAP-free: every refusal here must fire before anything contacts
// SAP, so a typo costs an error, never a prepared graph. The live probe
// (FR-2 capability checks) belongs to Phase 1 GREEN's second half; this file
// pins the static rules.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "abap_skeletons.hpp"
#include "ape_validate.hpp"

using namespace erpl_rev::ape;
using Catch::Matchers::ContainsSubstring;

namespace {

ApeRegistration ValidDelta() {
    ApeRegistration r;
    r.method = "APE_DELTA";
    r.source = "ZERPL_APE_D";
    r.keys = "MANDT,ID";
    r.subscriber_process = "ERPLREV01";
    r.cadence = "hourly";
    return r;
}

}  // namespace

TEST_CASE("ape_register: a valid delta registration passes", "[ape]") {
    CHECK(ValidateRegistration(ValidDelta()).empty());
}

TEST_CASE("ape_register: a valid full registration passes", "[ape]") {
    auto r = ValidDelta();
    r.method = "APE_FULL";
    CHECK(ValidateRegistration(r).empty());
}

TEST_CASE("ape_register: unknown methods are not APE", "[ape]") {
    CHECK_FALSE(IsApeMethod("WATERMARK"));
    CHECK_FALSE(IsApeMethod("SNAPSHOT"));
    CHECK_FALSE(IsApeMethod("CDC"));
    CHECK_FALSE(IsApeMethod("ape_delta"));
    CHECK(IsApeMethod("APE_FULL"));
    CHECK(IsApeMethod("APE_DELTA"));
}

TEST_CASE("ape_register: delta without a subscriber process is refused", "[ape]") {
    auto r = ValidDelta();
    r.subscriber_process.clear();
    const auto err = ValidateRegistration(r);
    CHECK_FALSE(err.empty());
    CHECK(err.find("subscriber") != std::string::npos);
}

TEST_CASE("ape_register: an overlong subscriber process is refused", "[ape]") {
    // Inherited from erpl_ape: at most 30 characters.
    auto r = ValidDelta();
    r.subscriber_process = std::string(31, 'X');
    CHECK_FALSE(ValidateRegistration(r).empty());
    r.subscriber_process = std::string(30, 'X');
    CHECK(ValidateRegistration(r).empty());
}

TEST_CASE("ape_register: control characters in the name are refused", "[ape]") {
    auto r = ValidDelta();
    r.subscriber_process = "ERPL\nREV";
    CHECK_FALSE(ValidateRegistration(r).empty());
    r.subscriber_process = "ERPL\tREV";
    CHECK_FALSE(ValidateRegistration(r).empty());
}

TEST_CASE("ape_register: sub-minute cadences are refused for APE", "[ape]") {
    // NFR-2: preparation alone takes tens of seconds; the 2 s daemon tick is
    // for Open-SQL methods.
    auto r = ValidDelta();
    r.cadence = "micro:30";
    const auto err = ValidateRegistration(r);
    CHECK_FALSE(err.empty());
    CHECK(err.find("micro") != std::string::npos);
    r.cadence = "manual";
    CHECK(ValidateRegistration(r).empty());
}

TEST_CASE("ape_register: a filter is refused, never silently ignored", "[ape]") {
    // ADR-1: the v6 reader has no filter path; returning all rows under the
    // appearance of selection is the failure mode.
    auto r = ValidDelta();
    r.has_filter = true;
    const auto err = ValidateRegistration(r);
    CHECK_FALSE(err.empty());
    CHECK(err.find("filter") != std::string::npos);
}

TEST_CASE("ape_register: source and keys stay required", "[ape]") {
    auto r = ValidDelta();
    r.source.clear();
    CHECK_FALSE(ValidateRegistration(r).empty());
    r = ValidDelta();
    r.keys.clear();
    CHECK_FALSE(ValidateRegistration(r).empty());
}

TEST_CASE("ape_register: the register fields carry the APE knobs", "[ape]") {
    // One ordered list feeds both writers (generated ABAP and the queue), so
    // a knob missing here arrives empty at the server on both paths -- the
    // time_col failure mode, not a compile error.
    erpl_rev::abapgen::SyncState st;
    st.target = "tgt";
    st.method = "APE_DELTA";
    st.source_from = "ZERPL_APE_D";
    st.keys = "MANDT,ID";
    st.subscriber_process = "ERPLREV01";
    st.chunk_size = 20000;
    st.wireformat = "W";
    st.allow_unreleased = "true";
    st.columns = "RID,DESCR";
    const auto fs = erpl_rev::abapgen::RegisterFields(st);
    auto raw = [&](const std::string &name) {
        for (const auto &f : fs)
            if (f.name == name) return f.raw;
        return std::string("<missing>");
    };
    CHECK(raw("subscriber_process") == "ERPLREV01");
    CHECK(raw("chunk_size") == "20000");
    CHECK(raw("wireformat") == "W");
    CHECK(raw("allow_unreleased") == "true");
    CHECK(raw("columns") == "RID,DESCR");
    // ...and the pre-existing fields still travel alongside.
    CHECK(raw("method") == "APE_DELTA");
    CHECK(raw("keys") == "MANDT,ID");
}

TEST_CASE("ape_register: the generated ABAP names the APE knobs", "[ape]") {
    erpl_rev::abapgen::SyncState st;
    st.target = "tgt";
    st.method = "APE_DELTA";
    st.source_from = "ZERPL_APE_D";
    st.keys = "MANDT,ID";
    st.subscriber_process = "ERPLREV01";
    st.columns = "RID,DESCR";
    const auto src = erpl_rev::abapgen::RenderSyncRegister(st, "abcdef12");
    CHECK_THAT(src, ContainsSubstring("subscriber_process"));
    CHECK_THAT(src, ContainsSubstring("ERPLREV01"));
    CHECK_THAT(src, ContainsSubstring("columns"));
    CHECK_THAT(src, ContainsSubstring("RID,DESCR"));
}
