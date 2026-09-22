// APE package decode: CSV body -> cells, by-name mapping, operation verdict.
//
// Pure string code by contract: envelope extraction (Body/Fields/batchIndex/
// lastBatch out of the JSON port data) happens in the cycle layer via
// DuckDB's JSON functions. Everything here runs with no database, so the
// hostile corpus from protocol §12 pins the decoder directly.
//
// CSV rules (measured live, protocol §12): RFC 4180 quoting — a value holding
// the separator or a double quote is wrapped, an inner quote is doubled. A
// backslash is data. A row whose cell count disagrees with the declared field
// count is refused with package/row identity, never silently shifted.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace erpl_rev {
namespace ape {

// Thrown for anything that cannot be decoded exactly. `what()` always names
// the package and, for row-level failures, the row.
struct ApeDecodeError : std::runtime_error {
    explicit ApeDecodeError(const std::string &msg) : std::runtime_error(msg) {}
};

// One self-describing package field (Attributes.ABAP.Fields).
struct ApeField {
    std::string name;
    std::string type;   // data element, e.g. S_PRICE (drives DDIC mapping)
    char kind = 'C';    // ABAP kind: C N D P I ...
    int length = 0;
    int decimals = 0;
    bool is_key = false;
};

// No operation column present (a source without the indicator): an upsert.
inline constexpr size_t kNoOperation = static_cast<size_t>(-1);

// Split a CSV body into rows of exactly field_count cells.
std::vector<std::vector<std::string>> DecodeCsv(const std::string &body,
                                                size_t field_count,
                                                const std::string &package_id);

// Index of the named field, matched case-insensitively. Throws when absent.
size_t FindField(const std::vector<ApeField> &fields, const std::string &name);

enum class RowOp { Upsert, Delete };

// /1DH/OPERATION verdict: U and blank are after-images (upsert), D deletes.
RowOp ClassifyRow(const std::vector<std::string> &row, size_t op_index);

}  // namespace ape
}  // namespace erpl_rev
