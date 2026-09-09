// The coverage matrix: every replication variant, and where it is proven.
//
// WHY THIS FILE EXISTS
//
// KEYS_IUD reached main having never completed a single cycle on any system. It
// carried thirteen green unit tests and zero live coverage, and nothing anywhere
// said so -- no artefact listed "CdcMode has three variants, here is each one's
// coverage", so the gap was not something a reviewer could have noticed. The
// live CDC suite exercised DELETE_ONLY and IMAGE_IUD, which made the *tier* look
// finished while a *mode* inside it had nothing at all.
//
// You cannot review an absence that nothing enumerates. So this enumerates it.
//
// HOW IT BITES
//
// Each CoverageOf() is a switch with NO default label. Adding a variant to one
// of these enums is a COMPILE ERROR here until somebody writes down where it is
// tested -- which is the moment to notice that nowhere is the honest answer.
// The declarations are then checked against reality: a named unit tag has to
// appear in a test source, and a named live marker has to appear in e2e.sh, so
// a declaration cannot simply be asserted into existence.
//
// WHAT IT DOES NOT DO
//
// It cannot tell you a test is any GOOD. Thirteen of the tests it would have
// counted for KEYS_IUD were structurally incapable of failing, because every
// fixture used one type and the code dispatches on type. This gate catches
// "nothing tests this"; it does not catch "the test cannot fail". For that,
// ask of every fixture: if I deleted the branch under test, would this still
// pass? See test_cdc_apply_keys.cpp for what the answer has to look like.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cdc_dialect.hpp"
#include "delta_plan.hpp"
#include "duckdb_bridge.hpp"
#include "load_type.hpp"
#include "publish.hpp"
#include "split_planner.hpp"

using namespace erpl_rev;

namespace {

struct Coverage {
    const char *variant;       // what it is called in the docs and the CLI
    const char *unit_tag;      // a Catch2 tag exercising it, or "" with a reason
    const char *live_marker;   // an e2e suite marker exercising it, or "" with a reason
    const char *note;          // required when either is empty: why, and what would close it
};

// --- the declarations ------------------------------------------------------
// No `default:` anywhere below. That is the mechanism, not an oversight.

Coverage CoverageOf(CdcMode m) {
    switch (m) {
        case CdcMode::DeleteOnly:
            return {"DELETE_ONLY", "[cdc]", "CDC", ""};
        case CdcMode::KeysIud:
            return {"KEYS_IUD", "[keys]", "CDC", ""};
        case CdcMode::ImageIud:
            return {"IMAGE_IUD", "[keys]", "CDC", ""};
    }
    return {"", "", "", "unreachable"};
}

Coverage CoverageOf(LoadType t) {
    switch (t) {
        // The live arm is zcl_erpl_rev_wmtest's m4_load_types, under the WM
        // suite -- there is no separate LOADTYPE suite, whatever the plan called
        // it.
        case LoadType::Full:     return {"F", "[loadtype]", "WM", ""};
        case LoadType::InitOnly: return {"I", "[loadtype]", "WM", ""};
        // Every cycle in the WM suite is a D run; that is what makes it the
        // default rather than a variant needing its own arm.
        case LoadType::Delta:    return {"D", "[loadtype]", "WM", ""};
        case LoadType::InitAndFull:
            // FOUND BY THIS GATE, on its first run. m4_load_types exercises I
            // and F and stops. L seeds the watermark and THEN full-loads, and
            // it is one-shot -- spent through one_shot_spent once it has run --
            // so the interesting part is the interaction between the seed, the
            // load and the spend, and none of it is proven on a live system.
            // The CLI lane only checks that load_type_default='L' survives the
            // command queue, which is the field round-tripping, not the
            // behaviour happening.
            return {"L", "[loadtype]", "",
                    "no live arm: add an L case to zcl_erpl_rev_wmtest's "
                    "m4_load_types asserting the seed, the full load and that "
                    "one_shot_spent flips exactly once"};
    }
    return {"", "", "", "unreachable"};
}

Coverage CoverageOf(wm::WmKind k) {
    switch (k) {
        case wm::WmKind::Numts:      return {"NUMTS", "[watermark]", "WM", ""};
        case wm::WmKind::Timestampl: return {"TIMESTAMPL", "[watermark]", "WM", ""};
        case wm::WmKind::Date:       return {"DATE", "[watermark]", "WM", ""};
        case wm::WmKind::Datetime:   return {"DATETIME", "[watermark]", "WM", ""};
        case wm::WmKind::Int:        return {"INT", "[watermark]", "WM", ""};
        case wm::WmKind::Changenr:
            // Refused at registration on purpose -- a number-range-buffered key
            // is not monotonic in commit order, so an offset on it proves
            // nothing. The unit case pins the REFUSAL; there is deliberately no
            // live replication arm because there is deliberately no feature.
            return {"CHANGENR", "[watermark]", "",
                    "refused at registration by design; the unit case pins the refusal"};
    }
    return {"", "", "", "unreachable"};
}

Coverage CoverageOf(IngestMode m) {
    switch (m) {
        case IngestMode::Insert: return {"INSERT", "[bridge]", "SLT", ""};
        case IngestMode::Upsert: return {"UPSERT", "[bridge]", "SLT", ""};
        case IngestMode::Merge:  return {"MERGE", "[bridge]", "SLT", ""};
    }
    return {"", "", "", "unreachable"};
}

Coverage CoverageOf(split::Strategy s) {
    switch (s) {
        case split::Strategy::Key:     return {"key", "[split]", "PARTITION", ""};
        case split::Strategy::Records: return {"records", "[split]", "PARTITION", ""};
        case split::Strategy::Size:    return {"size", "[split]", "PARTITION", ""};
        case split::Strategy::Time:    return {"time", "[split]", "PARTITION", ""};
        case split::Strategy::Fiscal:  return {"fiscal", "[split]", "PARTITION", ""};
        case split::Strategy::List:    return {"list", "[split]", "PARTITION", ""};
    }
    return {"", "", "", "unreachable"};
}

Coverage CoverageOf(SinkKind k) {
    switch (k) {
        case SinkKind::Parquet: return {"parquet", "[publish]", "PUBTEST", ""};
        case SinkKind::Table:   return {"table", "[publish]", "PUBTEST", ""};
    }
    return {"", "", "", "unreachable"};
}

// --- checking the declarations against reality -----------------------------

std::string ReadFile(const std::filesystem::path &p) {
    std::ifstream in(p);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Every test source concatenated, so a declared unit tag can be looked for.
const std::string &TestSources() {
    static const std::string all = [] {
        std::string acc;
        const std::filesystem::path dir = std::filesystem::path(ERPL_REV_SOURCE_DIR) / "test";
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".cpp") acc += ReadFile(e.path());
        }
        return acc;
    }();
    return all;
}

const std::string &E2eScript() {
    static const std::string s =
        ReadFile(std::filesystem::path(ERPL_REV_SOURCE_DIR) / "scripts" / "e2e.sh");
    return s;
}

void Check(const Coverage &c) {
    INFO("variant: " << c.variant);
    REQUIRE_FALSE(std::string(c.variant).empty());

    // A variant with no unit tag AND no live marker is undeclared, not covered.
    const bool has_unit = !std::string(c.unit_tag).empty();
    const bool has_live = !std::string(c.live_marker).empty();
    INFO("unit tag: '" << c.unit_tag << "'  live marker: '" << c.live_marker << "'");
    CHECK((has_unit || has_live));

    // Anything missing has to be argued for, in writing, here.
    if (!has_unit || !has_live) {
        INFO("a variant without both tiers must say why");
        CHECK_FALSE(std::string(c.note).empty());
    }

    // ...and a declaration must not be fiction.
    if (has_unit) {
        INFO("no test source mentions the tag " << c.unit_tag);
        CHECK(TestSources().find(c.unit_tag) != std::string::npos);
    }
    if (has_live) {
        INFO("scripts/e2e.sh has no suite with marker " << c.live_marker);
        CHECK(E2eScript().find(c.live_marker) != std::string::npos);
    }
}

}  // namespace

TEST_CASE("coverage: every trigger-CDC mode is exercised", "[coverage]") {
    for (auto m : {CdcMode::DeleteOnly, CdcMode::KeysIud, CdcMode::ImageIud}) Check(CoverageOf(m));
}

TEST_CASE("coverage: every load type is exercised", "[coverage]") {
    for (auto t : {LoadType::Full, LoadType::InitOnly, LoadType::InitAndFull, LoadType::Delta})
        Check(CoverageOf(t));
}

TEST_CASE("coverage: every watermark kind is exercised", "[coverage]") {
    for (auto k : {wm::WmKind::Numts, wm::WmKind::Timestampl, wm::WmKind::Date, wm::WmKind::Datetime,
                   wm::WmKind::Int, wm::WmKind::Changenr})
        Check(CoverageOf(k));
}

TEST_CASE("coverage: every ingest mode is exercised", "[coverage]") {
    for (auto m : {IngestMode::Insert, IngestMode::Upsert, IngestMode::Merge})
        Check(CoverageOf(m));
}

TEST_CASE("coverage: every split strategy is exercised", "[coverage]") {
    for (auto s : {split::Strategy::Key, split::Strategy::Records, split::Strategy::Size,
                   split::Strategy::Time, split::Strategy::Fiscal, split::Strategy::List})
        Check(CoverageOf(s));
}

TEST_CASE("coverage: every publish sink is exercised", "[coverage]") {
    for (auto k : {SinkKind::Parquet, SinkKind::Table}) Check(CoverageOf(k));
}

TEST_CASE("coverage: the matrix can actually see the repo", "[coverage]") {
    // Without this, every lookup above silently succeeds against an empty
    // string -- a gate that passes because it is reading nothing is worse than
    // no gate, because it reports safety.
    CHECK(TestSources().size() > 1000);
    CHECK(E2eScript().find("suite ") != std::string::npos);
}
