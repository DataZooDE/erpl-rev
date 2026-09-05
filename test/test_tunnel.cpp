// Tests for the optional gateway tunnel's pure logic.
//
// Two properties matter more than the rest. First, that the feature is inert:
// with no secret named, Resolve must hand back the gateway itself as the dial
// target, because every deployment that exists today runs that way and must keep
// running that way byte for byte. Second, that the gateway keeps its identity —
// `gwhost`/`gwserv` are what doctor prints, what setup persists and what the
// reginfo handout puts in ACCESS/CANCEL, so a tunnel must never overwrite them
// with 127.0.0.1. That confusion is exactly the bug the hand-rolled --init-sql
// recipe causes, and it is what this file is here to prevent.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "tunnel.hpp"

using namespace erpl_rev;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("no secret means no tunnel, and the gateway is dialled directly", "[tunnel]") {
    tunnel::Plan p;
    std::string err;
    REQUIRE(tunnel::Resolve("", "", "sapgw.corp", "3300", 0, p, err));
    CHECK(err.empty());
    CHECK_FALSE(p.enabled);
    CHECK(p.dial_host == "sapgw.corp");
    CHECK(p.dial_port == "3300");
    CHECK(p.gwhost == "sapgw.corp");
    CHECK(p.gwserv == "3300");
    CHECK(tunnel::ImportSql(p).empty());
}

TEST_CASE("a non-numeric gateway service is fine when no tunnel is configured", "[tunnel]") {
    // sapgw00 is valid for gwserv -- the RFC SDK resolves it through /etc/services.
    // Only the tunnel needs a number, so the no-tunnel path must not reject it.
    tunnel::Plan p;
    std::string err;
    REQUIRE(tunnel::Resolve("", "", "sapgw.corp", "sapgw00", 0, p, err));
    CHECK(p.dial_port == "sapgw00");
}

TEST_CASE("a tunnel dials loopback but the gateway keeps its identity", "[tunnel]") {
    tunnel::Plan p;
    std::string err;
    REQUIRE(tunnel::Resolve("sap", "", "sapgw.corp", "3300", 40511, p, err));
    CHECK(p.enabled);
    CHECK(p.secret == "sap");
    CHECK(p.dial_host == "127.0.0.1");
    CHECK(p.dial_port == "40511");
    // The part that must never regress: these still name the real gateway.
    CHECK(p.gwhost == "sapgw.corp");
    CHECK(p.gwserv == "3300");
}

TEST_CASE("--tunnel-target overrides the far end", "[tunnel]") {
    tunnel::Plan p;
    std::string err;
    REQUIRE(tunnel::Resolve("sap", "other.corp:3301", "sapgw.corp", "3300", 40511, p, err));
    CHECK(p.gwhost == "other.corp");
    CHECK(p.gwserv == "3301");
    CHECK(p.dial_port == "40511");
}

TEST_CASE("a half-configured tunnel is refused, not ignored", "[tunnel]") {
    tunnel::Plan p;
    std::string err;
    // A target with no secret forwards nothing; silently dropping it would leave
    // the operator believing a tunnel is up.
    CHECK_FALSE(tunnel::Resolve("", "other.corp:3301", "sapgw.corp", "3300", 0, p, err));
    CHECK_THAT(err, ContainsSubstring("--tunnel-secret"));
    // Same for a pinned local port with nothing to bind.
    CHECK_FALSE(tunnel::Resolve("", "", "sapgw.corp", "3300", 40511, p, err));
    CHECK_THAT(err, ContainsSubstring("--tunnel-secret"));
}

TEST_CASE("a tunnel to a named gateway service is refused with the fix in the message",
          "[tunnel]") {
    tunnel::Plan p;
    std::string err;
    CHECK_FALSE(tunnel::Resolve("sap", "", "sapgw.corp", "sapgw00", 40511, p, err));
    CHECK_THAT(err, ContainsSubstring("sapgw00"));
    CHECK_THAT(err, ContainsSubstring("--tunnel-target"));
}

TEST_CASE("SplitTarget", "[tunnel]") {
    std::string h, p, err;
    REQUIRE(tunnel::SplitTarget("host.corp:3300", h, p, err));
    CHECK(h == "host.corp");
    CHECK(p == "3300");

    // Bracketed IPv6 keeps its own colons.
    REQUIRE(tunnel::SplitTarget("[fe80::1]:3300", h, p, err));
    CHECK(h == "[fe80::1]");
    CHECK(p == "3300");

    CHECK_FALSE(tunnel::SplitTarget("host.corp", h, p, err));
    CHECK_FALSE(tunnel::SplitTarget(":3300", h, p, err));
    CHECK_FALSE(tunnel::SplitTarget("host.corp:sapgw00", h, p, err));
    CHECK_FALSE(tunnel::SplitTarget("", h, p, err));
}

TEST_CASE("ImportSql quotes its operands", "[tunnel]") {
    tunnel::Plan p;
    std::string err;
    REQUIRE(tunnel::Resolve("sap", "", "sapgw.corp", "3300", 40511, p, err));
    const std::string sql = tunnel::ImportSql(p);
    // A PRAGMA with named arguments -- the form erpl-tunnel actually registers.
    // `CALL tunnel_import(...)` compiles fine and fails at runtime with "Table
    // Function with name tunnel_import does not exist", which is why this is
    // pinned by an exact-match test rather than a substring one.
    CHECK(sql == "PRAGMA tunnel_import(secret = 'sap', remote_host = 'sapgw.corp', "
                 "remote_port = 3300, local_port = 40511, timeout = 30)");
}

TEST_CASE("a quote in the secret name cannot terminate the literal", "[tunnel]") {
    // The secret name is operator input and reaches DuckDB as SQL. It has no
    // business ending the string it sits in.
    tunnel::Plan p;
    std::string err;
    REQUIRE(tunnel::Resolve("it's", "", "sapgw.corp", "3300", 40511, p, err));
    const std::string sql = tunnel::ImportSql(p);
    CHECK_THAT(sql, ContainsSubstring("'it''s'"));
    // Same for the host half.
    REQUIRE(tunnel::Resolve("sap", "o'hare.corp:3300", "sapgw.corp", "3300", 40511, p, err));
    CHECK_THAT(tunnel::ImportSql(p), ContainsSubstring("'o''hare.corp'"));
}

TEST_CASE("an out-of-range local port is refused", "[tunnel]") {
    tunnel::Plan p;
    std::string err;
    CHECK_FALSE(tunnel::Resolve("sap", "", "sapgw.corp", "3300", 0, p, err));
    CHECK_FALSE(tunnel::Resolve("sap", "", "sapgw.corp", "3300", 70000, p, err));
}
