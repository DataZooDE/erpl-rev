// Minimal JSON helpers for the bridge: serialize scalar cells to JSON, and
// parse a JSON array of flat objects (string/number/bool/null values) into
// ordered key/value pairs. Deliberately small — the RFC payloads are flat
// tabular data, not arbitrary nested JSON.
#pragma once

#include <string>
#include <vector>
#include <utility>

namespace erpl_rev {
namespace json {

// One parsed cell. `is_raw` true => emit value verbatim (number/bool/null);
// false => a string value (already unescaped).
struct Cell {
    std::string key;
    std::string value;
    bool is_string = true;   // value is a string literal (needs quoting in SQL)
    bool is_null = false;
};

using Row = std::vector<Cell>;

// Escape a std::string as a JSON string body (no surrounding quotes).
std::string EscapeString(const std::string &s);

// Quote+escape into a full JSON string token.
std::string QuoteString(const std::string &s);

// Parse a JSON array "[ {..}, {..} ]" of flat objects. Throws std::runtime_error
// on malformed input.
std::vector<Row> ParseRows(const std::string &json_array);

} // namespace json
} // namespace erpl_rev
