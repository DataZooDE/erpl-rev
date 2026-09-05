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

// ---------------------------------------------------------------------------
// Scaffolding for `erpl-rev setup`
//
// The generated block is erpl-tunnel's own SQL. Generating it rather than asking
// the operator to copy it out of a doc is the point: a mistyped backend option
// produces a secret that parses and then fails at boot, which is a long way from
// where the mistake was made.
// ---------------------------------------------------------------------------

TEST_CASE("backend names round-trip, and nothing else is accepted", "[tunnel]") {
    tunnel::Backend b;
    REQUIRE(tunnel::ParseBackend("tailscale", b));
    CHECK(std::string(tunnel::BackendName(b)) == "tailscale");
    REQUIRE(tunnel::ParseBackend("netbird", b));
    CHECK(std::string(tunnel::BackendName(b)) == "netbird");
    REQUIRE(tunnel::ParseBackend("ssh", b));
    CHECK(std::string(tunnel::BackendName(b)) == "ssh");
    CHECK_FALSE(tunnel::ParseBackend("wireguard", b));
    CHECK_FALSE(tunnel::ParseBackend("", b));
    CHECK_FALSE(tunnel::ParseBackend("Tailscale", b));   // exact, not fuzzy
}

TEST_CASE("the scaffolded secret loads the extension and names the right key", "[tunnel]") {
    tunnel::SecretSpec s;
    s.backend = tunnel::Backend::Tailscale;
    s.secret = "sap_gateway";
    s.key = "tskey-auth-abc";
    s.hostname = "ingest01";
    s.state_dir = "/var/lib/erpl/mesh";
    const std::string sql = tunnel::RenderSecretSql(s);

    // CREATE SECRET (TYPE tunnel) is only recognised once the extension is loaded,
    // so the block has to carry the INSTALL/LOAD itself.
    CHECK_THAT(sql, ContainsSubstring("INSTALL erpl_tunnel FROM community"));
    CHECK_THAT(sql, ContainsSubstring("LOAD erpl_tunnel"));
    CHECK_THAT(sql, ContainsSubstring("CREATE OR REPLACE SECRET sap_gateway"));
    CHECK_THAT(sql, ContainsSubstring("backend 'tailscale'"));
    CHECK_THAT(sql, ContainsSubstring("auth_key 'tskey-auth-abc'"));
    CHECK_THAT(sql, ContainsSubstring("state_dir '/var/lib/erpl/mesh'"));
    // NetBird's key has a different name; using Tailscale's would fail at boot.
    CHECK_THAT(sql, !ContainsSubstring("setup_key"));
}

TEST_CASE("netbird gets setup_key, not auth_key", "[tunnel]") {
    tunnel::SecretSpec s;
    s.backend = tunnel::Backend::NetBird;
    s.secret = "nb";
    s.key = "A2C8-E62B";
    const std::string sql = tunnel::RenderSecretSql(s);
    CHECK_THAT(sql, ContainsSubstring("backend 'netbird'"));
    CHECK_THAT(sql, ContainsSubstring("setup_key 'A2C8-E62B'"));
    CHECK_THAT(sql, !ContainsSubstring("auth_key"));
}

TEST_CASE("the ssh block carries the host-key warning", "[tunnel]") {
    tunnel::SecretSpec s;
    s.backend = tunnel::Backend::Ssh;
    s.secret = "bastion";
    s.hostname = "jump.corp";
    s.ssh_user = "svc";
    s.ssh_port = "2222";
    s.key = "hunter2";
    const std::string sql = tunnel::RenderSecretSql(s);
    CHECK_THAT(sql, ContainsSubstring("TYPE ssh_tunnel"));
    CHECK_THAT(sql, ContainsSubstring("host 'jump.corp'"));
    CHECK_THAT(sql, ContainsSubstring("port 2222"));
    CHECK_THAT(sql, ContainsSubstring("user 'svc'"));
    // erpl-tunnel's own docs say the SSH backend does not pin host keys. Anyone
    // reading the generated file should learn that from the file.
    CHECK_THAT(sql, ContainsSubstring("does not pin"));
}

TEST_CASE("a missing key renders a marked placeholder, not an empty literal", "[tunnel]") {
    // '' would parse and then fail to authenticate at boot, a long way from here.
    tunnel::SecretSpec s;
    s.backend = tunnel::Backend::Tailscale;
    s.secret = "sap";
    const std::string sql = tunnel::RenderSecretSql(s);
    CHECK_THAT(sql, ContainsSubstring("PASTE-YOUR-KEY-HERE"));
    CHECK_THAT(sql, ContainsSubstring("TODO"));
    CHECK_THAT(sql, !ContainsSubstring("auth_key ''"));
}

TEST_CASE("a quote in a scaffolded value cannot break out of its literal", "[tunnel]") {
    tunnel::SecretSpec s;
    s.backend = tunnel::Backend::Tailscale;
    s.secret = "sap";
    s.key = "ab'cd";
    s.hostname = "o'hare";
    const std::string sql = tunnel::RenderSecretSql(s);
    CHECK_THAT(sql, ContainsSubstring("'ab''cd'"));
    CHECK_THAT(sql, ContainsSubstring("'o''hare'"));
}
