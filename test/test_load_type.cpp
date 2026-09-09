// The four load types a replication run can be started as.
//
// Only D (delta) and an implicit L (seed by running a full load, then
// registering) existed. The gap that mattered in practice was I: register a
// target at "now" and transfer nothing, for a table already populated from
// somewhere else -- a restore, a migration, a parquet drop.

#include <catch2/catch_test_macros.hpp>

#include "load_type.hpp"

using namespace erpl_rev;

TEST_CASE("load_type: F repairs the data without moving the watermark", "[loadtype]") {
    // A repair re-reads everything to fix a target that drifted. It must not
    // touch the delta position: the operator is fixing data, not re-seeding, and
    // silently rewinding or advancing the watermark would turn a repair into a
    // second incident.
    const auto p = PlanLoad(LoadType::Full);
    CHECK(p.truncate_target);
    CHECK(p.read_rows);
    CHECK_FALSE(p.apply_floor);        // everything, not a window
    CHECK_FALSE(p.advance_watermark);
    CHECK_FALSE(p.seed_watermark);
}

TEST_CASE("load_type: I seeds the watermark and transfers nothing", "[loadtype]") {
    const auto p = PlanLoad(LoadType::InitOnly);
    CHECK_FALSE(p.read_rows);          // the whole point
    CHECK_FALSE(p.truncate_target);    // the data is already there
    CHECK(p.seed_watermark);
    CHECK_FALSE(p.advance_watermark);  // seeded from the source, not from a ceiling
}

TEST_CASE("load_type: L seeds and loads in one run", "[loadtype]") {
    const auto p = PlanLoad(LoadType::InitAndFull);
    CHECK(p.truncate_target);
    CHECK(p.read_rows);
    CHECK_FALSE(p.apply_floor);
    CHECK(p.advance_watermark);        // to the cycle ceiling, so D can follow
    CHECK_FALSE(p.seed_watermark);
}

TEST_CASE("load_type: D is the ordinary bounded cycle", "[loadtype]") {
    const auto p = PlanLoad(LoadType::Delta);
    CHECK_FALSE(p.truncate_target);
    CHECK(p.read_rows);
    CHECK(p.apply_floor);
    CHECK(p.advance_watermark);
    CHECK_FALSE(p.seed_watermark);
}

TEST_CASE("load_type: exactly one of seed and advance is ever set", "[loadtype]") {
    // Both would mean two writers of wm_value in one run, racing each other.
    for (auto t : {LoadType::Full, LoadType::InitOnly, LoadType::InitAndFull, LoadType::Delta}) {
        const auto p = PlanLoad(t);
        CHECK_FALSE((p.seed_watermark && p.advance_watermark));
    }
}

TEST_CASE("load_type: a run that reads nothing never truncates", "[loadtype]") {
    // Truncating without reading empties a populated target -- the exact
    // opposite of what I is for.
    for (auto t : {LoadType::Full, LoadType::InitOnly, LoadType::InitAndFull, LoadType::Delta}) {
        const auto p = PlanLoad(t);
        if (!p.read_rows) CHECK_FALSE(p.truncate_target);
    }
}

TEST_CASE("load_type: stored and typed spellings round-trip", "[loadtype]") {
    CHECK(ParseLoadType("F") == LoadType::Full);
    CHECK(ParseLoadType("I") == LoadType::InitOnly);
    CHECK(ParseLoadType("L") == LoadType::InitAndFull);
    CHECK(ParseLoadType("D") == LoadType::Delta);
    CHECK(ParseLoadType("d") == LoadType::Delta);       // CLI input is not shouted
    CHECK(LoadTypeCode(LoadType::InitOnly) == "I");
    CHECK(DefaultLoadType() == LoadType::Delta);
}

TEST_CASE("load_type: an unknown code is refused and names the legal set", "[loadtype]") {
    // The CLI's flag-name whitelist never inspects values, so this is the only
    // thing standing between `--load-type Z` and a silently wrong run.
    CHECK_FALSE(IsValidLoadTypeCode("Z"));
    CHECK_FALSE(IsValidLoadTypeCode(""));
    CHECK(IsValidLoadTypeCode("L"));
    CHECK_THROWS(ParseLoadType("Z"));
    try {
        ParseLoadType("Z");
    } catch (const std::exception &e) {
        const std::string m = e.what();
        CHECK(m.find("F") != std::string::npos);
        CHECK(m.find("I") != std::string::npos);
        CHECK(m.find("L") != std::string::npos);
        CHECK(m.find("D") != std::string::npos);
    }
}
