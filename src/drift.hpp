// Structure watchdog: the source's DDIC field list against the replicated target.
//
// The asymmetry is deliberate. A new column can be appended to a target without
// touching existing rows, so it is applied automatically. A removed or retyped
// column cannot be handled without deciding what happens to replicated data --
// so the target is BLOCKED and an operator decides, rather than the engine
// quietly writing rows against a schema it knows disagrees.
#pragma once

#include <string>
#include <vector>

namespace erpl_rev {
namespace drift {

struct Field {
    std::string name;
    std::string datatype;   // DDIC type: CHAR, INT4, DEC, DATS, ...
    int length = 0;
    int decimals = 0;
};

using Schema = std::vector<Field>;

struct DiffResult {
    std::vector<Field> added;
    std::vector<Field> removed;
    std::vector<Field> retyped;
    bool blocked = false;   // removed or retyped: not safe to apply automatically
};

DiffResult Diff(const Schema &ddic, const Schema &target);

// The ALTERs to apply. Empty when blocked: the safe half of a diff must not be
// applied while the unsafe half is outstanding, or the target ends up in a shape
// nobody chose.
std::vector<std::string> AlterPlan(const DiffResult &d, const std::string &target);

// A human-readable account for the operator who has to decide.
std::string Explain(const DiffResult &d);

// DDIC type -> DuckDB type.
std::string DuckType(const Field &f);

}  // namespace drift
}  // namespace erpl_rev
