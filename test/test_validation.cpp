// Data validation: comparing a replicated target against its SAP source.
//
// This logic already existed and was already correct -- inside an end-to-end
// test class. Promoting it to a feature means MOVING it, not writing a second
// copy: two fingerprint implementations that disagree defeat both the test and
// the feature.
//
// The split is deliberate and asymmetric. The DuckDB-side expression is SQL and
// moves here. The SAP-side one formats a DDIC-typed ABAP value into canonical
// text and cannot move -- reimplementing DATS/TIMS/RAW/NUMC/CURR canonicalisation
// in C++ would be ~80 lines that can silently disagree with the ABAP. It stays
// in ABAP, and an ABAP Unit golden test per DDIC type binds the two halves.

#include <catch2/catch_test_macros.hpp>

#include "validation.hpp"

using namespace erpl_rev::validation;

TEST_CASE("validation: every type canonicalises to text, never to NULL", "[validation]") {
    // A NULL anywhere in the concatenation swallows the whole row fingerprint,
    // so two different rows would compare equal. Every branch coalesces.
    for (const char *t : {"DATS", "TIMS", "INT4", "DEC", "CURR", "QUAN", "RAW", "CHAR", "NUMC"}) {
        const auto e = DuckExpr({"col", t, 10, 0});
        INFO("type " << t << " -> " << e);
        CHECK(e.find("coalesce") != std::string::npos);
    }
}

TEST_CASE("validation: binary columns compare as hex", "[validation]") {
    // A RAW column compared as text depends on the client encoding and on
    // trailing-zero handling; hex is exact.
    CHECK(DuckExpr({"data", "RAW", 16, 0}).find("hex(") != std::string::npos);
    CHECK(DuckExpr({"data", "RSTR", 0, 0}).find("hex(") != std::string::npos);
}

TEST_CASE("validation: character columns are right-trimmed", "[validation]") {
    // SAP pads CHAR to the field length; DuckDB does not. Without rtrim every
    // character column would differ on every row.
    CHECK(DuckExpr({"name", "CHAR", 20, 0}).find("rtrim") != std::string::npos);
}

TEST_CASE("validation: float columns are excluded from comparison", "[validation]") {
    // Binary floating point does not round-trip through decimal text, so
    // comparing it produces false mismatches on correct data.
    CHECK(IsComparable({"x", "CHAR", 1, 0}));
    CHECK_FALSE(IsComparable({"x", "FLTP", 16, 0}));
}

TEST_CASE("validation: a row fingerprint joins columns with a separator", "[validation]") {
    // Without a separator, ('AB','C') and ('A','BC') fingerprint identically.
    const auto sql = RowFingerprint({{"a", "CHAR", 5, 0}, {"b", "CHAR", 5, 0}});
    CHECK(sql.find("'|'") != std::string::npos);
}

TEST_CASE("validation: a plan can sample or take everything", "[validation]") {
    Policy p;
    p.mode = Mode::Sample;
    p.sample_rows = 1000;
    const auto plan = BuildPlan(p, "t", {{"id", "INT4", 10, 0}});
    CHECK(plan.sql.find("LIMIT 1000") != std::string::npos);

    p.mode = Mode::Full;
    CHECK(BuildPlan(p, "t", {{"id", "INT4", 10, 0}}).sql.find("LIMIT") == std::string::npos);
}

TEST_CASE("validation: a plan is ordered, or the comparison is meaningless",
          "[validation]") {
    // Two unordered result sets cannot be compared row by row.
    Policy p;
    p.mode = Mode::Full;
    const auto plan = BuildPlan(p, "t", {{"id", "INT4", 10, 0}, {"v", "CHAR", 5, 0}});
    CHECK(plan.sql.find("ORDER BY") != std::string::npos);
}

TEST_CASE("validation: MD5 mode hashes the row rather than shipping it", "[validation]") {
    Policy p;
    p.mode = Mode::Full;
    p.hash = true;
    const auto plan = BuildPlan(p, "t", {{"id", "INT4", 10, 0}});
    CHECK(plan.sql.find("md5(") != std::string::npos);
}

TEST_CASE("validation: a verdict names the offending keys", "[validation]") {
    // "1000 rows differ" is not actionable; the keys are.
    Result r;
    r.compared = 3;
    r.mismatched_keys = {"0000000017", "0000000042"};
    const auto v = Verdict(r);
    CHECK(v.find("0000000017") != std::string::npos);
    CHECK(v.find("0000000042") != std::string::npos);
    CHECK_FALSE(Passed(r));

    r.mismatched_keys.clear();
    CHECK(Passed(r));
}

TEST_CASE("validation: the plan emits a key, so rows are paired not counted",
          "[validation]") {
    // Positional comparison assumed two engines given the same ORDER BY produce
    // the same sequence. They need not: collation, NULL placement and
    // numeric-versus-text ordering all differ between HANA and DuckDB. Paired
    // wrongly, the comparison reports FAILED on correct data -- and, worse, can
    // report PASSED when row i on one side happens to match row i on the other
    // while both sets genuinely differ.
    //
    // A key makes the pairing explicit: each side says which row it is talking
    // about, and a row present on one side and missing on the other is a
    // mismatch rather than an off-by-one that shifts everything after it.
    Policy p;
    std::vector<Field> fields{
        {"MANDT", "CLNT", 3, 0}, {"BELNR", "CHAR", 10, 0}, {"DMBTR", "CURR", 23, 2}};

    const auto plan = BuildPlan(p, "t", fields, {"MANDT", "BELNR"});

    CHECK(plan.sql.find(" AS k") != std::string::npos);
    CHECK(plan.sql.find(" AS fp") != std::string::npos);
    // The key is built from the key columns only, in the order given.
    CHECK(plan.sql.find("mandt") < plan.sql.find(" AS k"));
    CHECK(plan.sql.find("belnr") < plan.sql.find(" AS k"));
}

TEST_CASE("validation: a keyless target still produces a usable plan",
          "[validation]") {
    // Not every registration has keys, and a validator that throws on one is a
    // feature an operator cannot run at all. Falling back to the whole row as
    // its own key keeps the pairing honest: identical rows pair, and a row that
    // exists on one side only is still reported.
    Policy p;
    std::vector<Field> fields{{"A", "CHAR", 5, 0}};
    const auto plan = BuildPlan(p, "t", fields, {});
    CHECK(plan.sql.find(" AS k") != std::string::npos);
}

TEST_CASE("validation: key parts keep their separators when a value is empty",
          "[validation]") {
    // The two sides must render the SAME key for the same row. Joining only
    // non-empty parts makes ('','B') render as "B" on one side and "|B" on the
    // other, so nothing pairs -- and on a byte-perfect replica full mode then
    // reports every source row missing AND every replica row extra, whose
    // documented remedy is a destructive reload.
    Policy p;
    std::vector<Field> fields{{"A", "CHAR", 5, 0}, {"B", "CHAR", 5, 0}};
    const auto plan = BuildPlan(p, "t", fields, {"A", "B"});
    // Both parts present in order, separated unconditionally.
    const auto k = plan.sql.substr(0, plan.sql.find(" AS k"));
    CHECK(k.find("a") != std::string::npos);
    CHECK(k.find("b") != std::string::npos);
    CHECK(k.find("'|'") != std::string::npos);
}

TEST_CASE("validation: key names are matched case- and blank-insensitively",
          "[validation]") {
    // `sync create --keys "mandt, bukrs"` stores exactly that. If either side
    // matches key names literally, it finds no columns, falls back to something
    // else, and nothing pairs.
    Policy p;
    std::vector<Field> fields{{"MANDT", "CLNT", 3, 0}, {"BUKRS", "CHAR", 4, 0}};
    const auto plan = BuildPlan(p, "t", fields, {"mandt", " BUKRS "});
    const auto k = plan.sql.substr(0, plan.sql.find(" AS k"));
    CHECK(k.find("mandt") != std::string::npos);
    CHECK(k.find("bukrs") != std::string::npos);
}
