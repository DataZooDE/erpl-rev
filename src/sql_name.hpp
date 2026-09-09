// SQL identifier sanitizing, in one place.
//
// Two different jobs, deliberately separated:
//
//   Token()       the ZCDC_* object-name rule. Uppercase, every non-alphanumeric
//                 character folded to '_'. NOT injective -- MY-TAB and MY_TAB
//                 both give MY_TAB -- but it must not change: installed systems
//                 carry ZCDC_* triggers, sequences and log tables named by it,
//                 and a "better" rule orphans every one of them.
//
//   UniqueToken() for namespaces whose inputs are customer-chosen names and
//                 where a collision would mean two targets sharing one table
//                 (the per-target change log). Keeps Token()'s output whenever
//                 that is already faithful, and otherwise appends a short digest
//                 of the original so distinct inputs stay distinct.
//
//   QuoteIdent()  a quoted identifier, with an embedded quote doubled. The
//                 dialect's old private Quote() concatenated blindly, which an
//                 identifier carrying a '"' would have escaped out of.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace erpl_rev {
namespace sqlname {

inline std::string Upper(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

inline std::string Token(const std::string &s) {
    std::string r = Upper(s);
    for (char &c : r)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    return r;
}

inline std::string QuoteIdent(const std::string &id) {
    std::string r = "\"";
    for (char c : id) {
        if (c == '"') r += '"';   // double it, per SQL
        r += c;
    }
    r += '"';
    return r;
}

// FNV-1a, so the suffix is stable across runs, platforms and restarts -- a
// change-log table name has to survive the server being restarted.
inline std::string ShortDigest(const std::string &s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    static const char *hex = "0123456789ABCDEF";
    std::string r;
    for (int i = 0; i < 6; ++i) r += hex[(h >> (i * 4)) & 0xF];
    return r;
}

inline std::string UniqueToken(const std::string &s) {
    const std::string up = Upper(s);
    const std::string tok = Token(s);
    // Case folding alone is not a collision worth separating: SAP object names
    // are case-insensitive, so SFLIGHT and sflight ARE the same name. Only a
    // character actually rewritten by Token() earns a suffix.
    if (tok == up) return tok;
    return tok + "_" + ShortDigest(up);
}

}  // namespace sqlname
}  // namespace erpl_rev
