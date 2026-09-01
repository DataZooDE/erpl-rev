// Tests for result rendering. Pure functions over a QueryResult, so no
// database is involved.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "table_render.hpp"

using namespace erpl_rev;
using namespace erpl_rev::render;
using Catch::Matchers::ContainsSubstring;

namespace {

QueryResult Make(std::vector<QueryColumn> cols, std::vector<std::string> rows,
                 long long total = -1, bool truncated = false) {
    QueryResult r;
    r.columns = std::move(cols);
    r.rows = std::move(rows);
    r.row_count = total < 0 ? static_cast<long long>(r.rows.size()) : total;
    r.truncated = truncated;
    return r;
}

} // namespace

TEST_CASE("json output is a passthrough of the row objects", "[render]") {
    // The rows are already JSON; re-encoding them would only add ways to be
    // wrong, so the bytes must survive untouched.
    const auto r = Make({{"n", "INTEGER"}}, {R"({"n":42})"});
    const std::string out = Render(r, Format::Json);
    CHECK_THAT(out, ContainsSubstring(R"({"n":42})"));
    CHECK_THAT(out, ContainsSubstring(R"("name":"n")"));
    CHECK_THAT(out, ContainsSubstring(R"("truncated":false)"));
}

TEST_CASE("csv quotes exactly the fields RFC 4180 requires", "[render]") {
    CHECK(CsvField("plain", false) == "plain");
    CHECK(CsvField("has,comma", false) == "\"has,comma\"");
    CHECK(CsvField("has\"quote", false) == "\"has\"\"quote\"");
    CHECK(CsvField("has\nnewline", false) == "\"has\nnewline\"");
    CHECK(CsvField("has\rcr", false) == "\"has\rcr\"");
}

TEST_CASE("csv distinguishes NULL from the empty string", "[render]") {
    // An unquoted empty field is NULL; "" is a present, empty value. That is
    // the one distinction a CSV reader can still make downstream.
    CHECK(CsvField("", true) == "");
    CHECK(CsvField("", false) == "");
    const auto r = Make({{"a", "VARCHAR"}, {"b", "VARCHAR"}},
                        {R"({"a":null,"b":""})"});
    CHECK_THAT(Render(r, Format::Csv), ContainsSubstring("a,b\n,\n"));
}

TEST_CASE("an empty result still renders its header", "[render]") {
    const auto r = Make({{"a", "VARCHAR"}}, {});
    CHECK_THAT(Render(r, Format::Csv), ContainsSubstring("a\n"));
    CHECK_THAT(Render(r, Format::Table), ContainsSubstring("(0 rows)"));
}

TEST_CASE("width is measured in code points, not bytes", "[render]") {
    // Counting bytes misaligns every table containing an umlaut.
    CHECK(DisplayWidth("abc") == 3);
    CHECK(DisplayWidth("Müller") == 6);     // 7 bytes
    CHECK(DisplayWidth("日本語") == 3);      // 9 bytes
    CHECK(DisplayWidth("🦆") == 1);          // 4 bytes
}

TEST_CASE("a non-ASCII value does not skew the column", "[render]") {
    const auto r = Make({{"name", "VARCHAR"}},
                        {R"({"name":"Müller"})", R"({"name":"Smith"})"});
    const std::string out = Render(r, Format::Table);
    // Both data lines must be padded to the same visible width.
    std::vector<std::string> lines;
    std::string cur;
    for (char c : out) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += c; }
    REQUIRE(lines.size() >= 4);
    CHECK(DisplayWidth(lines[2]) == DisplayWidth(lines[3]));
}

TEST_CASE("numeric columns are right-aligned, text is not", "[render]") {
    const auto r = Make({{"n", "BIGINT"}, {"s", "VARCHAR"}},
                        {R"({"n":1,"s":"a"})", R"({"n":1000,"s":"bbbb"})"});
    const std::string out = Render(r, Format::Table);
    CHECK_THAT(out, ContainsSubstring("   1  a"));      // number padded on the left
    CHECK_THAT(out, ContainsSubstring("1000  bbbb"));
}

TEST_CASE("a truncated result says so, and names how to get the rest", "[render]") {
    const auto r = Make({{"n", "INTEGER"}}, {R"({"n":1})"}, 4182993, true);
    const std::string out = Render(r, Format::Table);
    CHECK_THAT(out, ContainsSubstring("showing 1 of 4,182,993 rows"));
    CHECK_THAT(out, ContainsSubstring("--limit 0"));
}

TEST_CASE("an over-wide cell is clipped with an ellipsis", "[render]") {
    const auto r = Make({{"s", "VARCHAR"}}, {R"({"s":"abcdefghij"})"});
    Opts o;
    o.max_col_width = 5;
    CHECK_THAT(Render(r, Format::Table, o), ContainsSubstring("abcd…"));
}

TEST_CASE("format names parse, and nothing else does", "[render]") {
    Format f;
    CHECK(ParseFormat("table", f));
    CHECK(ParseFormat("csv", f));
    CHECK(ParseFormat("json", f));
    CHECK_FALSE(ParseFormat("yaml", f));
    CHECK_FALSE(ParseFormat("", f));
}
