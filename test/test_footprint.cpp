// The delivered object inventory.
//
// "Adding a replication source adds a row in DuckDB, not an object in SAP" is
// erpl-rev's central claim, and it is the kind of property that erodes one
// convenient exception at a time. The embedded asset list is exactly what
// reaches a customer, so it is the right place to assert what ships -- and
// unlike the ABAP-side check it runs everywhere, on every build.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "abap_assets.hpp"

using namespace erpl_rev;

TEST_CASE("footprint: the delivered set is exactly the documented one", "[footprint]") {
    // Changing this list is a deliberate edit with a reviewer, which is the
    // point: a new SAP object should never arrive unnoticed.
    const std::vector<std::string> expected = {
        "ZIF_ERPL_REV_PROGRESS", "ZCL_ERPL_REV_TYPEMAP", "ZCL_ERPL_REV_UTIL",
        "ZCL_ERPL_REV_DELTA",    "ZCL_ERPL_REV_CDC",     "ZCL_ERPL_REV_MKFM",
        "ZCL_ERPL_REV_SETUP",    "ZCL_ERPL_REV_DIAG",    "ZCL_ERPL_REV_CLIDRV",
        "Z_ERPL_REV_REPL_WORKER", "Z_ERPL_REV_REPLICATE", "Z_ERPL_REV_SQL",
        "Z_ERPL_REV_DELTA",      "Z_ERPL_REV_DAEMON",
    };

    std::vector<std::string> actual;
    for (const auto &a : abap::ProductionAssets()) actual.emplace_back(a.name);

    auto sorted = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    CHECK(sorted(actual) == sorted(expected));
}

TEST_CASE("footprint: no test fixture or demo ships", "[footprint]") {
    // Named explicitly rather than pattern-matched: a heuristic on "DRV" also
    // catches ZCL_ERPL_REV_CLIDRV, which is the CLI command driver and very much
    // delivered. Two of these were in the delivered list once, directly
    // contradicting the comment above it saying fixtures are excluded.
    const std::vector<std::string> dev_only = {
        "ZDELTA_WM", "ZDELTA_D", "ZDELTA_DT", "ZWIDE_BSEG",
        "ZCL_ERPL_REV_DELTADRV", "ZCL_ERPL_REV_WMTEST", "ZCL_ERPL_REV_FOOTPRINT",
        "ZCL_ERPL_REV_DIFFTEST", "ZCL_ERPL_REV_DELTATEST", "ZCL_ERPL_REV_CDCTEST",
        "Z_ERPL_REV_DELTA_SFLIGHT",
    };
    for (const auto &a : abap::ProductionAssets()) {
        const std::string n{a.name};
        INFO(n << " is dev scaffolding but is in the delivered set");
        CHECK(std::find(dev_only.begin(), dev_only.end(), n) == dev_only.end());
    }
}

TEST_CASE("footprint: everything is in the customer namespace", "[footprint]") {
    for (const auto &a : abap::ProductionAssets()) {
        const std::string n{a.name};
        INFO(n << " is outside the Z namespace");
        CHECK(n.rfind("Z", 0) == 0);
    }
}
