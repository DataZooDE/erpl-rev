#include "abap_codegen.hpp"

#include <algorithm>
#include <random>
#include <sstream>

namespace erpl_rev::abapgen {

UnsafeValue::UnsafeValue(const std::string &f, const std::string &why)
    : std::runtime_error(f + ": " + why), field(f) {}

namespace {

// Decode one UTF-8 sequence starting at i. Returns its length, or 0 if the
// bytes there are not a well-formed sequence. Rejects overlong encodings and
// the surrogate range, both of which are how a validator gets bypassed.
size_t Utf8Len(const std::string &v, size_t i) {
    const auto b = static_cast<unsigned char>(v[i]);
    size_t n = 0;
    unsigned cp = 0;
    if (b < 0x80)                  { return 1; }
    else if ((b & 0xE0) == 0xC0)   { n = 2; cp = b & 0x1Fu; }
    else if ((b & 0xF0) == 0xE0)   { n = 3; cp = b & 0x0Fu; }
    else if ((b & 0xF8) == 0xF0)   { n = 4; cp = b & 0x07u; }
    else                           { return 0; }   // continuation or 5+ byte lead

    if (i + n > v.size()) return 0;
    for (size_t k = 1; k < n; k++) {
        const auto c = static_cast<unsigned char>(v[i + k]);
        if ((c & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (c & 0x3Fu);
    }
    if (n == 2 && cp < 0x80) return 0;             // overlong
    if (n == 3 && cp < 0x800) return 0;            // overlong
    if (n == 4 && cp < 0x10000) return 0;          // overlong
    if (cp > 0x10FFFF) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;    // surrogate half
    return n;
}

void Check(const std::string &v, const std::string &field, size_t max_bytes,
           bool allow_newline) {
    if (v.size() > max_bytes) {
        throw UnsafeValue(field, "is " + std::to_string(v.size()) + " bytes; ABAP source "
                                 "cannot carry a value that large (limit " +
                                 std::to_string(max_bytes) + "). Put the predicate in a "
                                 "CDS view, or filter in DuckDB after the load.");
    }
    for (size_t i = 0; i < v.size();) {
        const auto c = static_cast<unsigned char>(v[i]);
        if (c < 0x20 || c == 0x7F) {
            if (allow_newline && (c == '\n' || c == '\r')) { i++; continue; }
            std::ostringstream m;
            m << "contains a control character (0x" << std::hex << int(c) << ") at byte "
              << std::dec << i << ". A newline in a value would end the generated ABAP "
                                  "statement, so it is refused rather than escaped.";
            throw UnsafeValue(field, m.str());
        }
        const size_t n = Utf8Len(v, i);
        if (n == 0)
            throw UnsafeValue(field, "is not valid UTF-8 at byte " + std::to_string(i) +
                                     ". The generated source is written as UTF-8.");
        i += n;
    }
}

// Split an already-escaped literal body so no emitted line exceeds the limit,
// joining the pieces with ABAP's `&&`. Splits only on UTF-8 boundaries.
std::string Chunk(const std::string &escaped, char quote, size_t budget) {
    if (escaped.size() <= budget) return std::string(1, quote) + escaped + quote;

    std::vector<std::string> parts;
    size_t i = 0;
    while (i < escaped.size()) {
        size_t take = std::min(budget, escaped.size() - i);
        // Never cut inside a multi-byte sequence: walk back to a lead byte.
        while (take > 0 && (static_cast<unsigned char>(escaped[i + take]) & 0xC0) == 0x80)
            take--;
        // Never cut between the two halves of a doubled quote, or the escape
        // stops meaning what it meant.
        while (take > 0 && escaped[i + take - 1] == quote &&
               (take < 2 || escaped[i + take - 2] != quote))
            take--;
        if (take == 0) take = std::min(budget, escaped.size() - i);   // pathological; emit anyway
        parts.push_back(std::string(1, quote) + escaped.substr(i, take) + quote);
        i += take;
    }
    std::string out;
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += " &&\n    ";
        out += parts[k];
    }
    return out;
}

std::string Double(const std::string &v, char q) {
    std::string out;
    out.reserve(v.size() + 8);
    for (char c : v) {
        if (c == q) out += q;
        out += c;
    }
    return out;
}

} // namespace

bool IsEmbeddable(const std::string &v, std::string *why) {
    try {
        Check(v, "value", kMaxValueBytes, /*allow_newline=*/false);
        return true;
    } catch (const UnsafeValue &e) {
        if (why) *why = e.what();
        return false;
    }
}

std::string Backtick(const std::string &v, const std::string &field, size_t max_bytes) {
    Check(v, field, max_bytes, /*allow_newline=*/false);
    return Chunk(Double(v, '`'), '`', kMaxLineChars - 20);
}

std::string Apostrophe(const std::string &v, const std::string &field, size_t max_bytes) {
    Check(v, field, max_bytes, /*allow_newline=*/false);
    if (!v.empty() && v.back() == ' ') {
        throw UnsafeValue(field, "ends with a blank. ABAP strips trailing blanks from "
                                 "'...' literals, which would silently change the value. "
                                 "Use a text-string literal instead.");
    }
    return Chunk(Double(v, '\''), '\'', kMaxLineChars - 20);
}

std::string TemplateBody(const std::string &v, const std::string &field, size_t max_bytes) {
    Check(v, field, max_bytes, /*allow_newline=*/false);
    std::string out;
    out.reserve(v.size() + 8);
    for (char c : v) {
        if (c == '\\' || c == '|' || c == '{' || c == '}') out += '\\';
        out += c;
    }
    return out;
}

std::string Int(long long v) { return std::to_string(v); }
std::string Bool(bool v) { return v ? "abap_true" : "abap_false"; }

std::vector<std::string> MultilineTemplate(const std::string &text,
                                           const std::string &indent,
                                           const std::string &field) {
    Check(text, field, kMaxValueBytes * 8, /*allow_newline=*/true);

    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    lines.push_back(cur);
    while (lines.size() > 1 && lines.back().empty()) lines.pop_back();

    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string body;
        for (char c : lines[i]) {
            if (c == '\\' || c == '|' || c == '{' || c == '}') body += '\\';
            body += c;
        }
        // A trailing space on every line but the last, so the concatenation is
        // still valid SQL rather than gluing two tokens together.
        const bool last = (i + 1 == lines.size());
        out.push_back(indent + "|" + body + (last ? "" : " ") + "|" + (last ? "" : " &&"));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Template
// ---------------------------------------------------------------------------

Template::Template(std::string skeleton) : skeleton_(std::move(skeleton)) {}

Template &Template::Set(const std::string &key, const std::string &escaped) {
    const std::string ph = "$ERPL_" + key + "$";
    if (skeleton_.find(ph) == std::string::npos) {
        // The bug this guards against: someone renames a placeholder in the
        // skeleton, the substitution silently stops happening, and the
        // generated program runs with a stale hardcoded value.
        throw std::logic_error("abapgen: placeholder " + ph +
                               " does not occur in the skeleton");
    }
    bound_.emplace_back(ph, escaped);
    return *this;
}

std::string Template::Render() const {
    std::string out;
    out.reserve(skeleton_.size() * 2);

    // One left-to-right pass. Substituted text goes straight into the output
    // and is never looked at again, so a value equal to "$ERPL_WHERE$" is
    // emitted literally rather than treated as a placeholder.
    size_t i = 0;
    while (i < skeleton_.size()) {
        if (skeleton_.compare(i, 6, "$ERPL_") == 0) {
            const size_t end = skeleton_.find('$', i + 6);
            if (end != std::string::npos) {
                const std::string ph = skeleton_.substr(i, end - i + 1);
                auto it = std::find_if(bound_.begin(), bound_.end(),
                                       [&](const auto &b) { return b.first == ph; });
                if (it == bound_.end())
                    throw std::logic_error("abapgen: placeholder " + ph + " was never bound");
                out += it->second;
                i = end + 1;
                continue;
            }
        }
        out += skeleton_[i++];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

std::string MakeNonce() {
    static const char *hex = "0123456789abcdef";
    std::random_device rd;
    std::string s;
    s.reserve(8);
    for (int i = 0; i < 8; i++) s += hex[rd() & 0xF];
    return s;
}

std::vector<std::pair<std::string, std::string>>
ResultLines(const std::string &output, const std::string &nonce) {
    const std::string prefix = "ERPL-CLI/" + nonce + " ";
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos <= output.size()) {
        const size_t eol = output.find('\n', pos);
        const size_t len = (eol == std::string::npos ? output.size() : eol) - pos;
        std::string line = output.substr(pos, len);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, prefix.size(), prefix) == 0) {
            const std::string rest = line.substr(prefix.size());
            const auto eq = rest.find('=');
            if (eq != std::string::npos)
                out.emplace_back(rest.substr(0, eq), rest.substr(eq + 1));
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

std::string ResultField(const std::string &output, const std::string &nonce,
                        const std::string &key) {
    for (const auto &[k, v] : ResultLines(output, nonce))
        if (k == key) return v;
    return {};
}

} // namespace erpl_rev::abapgen
