// Tests for the erpl-adt launcher probe.
//
// doctor blamed a missing uv for every `uvx erpl-adt --version` failure,
// including cache and download failures where uv ran fine and the remedy is
// completely different. ProbeTool separates "the launcher could not start"
// from "it ran and failed"; passing the argv keeps the tests off the cached
// global launcher resolution.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "adt.hpp"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("RunCapture reports a nonexistent explicit path as spawn failure", "[adt]") {
    // Decided without consulting a shell, so the verdict is identical on
    // POSIX sh (exit 127) and Windows cmd.exe (exit 1): an explicit tool
    // path that does not exist never ran.
    const erpl_rev::adt::Result r =
        erpl_rev::adt::RunCapture({"/nonexistent-dir/erpl-adt-missing", "--version"});
    CHECK(r.spawn_failed);
    CHECK_FALSE(r.ok());
}

TEST_CASE("tool probe reports a missing launcher as not-ran", "[adt]") {
    const erpl_rev::adt::ToolProbe p =
        erpl_rev::adt::ProbeTool({"/nonexistent-dir/erpl-adt-missing"});
    CHECK_FALSE(p.ran);
    CHECK(p.version.empty());
    CHECK_THAT(p.diagnostic, ContainsSubstring("could not be started"));
}

#ifndef _WIN32
TEST_CASE("tool probe reports a failing version run with its exit", "[adt]") {
    // `sh -c "exit 3"` spawns and exits 3 with no output: the launcher ran,
    // the version did not. (/bin/sh is the portable choice: /bin/false is
    // absent on some runners, where the shell reports 127 and the probe
    // reads that as "could not be started". Unix-only: there is no
    // guaranteed failing executable on Windows.)
    const erpl_rev::adt::ToolProbe p =
        erpl_rev::adt::ProbeTool({"/bin/sh", "-c", "exit 3"});
    CHECK(p.ran);
    CHECK(p.version.empty());
    CHECK_THAT(p.diagnostic, ContainsSubstring("exit 3"));
}
#endif
