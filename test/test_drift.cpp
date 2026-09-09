// Structure watchdog: the DDIC field list against the replicated target.
//
// The field list is what zcl_erpl_rev_util=>describe_table already computes for
// every replication, so it rides along on the first package rather than costing
// a second round trip -- and there is no second implementation of "what does
// this table look like".
//
// The asymmetry is the point: a new column can be added to a target safely, so
// it is. A removed or retyped column cannot, so the target is BLOCKED until an
// operator decides, rather than quietly writing rows against a schema known to
// disagree.

#include <catch2/catch_test_macros.hpp>

#include "drift.hpp"

using namespace erpl_rev::drift;

namespace {
Schema Ddic() {
    return {{"MANDT", "CLNT", 3, 0}, {"ID", "INT4", 10, 0}, {"V", "CHAR", 20, 0}};
}
Schema Target() {
    return {{"MANDT", "CLNT", 3, 0}, {"ID", "INT4", 10, 0}, {"V", "CHAR", 20, 0}};
}
}  // namespace

TEST_CASE("drift: an unchanged structure produces no plan", "[drift]") {
    const auto d = Diff(Ddic(), Target());
    CHECK(d.added.empty());
    CHECK(d.removed.empty());
    CHECK(d.retyped.empty());
    CHECK_FALSE(d.blocked);
    CHECK(AlterPlan(d, "t").empty());
}

TEST_CASE("drift: a new column is added automatically", "[drift]") {
    auto ddic = Ddic();
    ddic.push_back({"NEWCOL", "CHAR", 10, 0});
    const auto d = Diff(ddic, Target());

    REQUIRE(d.added.size() == 1);
    CHECK(d.added[0].name == "NEWCOL");
    CHECK_FALSE(d.blocked);   // appending is safe
    const auto plan = AlterPlan(d, "t");
    REQUIRE(plan.size() == 1);
    CHECK(plan[0] == "ALTER TABLE t ADD COLUMN NEWCOL VARCHAR");
}

TEST_CASE("drift: a removed column blocks the target", "[drift]") {
    // Dropping it would destroy replicated data; ignoring it means every later
    // package writes a column the source no longer has. Neither is ours to
    // choose, so the run stops and an operator decides.
    auto ddic = Ddic();
    ddic.pop_back();
    const auto d = Diff(ddic, Target());

    REQUIRE(d.removed.size() == 1);
    CHECK(d.removed[0].name == "V");
    CHECK(d.blocked);
    CHECK(AlterPlan(d, "t").empty());   // nothing is applied automatically
    CHECK(Explain(d).find("V") != std::string::npos);
}

TEST_CASE("drift: a retyped column blocks the target", "[drift]") {
    auto ddic = Ddic();
    ddic[2].datatype = "INT4";          // CHAR -> INT4
    const auto d = Diff(ddic, Target());

    REQUIRE(d.retyped.size() == 1);
    CHECK(d.retyped[0].name == "V");
    CHECK(d.blocked);
    CHECK(AlterPlan(d, "t").empty());
}

TEST_CASE("drift: a widened column is a retype too", "[drift]") {
    // CHAR(20) -> CHAR(40) silently truncates on every later package if the
    // target is left alone, which is data loss that no error reports.
    auto ddic = Ddic();
    ddic[2].length = 40;
    const auto d = Diff(ddic, Target());
    REQUIRE(d.retyped.size() == 1);
    CHECK(d.blocked);
}

TEST_CASE("drift: reordering columns is not a change", "[drift]") {
    // DDIC order is not the target's order and never has been; treating a
    // reorder as drift would block every target on a cosmetic edit.
    auto ddic = Ddic();
    std::swap(ddic[1], ddic[2]);
    const auto d = Diff(ddic, Target());
    CHECK(d.added.empty());
    CHECK(d.removed.empty());
    CHECK(d.retyped.empty());
    CHECK_FALSE(d.blocked);
}

TEST_CASE("drift: comparison ignores case", "[drift]") {
    auto target = Target();
    for (auto &f : target) for (char &c : f.name) c = static_cast<char>(::tolower(c));
    const auto d = Diff(Ddic(), target);
    CHECK(d.added.empty());
    CHECK(d.removed.empty());
    CHECK_FALSE(d.blocked);
}

TEST_CASE("drift: several additions all land in one plan", "[drift]") {
    auto ddic = Ddic();
    ddic.push_back({"A1", "CHAR", 5, 0});
    ddic.push_back({"A2", "DEC", 15, 2});
    const auto d = Diff(ddic, Target());
    const auto plan = AlterPlan(d, "t");
    CHECK(plan.size() == 2);
    CHECK(plan[1].find("DECIMAL(15,2)") != std::string::npos);
}

TEST_CASE("drift: an addition alongside a removal still blocks", "[drift]") {
    // The safe half must not be applied while the unsafe half is outstanding, or
    // the target ends up in a shape nobody chose.
    auto ddic = Ddic();
    ddic.pop_back();
    ddic.push_back({"NEWCOL", "CHAR", 10, 0});
    const auto d = Diff(ddic, Target());
    CHECK(d.blocked);
    CHECK(AlterPlan(d, "t").empty());
}
