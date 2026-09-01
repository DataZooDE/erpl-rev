// A thin wrapper around the `erpl-adt` CLI, which is how erpl-rev talks to the
// ABAP repository (create objects, write and activate source, run classruns).
//
// Why shell out rather than link erpl-adt's client: erpl-adt builds everything
// under src/ into one static library that also drags in DuckDB, ftxui and
// argparse, and exports no CMake package. Linking it would mean vendoring that
// whole tree to reach four HTTP calls. The CLI is already published to PyPI, and
// anyone who installed erpl-rev with `uvx` has `uv` by definition -- so `uvx
// erpl-adt` costs nothing on the path most users take, and where it is missing
// setup degrades to printing the manual runbook instead of failing.
#pragma once

#include <string>
#include <vector>

namespace erpl_rev::adt {

// Where and as whom to talk to the ABAP system.
struct Conn {
    std::string host = "localhost";
    std::string port = "50000";
    std::string client = "001";
    std::string user;
    std::string password;
    bool https = false;
    // erpl-adt defaults its read timeout to 120s, which truncates CDS activation
    // and surfaces as an opaque "Failed to read connection" while the work
    // carries on server-side. See DataZooDE/erpl-adt#42.
    int timeout_s = 600;
};

struct Result {
    int exit_code = -1;
    std::string output;        // stdout and stderr, interleaved
    bool spawn_failed = false; // the tool could not be started at all
    bool ok() const { return exit_code == 0 && !spawn_failed; }
};

// Run an arbitrary command, capturing stdout+stderr. Exposed because doctor uses
// it for `uv --version` as well as for erpl-adt.
Result RunCapture(const std::vector<std::string> &argv);

// True if `uvx` can be found and can run erpl-adt.
bool ToolAvailable();
// The erpl-adt version string, or empty if unavailable.
std::string ToolVersion();

// erpl-adt invocations. Each returns the raw result so callers can inspect the
// output -- a classrun that hits an ABAP short dump comes back as HTTP 500 with
// the dump text in the body, so the status alone is not enough to report on.
Result Run(const Conn &c, const std::vector<std::string> &args);

// `object create` + `source write --activate`, the pair deploy-abap.sh uses.
// `adt_type` is e.g. "CLAS/OC"; `src_type` is the optional --type for source
// write ("TABL", "INTF", "DDLS") and is empty for classes and programs.
Result CreateObject(const Conn &c, const std::string &adt_type, const std::string &name,
                    const std::string &package, const std::string &description,
                    const std::string &transport = "");
Result WriteSource(const Conn &c, const std::string &name, const std::string &file,
                   const std::string &src_type = "", const std::string &transport = "");

// Run an IF_OO_ADT_CLASSRUN class and return its console output.
Result RunClass(const Conn &c, const std::string &name);

// Delete an object. Note this takes a URI -- /sap/bc/adt/oo/classes/<name> --
// and not the --type/--name pair the create side uses.
Result DeleteObject(const Conn &c, const std::string &uri);

// True when a `source write --activate` actually succeeded. "Nothing to
// activate" is what an UNCHANGED object returns and is success, not failure --
// matching only "Activated" makes every re-run look broken, which teaches
// people to ignore the output.
bool ActivationSucceeded(const Result &r);

} // namespace erpl_rev::adt
