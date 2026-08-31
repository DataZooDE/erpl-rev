#include "table_render.hpp"

#include <algorithm>
#include <sstream>

#include "json_util.hpp"

namespace erpl_rev::render {

bool ParseFormat(const std::string &s, Format &out) {
    if (s == "table") { out = Format::Table; return true; }
    if (s == "csv")   { out = Format::Csv;   return true; }
    if (s == "json")  { out = Format::Json;  return true; }
    return false;
}

size_t DisplayWidth(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;   // count lead bytes, skip continuations
    return n;
}

namespace {

// Truncate to `width` code points, appending an ellipsis when it had to cut.
std::string Clip(const std::string &s, size_t width) {
    if (DisplayWidth(s) <= width) return s;
    if (width == 0) return "";
    size_t seen = 0, i = 0;
    for (; i < s.size(); i++) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (seen == width - 1) break;
            seen++;
        }
    }
    return s.substr(0, i) + "…";
}

bool IsNumeric(const std::string &type) {
    static const char *kNum[] = {"TINYINT", "SMALLINT", "INTEGER", "BIGINT",
                                 "HUGEINT", "UTINYINT", "USMALLINT", "UINTEGER",
                                 "UBIGINT", "FLOAT", "DOUBLE", "DECIMAL"};
    for (const char *n : kNum)
        if (type.rfind(n, 0) == 0) return true;
    return false;
}

// Wrap the row objects into one array so json::ParseRows can take them, then
// index each row by column name. Rows are already JSON objects, so this is a
// parse rather than a conversion.
std::vector<json::Row> ParseAll(const QueryResult &r) {
    if (r.rows.empty()) return {};
    std::string arr = "[";
    for (size_t i = 0; i < r.rows.size(); i++) {
        if (i) arr += ",";
        arr += r.rows[i];
    }
    arr += "]";
    return json::ParseRows(arr);
}

const json::Cell *Find(const json::Row &row, const std::string &key) {
    for (const auto &c : row)
        if (c.key == key) return &c;
    return nullptr;
}

std::string Thousands(long long v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

} // namespace

std::string CsvField(const std::string &v, bool is_null) {
    // An unquoted empty field is NULL; "" is the empty string. That is the one
    // distinction a CSV reader can still make, so keep it.
    if (is_null) return "";
    const bool needs = v.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs) return v;
    std::string out = "\"";
    for (char c : v) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::string Render(const QueryResult &r, Format f, const Opts &o) {
    if (f == Format::Json) {
        // Zero conversion: the rows are already JSON objects, so re-encoding
        // them would only add ways to be wrong.
        std::ostringstream s;
        s << "{\"columns\":[";
        for (size_t i = 0; i < r.columns.size(); i++) {
            if (i) s << ",";
            s << "{\"name\":" << json::QuoteString(r.columns[i].name)
              << ",\"type\":" << json::QuoteString(r.columns[i].type) << "}";
        }
        s << "],\"rows\":[";
        for (size_t i = 0; i < r.rows.size(); i++) {
            if (i) s << ",";
            s << r.rows[i];
        }
        s << "],\"row_count\":" << r.row_count
          << ",\"truncated\":" << (r.truncated ? "true" : "false") << "}\n";
        return s.str();
    }

    const auto rows = ParseAll(r);

    if (f == Format::Csv) {
        std::ostringstream s;
        if (o.header) {
            for (size_t i = 0; i < r.columns.size(); i++) {
                if (i) s << ",";
                s << CsvField(r.columns[i].name, false);
            }
            s << "\n";
        }
        for (const auto &row : rows) {
            for (size_t i = 0; i < r.columns.size(); i++) {
                if (i) s << ",";
                const json::Cell *c = Find(row, r.columns[i].name);
                if (c) s << CsvField(c->value, c->is_null);
            }
            s << "\n";
        }
        return s.str();
    }

    // --- table ---------------------------------------------------------------
    const size_t n = r.columns.size();
    std::vector<size_t> w(n);
    for (size_t i = 0; i < n; i++) w[i] = DisplayWidth(r.columns[i].name);

    std::vector<std::vector<std::string>> cells;
    cells.reserve(rows.size());
    for (const auto &row : rows) {
        std::vector<std::string> line(n);
        for (size_t i = 0; i < n; i++) {
            const json::Cell *c = Find(row, r.columns[i].name);
            line[i] = !c ? "" : (c->is_null ? "NULL" : c->value);
            line[i] = Clip(line[i], o.max_col_width);
            w[i] = std::max(w[i], DisplayWidth(line[i]));
        }
        cells.push_back(std::move(line));
    }

    auto pad = [](const std::string &v, size_t width, bool right) {
        const size_t have = DisplayWidth(v);
        const std::string sp(width > have ? width - have : 0, ' ');
        return right ? sp + v : v + sp;
    };

    std::ostringstream s;
    if (o.header) {
        for (size_t i = 0; i < n; i++) {
            if (i) s << "  ";
            s << pad(r.columns[i].name, w[i], IsNumeric(r.columns[i].type));
        }
        s << "\n";
        for (size_t i = 0; i < n; i++) {
            if (i) s << "  ";
            s << std::string(w[i], '-');
        }
        s << "\n";
    }
    for (const auto &line : cells) {
        for (size_t i = 0; i < n; i++) {
            if (i) s << "  ";
            s << pad(line[i], w[i], IsNumeric(r.columns[i].type));
        }
        s << "\n";
    }
    if (o.footer) {
        if (r.truncated)
            s << "(showing " << Thousands(static_cast<long long>(rows.size()))
              << " of " << Thousands(r.row_count) << " rows — --limit 0 for all)\n";
        else
            s << "(" << Thousands(static_cast<long long>(rows.size())) << " rows)\n";
    }
    return s.str();
}

} // namespace erpl_rev::render
