// Data validation: comparing a replicated target against its SAP source.
//
// The logic existed already, and was already right -- inside an end-to-end test
// class. Promoting it to a feature is a MOVE, not a rewrite: two fingerprint
// implementations that can disagree defeat both the test and the feature.
//
// Only the DuckDB half lives here. The SAP-side expression formats a DDIC-typed
// ABAP value into canonical text and stays in ABAP: reimplementing DATS/TIMS/
// RAW/NUMC/CURR canonicalisation in C++ would be a second copy of ~80 lines of
// DDIC knowledge that can silently drift from the one that ships.
#pragma once

#include <string>
#include <vector>

namespace erpl_rev {
namespace validation {

struct Field {
    std::string name;
    std::string datatype;
    int length = 0;
    int decimals = 0;
};

enum class Mode { Sample, Full };

struct Policy {
    Mode mode = Mode::Sample;
    long long sample_rows = 1000;
    bool hash = false;      // md5 per row instead of the raw text
};

struct Plan {
    std::string sql;
};

struct Result {
    long long compared = 0;
    std::vector<std::string> mismatched_keys;
};

// The canonical-text expression for one column, DuckDB side.
std::string DuckExpr(const Field &f);

// FLTP is excluded: binary floating point does not round-trip through decimal
// text, so comparing it reports mismatches on correct data.
bool IsComparable(const Field &f);

std::string RowFingerprint(const std::vector<Field> &fields);

Plan BuildPlan(const Policy &p, const std::string &target, const std::vector<Field> &fields);

bool Passed(const Result &r);
std::string Verdict(const Result &r);

}  // namespace validation
}  // namespace erpl_rev
