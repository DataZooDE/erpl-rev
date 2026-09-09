#include "load_type.hpp"

#include <cctype>
#include <stdexcept>

namespace erpl_rev {

LoadPlan PlanLoad(LoadType t) {
    LoadPlan p;
    switch (t) {
        case LoadType::Full:
            // A repair: re-read everything, leave the position alone. Moving it
            // would turn "fix the data" into "re-seed the delta", which is a
            // different and much larger action than the operator asked for.
            p.truncate_target = true;
            p.read_rows = true;
            p.apply_floor = false;
            break;
        case LoadType::InitOnly:
            // Adopt a position for a target already populated elsewhere. Reads
            // nothing, so it must not truncate either.
            p.read_rows = false;
            p.truncate_target = false;
            p.apply_floor = false;
            p.seed_watermark = true;
            break;
        case LoadType::InitAndFull:
            p.truncate_target = true;
            p.read_rows = true;
            p.apply_floor = false;
            p.advance_watermark = true;
            break;
        case LoadType::Delta:
            p.read_rows = true;
            p.apply_floor = true;
            p.advance_watermark = true;
            break;
    }
    return p;
}

namespace {
std::string UpperOne(const std::string &s) {
    if (s.size() != 1) return s;
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(s[0]))));
}
}  // namespace

bool IsValidLoadTypeCode(const std::string &code) {
    const auto c = UpperOne(code);
    return c == "F" || c == "I" || c == "L" || c == "D";
}

LoadType ParseLoadType(const std::string &code) {
    const auto c = UpperOne(code);
    if (c == "F") return LoadType::Full;
    if (c == "I") return LoadType::InitOnly;
    if (c == "L") return LoadType::InitAndFull;
    if (c == "D") return LoadType::Delta;
    throw std::runtime_error(
        "unknown load type '" + code +
        "'. Use F (full reload, watermark untouched), I (init without data), "
        "L (init + full load) or D (delta, the default).");
}

std::string LoadTypeCode(LoadType t) {
    switch (t) {
        case LoadType::Full: return "F";
        case LoadType::InitOnly: return "I";
        case LoadType::InitAndFull: return "L";
        case LoadType::Delta: return "D";
    }
    return "D";
}

LoadType DefaultLoadType() { return LoadType::Delta; }

}  // namespace erpl_rev
