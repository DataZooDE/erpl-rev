// APE envelope extraction: JSON port data -> body + fields + flow flags.
//
// The engine hands over `DHAPE_PORT-PORT_DATA`: a JSON envelope whose body is
// CSV (protocol §12), or NULL when there is nothing, or a control envelope
// (valid JSON, no Fields/Body) that still carries lastBatch. Pure string
// code: the JSON reader below parses exactly the subset the engine emits and
// refuses everything else, so a changed wire shape fails loudly at the gate
// instead of decoding as plausible nonsense downstream.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "ape_envelope.hpp"

using namespace erpl_rev::ape;
using Catch::Matchers::ContainsSubstring;

namespace {

// Verbatim envelope shape from protocol §12 (compacted; the reader must take
// pretty-printed wire too). Hostile body rows from the same section.
const char *kDataEnvelope = R"({
  "Encoding": "csv",
  "Wireformat": "Required Conversions Plus Time Format and Currency",
  "Attributes": {
    "metadata": [ { "Field": { "ColumnName": "Rid", "IsKeyField": "X" } } ],
    "message.batchIndex": 7,
    "message.lastBatch": false,
    "ABAP": { "Kind": "Table", "Header": {},
      "Fields": [
        { "Name": "RID", "Type": "CHAR10", "Kind": "C", "Length": 10, "Decimals": 0 },
        { "Name": "DESCR", "Type": "CHAR40", "Kind": "C", "Length": 40, "Decimals": 0 },
        { "Name": "AMOUNT", "Type": "S_PRICE", "Kind": "P", "Length": 8, "Decimals": 2 },
        { "Name": "/1DH/OPERATION", "Type": "CHAR1", "Kind": "C", "Length": 1, "Decimals": 0 } ] } },
  "Body": "H0001,\"comma,inside\",1.00,\nH0002,plain,-2.50,U\n"
})";

const char *kControlEnvelope = R"({
  "Encoding": "csv",
  "Attributes": { "message.batchIndex": 8, "message.lastBatch": true }
})";

}  // namespace

TEST_CASE("ape_envelope: a data envelope extracts everything", "[ape]") {
    const auto e = ExtractEnvelope(kDataEnvelope);
    CHECK(e.has_data);
    CHECK_FALSE(e.is_control);
    CHECK(e.batch_index == 7);
    CHECK_FALSE(e.last_batch);
    REQUIRE(e.fields.size() == 4);
    CHECK(e.fields[0].name == "RID");
    CHECK(e.fields[0].is_key);
    CHECK(e.fields[2].type == "S_PRICE");
    CHECK(e.fields[2].kind == 'P');
    CHECK(e.fields[2].decimals == 2);
    CHECK(e.fields[3].name == "/1DH/OPERATION");
    CHECK(e.body == "H0001,\"comma,inside\",1.00,\nH0002,plain,-2.50,U\n");
}

TEST_CASE("ape_envelope: NULL port data is no data, not an error", "[ape]") {
    // The engine hands over NULL (not empty) when a roundtrip has nothing.
    // Reading it as text yields the literal string "NULL" (protocol §12).
    const auto e = ExtractEnvelope(nullptr);
    CHECK_FALSE(e.has_data);
    CHECK_FALSE(e.is_control);
}

TEST_CASE("ape_envelope: empty port data is no data", "[ape]") {
    const auto e = ExtractEnvelope("");
    CHECK_FALSE(e.has_data);
}

TEST_CASE("ape_envelope: a control envelope skips but keeps lastBatch",
          "[ape]") {
    // Valid JSON with neither Fields nor Body: not an error, but the
    // lastBatch flag still decides termination, so read it first.
    const auto e = ExtractEnvelope(kControlEnvelope);
    CHECK_FALSE(e.has_data);
    CHECK(e.is_control);
    CHECK(e.last_batch);
    CHECK(e.batch_index == 8);
}

TEST_CASE("ape_envelope: lastBatch on a data envelope ends the scan", "[ape]") {
    std::string j = kDataEnvelope;
    const std::string needle = "\"message.lastBatch\": false";
    const auto pos = j.find(needle);
    REQUIRE(pos != std::string::npos);
    j.replace(pos, needle.size(), "\"message.lastBatch\": true");
    const auto e = ExtractEnvelope(j.c_str());
    CHECK(e.has_data);
    CHECK(e.last_batch);
}

TEST_CASE("ape_envelope: a non-csv encoding is refused", "[ape]") {
    std::string j = kDataEnvelope;
    const auto pos = j.find("\"csv\"");
    REQUIRE(pos != std::string::npos);
    j.replace(pos, 5, "\"json\"");
    CHECK_THROWS_AS(ExtractEnvelope(j.c_str()), ApeDecodeError);
}

TEST_CASE("ape_envelope: a body without fields is refused", "[ape]") {
    // The self-describing contract cuts both ways: rows without a declared
    // field list cannot be mapped by name.
    std::string j = kDataEnvelope;
    const auto pos = j.find("\"Fields\"");
    REQUIRE(pos != std::string::npos);
    j.replace(pos, 8, "\"NoFields\"");
    CHECK_THROWS_AS(ExtractEnvelope(j.c_str()), ApeDecodeError);
}

TEST_CASE("ape_envelope: truncated JSON is refused", "[ape]") {
    CHECK_THROWS_AS(ExtractEnvelope("{\"Encoding\": \"csv\", \"Attr"),
                    ApeDecodeError);
}

TEST_CASE("ape_envelope: a non-Table ABAP kind is a control envelope", "[ape]") {
    // Live shape (A4H): the lastBatch terminator arrives with
    // ABAP.Kind = "Element" (a scalar descriptor, no field list) while still
    // carrying a Body. Only Kind = "Table" carries stageable rows; anything
    // else skips after reading lastBatch (HLD T-3).
    const char *element = R"({
  "Encoding": "csv",
  "Wireformat": "Enhanced Format Conversions",
  "Attributes": {
    "message.batchIndex": 1,
    "message.lastBatch": true,
    "ABAP": { "Kind": "Element", "Header": { "Fieldname": "" } } },
  "Body": "7"
})";
    const auto e = ExtractEnvelope(element);
    CHECK_FALSE(e.has_data);
    CHECK(e.is_control);
    CHECK(e.last_batch);
    CHECK(e.batch_index == 1);
}
