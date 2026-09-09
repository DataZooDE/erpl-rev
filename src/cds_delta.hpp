// Delta configuration derived from a CDS view's own annotations.
//
// A delta-enabled CDS view already declares how it should be extracted. Reading
// that means a target configures itself, instead of an operator restating what
// the model says and getting it subtly wrong.
//
// The annotation VALUES arrive as data -- an ABAP DDIC/annotation read -- not as
// DDL source text. Parsing the source would require developer authorisations on
// the customer system, the exact dependency the pre-deployed command driver was
// built to remove. Parsing the values is pure, and therefore unit-testable.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace erpl_rev {
namespace cds {

enum class DeltaKind { None, Watermark, Trigger };

struct BaseTable {
    std::string table;
    std::string keys;
    std::string mode;
};

struct Annotations {
    std::string entity;
    std::map<std::string, std::string> values;         // annotation -> value
    std::map<std::string, std::string> element_types;  // element -> DDIC type
    std::vector<std::pair<std::string, std::string>> mappings;  // base table -> keys
};

struct Derived {
    DeltaKind kind = DeltaKind::None;
    std::string chg_col;
    std::string wm_kind;
    long long safety_secs = 120;
    std::vector<BaseTable> trigger_targets;
};

Derived Derive(const Annotations &a);

// DDIC element type -> the watermark kind that compares it correctly.
std::string WmKindForType(const std::string &ddic_type);

}  // namespace cds
}  // namespace erpl_rev
