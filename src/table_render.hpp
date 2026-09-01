// Rendering a DuckDB result set for a terminal, a pipe, or a program.
//
// Pure: no I/O, no globals, no dependencies beyond what the project already
// links. Kept separate from the commands so the formatting can be tested
// without a database.
#pragma once

#include <string>

#include "duckdb_bridge.hpp"

namespace erpl_rev::render {

enum class Format { Table, Csv, Json };

struct Opts {
    size_t max_col_width = 40;
    bool header = true;
    // Printed under a table when the result was capped. Ignored by csv/json,
    // which are pipe formats where a silent truncation would be worse than a
    // slow pipe.
    bool footer = true;
};

// Parses `--format`; returns false for anything else.
bool ParseFormat(const std::string &s, Format &out);

std::string Render(const QueryResult &r, Format f, const Opts &o = {});

// Exposed for tests: the display width of a UTF-8 string in code points.
// Counting bytes misaligns every table containing an umlaut; counting code
// points is right for Latin and Cyrillic and still narrow for CJK and emoji,
// which is a trade this makes knowingly rather than pretending otherwise.
size_t DisplayWidth(const std::string &s);

// Exposed for tests: RFC 4180 field quoting.
std::string CsvField(const std::string &v, bool is_null);

} // namespace erpl_rev::render
