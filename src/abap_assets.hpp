// The ABAP objects erpl-rev needs on the SAP side, compiled into the binary.
//
// `uvx erpl-rev setup` has no git checkout to read from, so the sources have to
// travel with the executable. The production set is ~150 KiB, which is nothing
// beside the DuckDB that is already in there.
//
// This is the PRODUCTION set only: no test drivers, no demo reports, and not the
// 100k-row ZWIDE_BSEG fixture. Those belong in a development checkout, not in a
// customer's system.
//
// The order of ProductionAssets() is a dependency order, not alphabetical, and
// each constraint in it was found by something failing:
//   - the progress interface before util, whose signature references it
//   - typemap before util, which depends on it
//   - the worker report before the replicate report, which SUBMITs it
// scripts/deploy-abap.sh carries the same order for the development set.
#pragma once

#include <string_view>
#include <vector>

namespace erpl_rev::abap {

struct Asset {
    std::string_view file;         // original filename, reused for the temp file
    std::string_view name;         // ABAP object name
    std::string_view adt_type;     // ADT creation type, e.g. "CLAS/OC"
    std::string_view src_type;     // --type for `source write`; empty for CLAS/PROG
    std::string_view description;
    std::string_view text;         // the source itself
};

// Deployment order matters; see above.
const std::vector<Asset> &ProductionAssets();

// Look one up by ABAP object name; nullptr if absent.
const Asset *Find(std::string_view name);

} // namespace erpl_rev::abap
