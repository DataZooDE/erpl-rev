#include "validation.hpp"

#include <algorithm>
#include <cctype>

namespace erpl_rev {
namespace validation {

namespace {
std::string Lower(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}
std::string Upper(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}
}  // namespace

std::string DuckExpr(const Field &f) {
    const auto c = Lower(f.name);
    const auto t = Upper(f.datatype);

    // Everything coalesces: a NULL anywhere in the concatenation swallows the
    // whole row fingerprint, and two different rows would then compare equal.
    if (t == "RAW" || t == "LRAW" || t == "RSTR")
        // Binary compared as text depends on encoding and on trailing-zero
        // handling; hex is exact.
        return "coalesce(hex(" + c + "),'')";
    if (t == "DATS" || t == "TIMS" || t == "INT1" || t == "INT2" || t == "INT4" ||
        t == "INT8" || t == "DEC" || t == "CURR" || t == "QUAN")
        return "coalesce(cast(" + c + " as varchar),'')";
    // SAP pads CHAR to the field length and DuckDB does not, so without rtrim
    // every character column differs on every row.
    return "coalesce(rtrim(" + c + "),'')";
}

bool IsComparable(const Field &f) { return Upper(f.datatype) != "FLTP"; }

std::string RowFingerprint(const std::vector<Field> &fields) {
    std::string expr;
    for (const auto &f : fields) {
        if (!IsComparable(f)) continue;
        // The separator matters: without it ('AB','C') and ('A','BC')
        // fingerprint identically.
        if (!expr.empty()) expr += " || '|' || ";
        expr += DuckExpr(f);
    }
    return expr;
}

Plan BuildPlan(const Policy &p, const std::string &target, const std::vector<Field> &fields,
               const std::vector<std::string> &keys) {
    Plan plan;
    std::string fp = RowFingerprint(fields);
    if (p.hash) fp = "md5(" + fp + ")";

    // The row's identity, rendered the same way on both sides. Falls back to the
    // fingerprint itself when the registration has no keys -- identical rows
    // still pair, and a row on one side only is still reported.
    std::string key;
    for (const auto &k : keys) {
        const auto lk = Lower(k);
        for (const auto &f : fields) {
            if (Lower(f.name) != lk) continue;
            if (!key.empty()) key += " || '|' || ";
            key += DuckExpr(f);
            break;
        }
    }
    if (key.empty()) key = fp;

    std::string order;
    for (const auto &f : fields) {
        if (!IsComparable(f)) continue;
        if (!order.empty()) order += ",";
        order += Lower(f.name);
    }

    // Ordered, always: two unordered result sets cannot be compared row by row.
    plan.sql = "SELECT " + key + " AS k, " + fp + " AS fp FROM " + target;
    if (!order.empty()) plan.sql += " ORDER BY " + order;
    if (p.mode == Mode::Sample) plan.sql += " LIMIT " + std::to_string(p.sample_rows);
    return plan;
}

bool Passed(const Result &r) { return r.mismatched_keys.empty(); }

std::string Verdict(const Result &r) {
    if (Passed(r)) return "validation passed: " + std::to_string(r.compared) + " rows compared";
    // "1000 rows differ" is not actionable; the keys are what an operator needs.
    std::string s = "validation FAILED: " + std::to_string(r.mismatched_keys.size()) + " of " +
                    std::to_string(r.compared) + " rows differ. Keys: ";
    for (size_t i = 0; i < r.mismatched_keys.size() && i < 20; ++i) {
        if (i) s += ", ";
        s += r.mismatched_keys[i];
    }
    if (r.mismatched_keys.size() > 20) s += ", ...";
    return s;
}

}  // namespace validation
}  // namespace erpl_rev
