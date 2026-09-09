#include "drift.hpp"

#include <algorithm>
#include <cctype>

namespace erpl_rev {
namespace drift {

namespace {
std::string Upper(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

const Field *Find(const Schema &s, const std::string &name) {
    const auto up = Upper(name);
    for (const auto &f : s)
        if (Upper(f.name) == up) return &f;
    return nullptr;
}

// Same column, different shape. Length counts: CHAR(20) -> CHAR(40) left alone
// truncates every later package, which is data loss no error reports.
bool SameShape(const Field &a, const Field &b) {
    return Upper(a.datatype) == Upper(b.datatype) && a.length == b.length &&
           a.decimals == b.decimals;
}
}  // namespace

std::string DuckType(const Field &f) {
    const auto t = Upper(f.datatype);
    if (t == "INT1" || t == "INT2" || t == "INT4") return "INTEGER";
    if (t == "INT8") return "BIGINT";
    if (t == "DEC" || t == "CURR" || t == "QUAN")
        return "DECIMAL(" + std::to_string(f.length) + "," + std::to_string(f.decimals) + ")";
    if (t == "FLTP") return "DOUBLE";
    if (t == "DATS") return "DATE";
    if (t == "TIMS") return "TIME";
    if (t == "RAW" || t == "LRAW") return "BLOB";
    return "VARCHAR";
}

DiffResult Diff(const Schema &ddic, const Schema &target) {
    DiffResult d;

    for (const auto &f : ddic) {
        const Field *t = Find(target, f.name);
        if (!t) d.added.push_back(f);
        else if (!SameShape(f, *t)) d.retyped.push_back(f);
    }
    for (const auto &t : target)
        if (!Find(ddic, t.name)) d.removed.push_back(t);

    // Column ORDER is deliberately not compared: DDIC order has never been the
    // target's order, and treating a reorder as drift would block every target
    // on a cosmetic edit to the source.
    d.blocked = !d.removed.empty() || !d.retyped.empty();
    return d;
}

std::vector<std::string> AlterPlan(const DiffResult &d, const std::string &target) {
    if (d.blocked) return {};
    std::vector<std::string> out;
    for (const auto &f : d.added)
        out.push_back("ALTER TABLE " + target + " ADD COLUMN " + f.name + " " + DuckType(f));
    return out;
}

std::string Explain(const DiffResult &d) {
    std::string s;
    auto list = [&](const char *what, const std::vector<Field> &v) {
        if (v.empty()) return;
        s += std::string(what) + ": ";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += ", ";
            s += v[i].name;
        }
        s += ". ";
    };
    list("added", d.added);
    list("removed from the source", d.removed);
    list("retyped", d.retyped);
    if (d.blocked)
        s += "The target is blocked: a removed or retyped column cannot be applied "
             "without deciding what happens to already-replicated data. Accept the "
             "change (re-seed the target) or waive it.";
    return s;
}

}  // namespace drift
}  // namespace erpl_rev
