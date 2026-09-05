// Tests for the command queue's payload.
//
// A queued command's parameters are user input that travels a long way: into a
// SQL string literal, into a DuckDB VARCHAR, and finally through a hand-written
// JSON reader in ABAP. Anything that survives all three unchanged is safe;
// anything that does not is a value silently becoming a different value, or an
// escape breaking a statement. These pin the first hop and model the last.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "commands.hpp"
#include "db_client.hpp"
#include "load_type.hpp"

using namespace erpl_rev;
using Catch::Matchers::ContainsSubstring;

namespace {

// A faithful model of ZCL_ERPL_REV_CLIDRV=>jstr: find "key":, then read either
// a quoted string with backslash escapes, or a bare scalar up to , } or space.
// Written independently of the C++ builder so a bug in the builder cannot
// cancel itself out here.
std::string AbapJstr(const std::string &json, const std::string &key) {
    const std::string needle = "\"" + key + "\":";
    const auto at = json.find(needle);
    if (at == std::string::npos) return {};
    size_t p = at + needle.size();
    while (p < json.size() && json[p] == ' ') p++;
    if (p >= json.size()) return {};

    if (json[p] != '"') {
        std::string out;
        while (p < json.size() && json[p] != ',' && json[p] != '}' && json[p] != ' ')
            out += json[p++];
        return out == "null" ? std::string() : out;
    }
    p++;
    std::string out;
    while (p < json.size()) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            const char e = json[p + 1];
            if (e == 'n') out += '\n';
            else if (e == 't') out += '\t';
            else out += e;
            p += 2;
            continue;
        }
        if (json[p] == '"') break;
        out += json[p++];
    }
    return out;
}

} // namespace

TEST_CASE("params round-trip through the ABAP reader", "[queue]") {
    const std::string j = cmd::BuildParams({{"table", "MARA"}, {"target", "mara"}});
    CHECK(AbapJstr(j, "table") == "MARA");
    CHECK(AbapJstr(j, "target") == "mara");
    CHECK(AbapJstr(j, "absent").empty());
}

TEST_CASE("a value with quotes survives the trip", "[queue]") {
    // The motivating case: WHERE MANDT = '000'. Apostrophes are ordinary in
    // JSON; double quotes are the ones that must be escaped and unescaped.
    for (const std::string v : {std::string("MANDT = '000'"),
                                std::string("name = \"quoted\""),
                                std::string("path\\with\\backslashes"),
                                std::string("mixed \"a\" and 'b' and \\c")}) {
        const std::string j = cmd::BuildParams({{"where", v}});
        CHECK(AbapJstr(j, "where") == v);
    }
}

TEST_CASE("a value cannot inject another key", "[queue]") {
    // If the escaping were wrong, this value would close its own string and
    // introduce a "target" the caller never asked for.
    const std::string evil = R"(x","target":"evil)";
    const std::string j = cmd::BuildParams({{"where", evil}, {"target", "honest"}});
    CHECK(AbapJstr(j, "where") == evil);
    CHECK(AbapJstr(j, "target") == "honest");
}

TEST_CASE("the JSON also survives being put in a SQL literal", "[queue]") {
    // Second hop: the whole document goes into an INSERT as a string literal.
    const std::string j = cmd::BuildParams({{"where", "MANDT = '000'"}});
    const std::string lit = dbc::SqlLiteral(j);
    CHECK(lit.front() == '\'');
    CHECK(lit.back() == '\'');
    // Every apostrophe inside is doubled, so the literal cannot end early.
    for (size_t i = 1; i + 1 < lit.size(); i++) {
        if (lit[i] == '\'') {
            REQUIRE(i + 2 < lit.size());
            CHECK(lit[i + 1] == '\'');
            i++;
        }
    }
}

TEST_CASE("empty and numeric values are represented, not dropped", "[queue]") {
    const std::string j = cmd::BuildParams({{"columns", ""}, {"batch", "50000"}});
    CHECK(AbapJstr(j, "columns").empty());
    // Numbers are written as strings deliberately: the ABAP side converts them,
    // and one representation is easier to reason about than two.
    CHECK(AbapJstr(j, "batch") == "50000");
}

TEST_CASE("an empty parameter set is still valid JSON", "[queue]") {
    CHECK(cmd::BuildParams({}) == "{}");
}

TEST_CASE("a bare scalar is read the way cmd_id arrives", "[queue]") {
    // DuckDB returns cmd_id as a JSON number, not a string. The driver read
    // only quoted values at first, so it claimed a command and then decided
    // there was nothing to run -- leaving the row stuck in RUNNING.
    CHECK(AbapJstr(R"({"cmd_id":42,"verb":"replicate"})", "cmd_id") == "42");
    CHECK(AbapJstr(R"({"cmd_id":42,"verb":"replicate"})", "verb") == "replicate");
    CHECK(AbapJstr(R"({"result":null})", "result").empty());
}

// ---------------------------------------------------------------------------
// Unknown subcommand flags
//
// main() collects sync's and replicate's flags without knowing them, so an
// unrecognised one used to reach the command and be read by nobody. That is not
// a cosmetic gap: `replicate --queue-only` on a build predating that flag ran
// happily and took the generated-ABAP path instead of the queue, with no
// complaint anywhere. A flag that silently does nothing looks like it worked.
// ---------------------------------------------------------------------------

TEST_CASE("replicate accepts the flags it reads", "[args]") {
    CHECK(cmd::UnknownFlag({"--table", "T000", "--target", "t000", "--where",
                            "MANDT = '000'", "--parallel", "--jobs", "4"},
                           "replicate")
              .empty());
}

TEST_CASE("replicate refuses a flag it does not read", "[args]") {
    CHECK(cmd::UnknownFlag({"--tabel", "T000"}, "replicate") == "--tabel");
    // A flag from a newer build, run against an older one -- the shape of the
    // original bug. (`--queue-only` is consumed by ParseOption today, so it
    // never reaches these words; a caller from the future is the general case.)
    CHECK(cmd::UnknownFlag({"--table", "T000", "--from-the-future"}, "replicate")
          == "--from-the-future");
    // A sync flag is not a replicate flag, however valid it is elsewhere.
    CHECK(cmd::UnknownFlag({"--table", "T000", "--cadence", "nightly"}, "replicate")
          == "--cadence");
}

TEST_CASE("a flag's value is never mistaken for a flag", "[args]") {
    // main() splits --where=--x into two words. The second is a value, not a flag.
    CHECK(cmd::UnknownFlag({"--where", "--x"}, "replicate").empty());
    // ... but the word after a boolean flag still has to be checked.
    CHECK(cmd::UnknownFlag({"--parallel", "--nope"}, "replicate") == "--nope");
}

TEST_CASE("positionals are not flags", "[args]") {
    CHECK(cmd::UnknownFlag({"create", "t000", "--method", "SNAPSHOT", "--source",
                            "T000", "--keys", "MANDT"},
                           "sync create")
              .empty());
}

TEST_CASE("each sync subcommand has its own flag set", "[args]") {
    CHECK(cmd::UnknownFlag({"schedule", "--every", "5"}, "sync schedule").empty());
    CHECK(cmd::UnknownFlag({"schedule", "--remove"}, "sync schedule").empty());
    // create's flags do not travel to schedule, nor schedule's to create.
    CHECK(cmd::UnknownFlag({"schedule", "--keys", "MANDT"}, "sync schedule") == "--keys");
    CHECK(cmd::UnknownFlag({"create", "t", "--every", "5"}, "sync create") == "--every");
    // ls / show / run read no flags of their own.
    CHECK(cmd::UnknownFlag({"ls", "--every", "5"}, "sync ls") == "--every");
    CHECK(cmd::UnknownFlag({"ls"}, "sync ls").empty());
}

// ---------------------------------------------------------------------------
// --load-type: three separate failure modes, not one
//
// UnknownFlag validates flag NAMES against a per-subcommand whitelist. It never
// looks at a value -- so `sync run --load-type Z` sails past it. Asserting on
// UnknownFlag alone would have "proved" validation that does not exist.
// ---------------------------------------------------------------------------

TEST_CASE("args: sync run accepts --load-type", "[args]") {
    CHECK(cmd::UnknownFlag({"run", "ZDELTA_WM", "--load-type", "I"}, "sync run").empty());
}

TEST_CASE("args: an unknown flag on sync run is still refused", "[args]") {
    CHECK(cmd::UnknownFlag({"run", "ZDELTA_WM", "--lod-type", "I"}, "sync run") == "--lod-type");
}

TEST_CASE("args: an unknown SUBCOMMAND is a different failure from an unknown flag",
          "[args]") {
    // Nothing is registered for "sync frobnicate", so every flag reads as
    // unknown -- which is why the subcommand has to be rejected on its own,
    // before its flags are judged.
    CHECK(cmd::UnknownFlag({"frobnicate", "--load-type", "I"}, "sync frobnicate") ==
          "--load-type");
}

TEST_CASE("args: an invalid --load-type VALUE is caught by the type, not the whitelist",
          "[args]") {
    // The whitelist passes it...
    CHECK(cmd::UnknownFlag({"run", "T", "--load-type", "Z"}, "sync run").empty());
    // ...so this is the only thing that stops a silently wrong run.
    CHECK_FALSE(erpl_rev::IsValidLoadTypeCode("Z"));
}
