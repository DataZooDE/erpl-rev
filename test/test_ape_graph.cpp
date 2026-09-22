// APE graph-spec builder: registration knobs -> v6 graph JSON.
//
// The engine parses this JSON to create the graph, so the golden test pins it
// byte-for-byte against the fixture the erpl spike verified live (A4H,
// APE 2.7.0). A drifted key is a failed graph, not a cosmetic diff.
//
// ADR-1 lives here: the v6 reader reads exactly seven config paths, none of
// them a filter or schema. Emitting a filter would return all rows under the
// appearance of selection, so a spec carrying one is refused outright.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>
#include <sstream>

#include "ape_graph.hpp"

using namespace erpl_rev::ape;
using Catch::Matchers::ContainsSubstring;

namespace {

std::string ReadFixture() {
    std::ifstream in(std::string(ERPL_REV_TEST_FIXTURE_DIR) +
                     "/ape_graph_v6_initial_load.json");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    // File hygiene, not document: the trailing newline is not graph JSON.
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

ApeGraphSpec GoldenSpec() {
    ApeGraphSpec s;
    s.graph_id = "erpl_ape_v6";
    s.cds_name = "ZERPL_APE_FLIGHT";
    s.action = "Initial Load";
    s.subscription_type = "New";
    s.subscription_name = "ERPL_APE_S1";
    s.chunk_size = 100;
    s.wireformat = "Required Conversions Plus Time Format and Currency";
    return s;
}

}  // namespace

TEST_CASE("ape_graph: the golden initial-load spec matches the live fixture",
          "[ape]") {
    // Provenance: test/fixtures/README.md. Byte-identical, not equivalent:
    // key order is part of the contract the engine was observed to accept.
    CHECK(BuildGraphJson(GoldenSpec()) == ReadFixture());
}

TEST_CASE("ape_graph: a filter is refused, never emitted", "[ape]") {
    auto s = GoldenSpec();
    s.has_filter = true;
    try {
        BuildGraphJson(s);
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("filter"));
    }
}

TEST_CASE("ape_graph: an existing subscription travels by ID", "[ape]") {
    auto s = GoldenSpec();
    s.subscription_type = "Existing";
    s.subscription_name.clear();
    s.subscription_id = "0123456789ABCDEF0123456789ABCDEF";
    const auto j = BuildGraphJson(s);
    CHECK_THAT(j, ContainsSubstring("\"subscriptionType\":\"Existing\""));
    CHECK_THAT(j, ContainsSubstring("\"subscriptionID\":\"0123456789ABCDEF0123456789ABCDEF\""));
    CHECK(j.find("subscriptionName") == std::string::npos);
}

TEST_CASE("ape_graph: a new subscription without a name is refused", "[ape]") {
    auto s = GoldenSpec();
    s.subscription_name.clear();
    CHECK_THROWS_AS(BuildGraphJson(s), ApeDecodeError);
}

TEST_CASE("ape_graph: an existing subscription without an ID is refused",
          "[ape]") {
    auto s = GoldenSpec();
    s.subscription_type = "Existing";
    s.subscription_name.clear();
    s.subscription_id.clear();
    CHECK_THROWS_AS(BuildGraphJson(s), ApeDecodeError);
}

TEST_CASE("ape_graph: zero chunk size resolves to the default", "[ape]") {
    auto s = GoldenSpec();
    s.chunk_size = 0;
    const auto j = BuildGraphJson(s);
    CHECK_THAT(j, ContainsSubstring("\"chunkSize\":20000"));
}

TEST_CASE("ape_graph: unknown actions and empty names are refused", "[ape]") {
    auto s = GoldenSpec();
    s.action = "Delta Load";
    CHECK_THROWS_AS(BuildGraphJson(s), ApeDecodeError);
    s = GoldenSpec();
    s.cds_name.clear();
    CHECK_THROWS_AS(BuildGraphJson(s), ApeDecodeError);
    s = GoldenSpec();
    s.graph_id.clear();
    CHECK_THROWS_AS(BuildGraphJson(s), ApeDecodeError);
}

TEST_CASE("ape_graph: MakeGraphSpec maps a FULL registration to Initial Load",
          "[ape]") {
    const auto s =
        MakeGraphSpec("APE_FULL", "ZERPL_APE_D", "ERPL_APE_S1", "", 0, "");
    CHECK(s.action == "Initial Load");
    CHECK(s.subscription_type == "New");
    CHECK(s.subscription_name == "ERPL_APE_S1");
    CHECK(s.cds_name == "ZERPL_APE_D");
    CHECK(s.chunk_size == 0);
    const auto j = BuildGraphJson(s);
    CHECK_THAT(j, ContainsSubstring("\"chunkSize\":20000"));
    CHECK(j.find("wireformat") == std::string::npos);
}

TEST_CASE("ape_graph: MakeGraphSpec maps DELTA to Replication with override",
          "[ape]") {
    const auto s = MakeGraphSpec("APE_DELTA", "ZERPL_APE_D", "ERPL_APE_S1",
                                 "ERPL_APE_S1_20260101", 100,
                                 "Required Conversions Plus Time Format and Currency");
    CHECK(s.action == "Replication");
    CHECK(s.subscription_name == "ERPL_APE_S1_20260101");
    const auto j = BuildGraphJson(s);
    CHECK_THAT(j, ContainsSubstring("\"chunkSize\":100"));
    CHECK_THAT(j, ContainsSubstring("Plus Time Format and Currency"));
}

TEST_CASE("ape_graph: MakeGraphSpec refuses unknown methods and nameless subs",
          "[ape]") {
    CHECK_THROWS_AS(MakeGraphSpec("SNAPSHOT", "X", "S", "", 0, ""), ApeDecodeError);
    CHECK_THROWS_AS(MakeGraphSpec("APE_FULL", "X", "", "", 0, ""), ApeDecodeError);
}

TEST_CASE("ape_graph: an existing subscription id resumes by Existing", "[ape]") {
    const auto s = MakeGraphSpec("APE_DELTA", "ZERPL_APE_D", "ERPL_APE_S1", "", 0, "",
                                 "0123456789ABCDEF0123456789ABCDEF");
    CHECK(s.action == "Replication");
    CHECK(s.subscription_type == "Existing");
    const auto j = BuildGraphJson(s);
    CHECK_THAT(j, ContainsSubstring("\"subscriptionType\":\"Existing\""));
    CHECK_THAT(j, ContainsSubstring("\"subscriptionID\":\"0123456789ABCDEF0123456789ABCDEF\""));
    CHECK(j.find("subscriptionName") == std::string::npos);
}
