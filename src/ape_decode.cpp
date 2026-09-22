#include "ape_decode.hpp"

#include <cctype>

namespace erpl_rev {
namespace ape {

namespace {

std::string UpperAscii(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::vector<std::string> SplitCells(const std::string &line, size_t field_count,
                                    const std::string &package_id, size_t row_no) {
    std::vector<std::string> cells;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cur += c;
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            cells.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (in_quotes)
        throw ApeDecodeError("APE decode: package '" + package_id + "' row " +
                             std::to_string(row_no) + ": unterminated quote");
    cells.push_back(cur);
    if (cells.size() != field_count)
        throw ApeDecodeError("APE decode: package '" + package_id + "' row " +
                             std::to_string(row_no) + ": " +
                             std::to_string(cells.size()) + " cells, expected " +
                             std::to_string(field_count));
    return cells;
}

}  // namespace

std::vector<std::vector<std::string>> DecodeCsv(const std::string &body,
                                                size_t field_count,
                                                const std::string &package_id) {
    std::vector<std::vector<std::string>> rows;
    size_t start = 0;
    size_t row_no = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        const bool last = (end == std::string::npos);
        std::string line = body.substr(start, last ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // A newline-terminated body ends with an empty remainder, not a row.
        if (!last || !line.empty()) {
            rows.push_back(SplitCells(line, field_count, package_id, row_no));
            ++row_no;
        }
        if (last) break;
        start = end + 1;
    }
    return rows;
}

size_t FindField(const std::vector<ApeField> &fields, const std::string &name) {
    const auto want = UpperAscii(name);
    for (size_t i = 0; i < fields.size(); ++i)
        if (UpperAscii(fields[i].name) == want) return i;
    throw ApeDecodeError("APE decode: unknown column '" + name + "'");
}

RowOp ClassifyRow(const std::vector<std::string> &row, size_t op_index) {
    if (op_index == kNoOperation) return RowOp::Upsert;
    if (op_index >= row.size())
        throw ApeDecodeError("APE decode: operation column out of range");
    const std::string &op = row[op_index];
    if (op.empty() || op == "U") return RowOp::Upsert;
    if (op == "D") return RowOp::Delete;
    throw ApeDecodeError("APE decode: unknown /1DH/OPERATION '" + op + "'");
}

}  // namespace ape
}  // namespace erpl_rev
