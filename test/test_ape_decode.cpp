// APE package decode: CSV body -> cells, by-name mapping, operation verdict.
//
// The engine's package is a JSON envelope whose body is CSV (protocol §12).
// Envelope extraction (Body/Fields/batchIndex/lastBatch) happens in the cycle
// layer via DuckDB's JSON functions; everything here is pure string code so
// the hostile corpus runs with no database.
//
// Corpus values below are verbatim from the live `ZERPL_APE_T` hostile seeding
// (protocol §12): a decoder that splits on bare commas shifts every later
// column of such a row into plausible nonsense instead of an error.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "ape_decode.hpp"

using namespace erpl_rev::ape;
using Catch::Matchers::ContainsSubstring;

namespace {

ApeField F(const std::string &name, bool key = false) {
    ApeField f;
    f.name = name;
    f.is_key = key;
    return f;
}

}  // namespace

TEST_CASE("ape_decode: hostile CSV cells decode exactly", "[ape]") {
    const std::string body =
        "H0001,\"comma,inside\",1.00,EUR,1,1,\n"
        "H0002,\"quote\"\"inside\",-2.50,EUR,2,1,\n"
        "H0003,\"both,\"\"and more\",3.00,EUR,3,1,\n"
        "H0004,back\\slash and ;,-4.75,EUR,4,1,\n";
    const auto rows = DecodeCsv(body, 7, "p0");
    REQUIRE(rows.size() == 4);
    CHECK(rows[0][1] == "comma,inside");
    CHECK(rows[1][1] == "quote\"inside");
    CHECK(rows[1][2] == "-2.50");
    CHECK(rows[2][1] == "both,\"and more");
    // A backslash is data: neither escaped nor a reason to quote.
    CHECK(rows[3][1] == "back\\slash and ;");
    CHECK(rows[3][2] == "-4.75");
}

TEST_CASE("ape_decode: a trailing comma is a trailing empty cell", "[ape]") {
    // The last field is /1DH/OPERATION, empty on an initial-load row.
    const auto rows = DecodeCsv("AA,0017,2016-11-15,422.94,USD,747-400,\n", 7, "p0");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].size() == 7);
    CHECK(rows[0][6].empty());
}

TEST_CASE("ape_decode: a newline-terminated body has no phantom row", "[ape]") {
    const auto rows = DecodeCsv("A,1,\nB,2,\n", 3, "p0");
    CHECK(rows.size() == 2);
}

TEST_CASE("ape_decode: a short row is refused with package and row identity",
          "[ape]") {
    // Splitting on bare commas would silently shift this row; the count
    // assertion is the second line of defence (protocol §12).
    try {
        DecodeCsv("H0001,\"comma,inside\",1.00\nH0002,plain,2.00,x,\n", 4, "pkg7");
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("pkg7"));
        CHECK_THAT(e.what(), ContainsSubstring("row 0"));
    }
}

TEST_CASE("ape_decode: a long row is refused the same way", "[ape]") {
    try {
        DecodeCsv("A,1,EXTRA,\n", 3, "pkg7");
        FAIL("expected ApeDecodeError");
    } catch (const ApeDecodeError &e) {
        CHECK_THAT(e.what(), ContainsSubstring("pkg7"));
    }
}

TEST_CASE("ape_decode: fields map by name, case-insensitively", "[ape]") {
    // The package is self-describing; position is not the contract (the ADT
    // and DDIC APIs already disagreed about case once in cds_delta).
    const std::vector<ApeField> fields = {F("Carrid", true), F("PRICE")};
    CHECK(FindField(fields, "CARRID") == 0);
    CHECK(FindField(fields, "carrid") == 0);
    CHECK(FindField(fields, "Price") == 1);
    CHECK_THROWS_AS(FindField(fields, "NOPE"), ApeDecodeError);
}

TEST_CASE("ape_decode: operation classifies upsert vs delete", "[ape]") {
    // INSERT and UPDATE both arrive as U (after-image); only D deletes.
    // Blank operation (initial load) is an upsert.
    CHECK(ClassifyRow({"AA", ""}, 1) == RowOp::Upsert);
    CHECK(ClassifyRow({"AA", "U"}, 1) == RowOp::Upsert);
    CHECK(ClassifyRow({"AA", "D"}, 1) == RowOp::Delete);
    // No operation column at all (a view without the indicator) is an upsert.
    CHECK(ClassifyRow({"AA"}, kNoOperation) == RowOp::Upsert);
}
