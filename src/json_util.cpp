#include "json_util.hpp"

#include <stdexcept>
#include <cctype>

namespace erpl_rev {
namespace json {

std::string EscapeString(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char *hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string QuoteString(const std::string &s) {
    return "\"" + EscapeString(s) + "\"";
}

namespace {

struct Parser {
    const std::string &s;
    size_t i = 0;
    explicit Parser(const std::string &str) : s(str) {}

    [[noreturn]] void fail(const std::string &msg) {
        throw std::runtime_error("JSON parse error at " + std::to_string(i) +
                                 ": " + msg);
    }
    void skip_ws() {
        while (i < s.size() &&
               (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
    }
    char peek() { skip_ws(); return i < s.size() ? s[i] : '\0'; }
    char next() { skip_ws(); return i < s.size() ? s[i++] : '\0'; }
    void expect(char c) { if (next() != c) fail(std::string("expected '") + c + "'"); }

    std::string parse_string() {
        if (next() != '"') fail("expected string");
        std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (c == '\\') {
                if (i >= s.size()) fail("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) fail("bad \\u");
                        // Minimal: decode BMP code point to UTF-8.
                        unsigned cp = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else fail("bad hex");
                        }
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: fail("bad escape char");
                }
            } else {
                out += c;
            }
        }
        fail("unterminated string");
    }

    // Parse a scalar value into a Cell (key set by caller).
    void parse_value(Cell &cell) {
        char c = peek();
        if (c == '"') {
            cell.value = parse_string();
            cell.is_string = true;
            cell.is_null = false;
        } else if (c == 't' || c == 'f') {
            // true / false
            std::string lit;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])))
                lit += s[i++];
            if (lit != "true" && lit != "false") fail("bad literal");
            cell.value = lit;
            cell.is_string = false;
            cell.is_null = false;
        } else if (c == 'n') {
            std::string lit;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])))
                lit += s[i++];
            if (lit != "null") fail("bad literal");
            cell.is_null = true;
            cell.is_string = false;
            cell.value.clear();
        } else {
            // number
            std::string num;
            while (i < s.size()) {
                char d = s[i];
                if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' ||
                    d == 'e' || d == 'E') { num += d; i++; }
                else break;
            }
            if (num.empty()) fail("bad value");
            cell.value = num;
            cell.is_string = false;
            cell.is_null = false;
        }
    }

    Row parse_object() {
        expect('{');
        Row row;
        if (peek() == '}') { i++; return row; }
        while (true) {
            Cell cell;
            cell.key = parse_string();
            expect(':');
            parse_value(cell);
            row.push_back(std::move(cell));
            char c = next();
            if (c == '}') break;
            if (c != ',') fail("expected ',' or '}'");
        }
        return row;
    }

    std::vector<Row> parse_array() {
        expect('[');
        std::vector<Row> rows;
        if (peek() == ']') { i++; return rows; }
        while (true) {
            rows.push_back(parse_object());
            char c = next();
            if (c == ']') break;
            if (c != ',') fail("expected ',' or ']'");
        }
        return rows;
    }
};

} // namespace

std::vector<Row> ParseRows(const std::string &json_array) {
    Parser p(json_array);
    auto rows = p.parse_array();
    p.skip_ws();
    return rows;
}

} // namespace json
} // namespace erpl_rev
