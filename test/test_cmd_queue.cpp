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
