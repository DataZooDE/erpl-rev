// The four load types a replication run can be started as.
//
//   F  full reload, delta position untouched -- a data repair
//   I  init without data: adopt a position, transfer nothing
//   L  init + full load: seed the target and the position together
//   D  delta (default)
//
// Kept as a plan struct rather than a switch scattered through the runner, so
// the combinations that must never occur (seeding and advancing the watermark in
// one run; truncating a target the run is not going to refill) are testable.
#pragma once

#include <string>

namespace erpl_rev {

enum class LoadType { Full, InitOnly, InitAndFull, Delta };

struct LoadPlan {
    bool truncate_target = false;
    bool read_rows = true;
    bool apply_floor = true;        // false = read everything, not a window
    bool advance_watermark = false; // to the cycle ceiling
    bool seed_watermark = false;    // from the source's current position
};

LoadPlan PlanLoad(LoadType t);

LoadType ParseLoadType(const std::string &code);
bool IsValidLoadTypeCode(const std::string &code);
std::string LoadTypeCode(LoadType t);
LoadType DefaultLoadType();

}  // namespace erpl_rev
