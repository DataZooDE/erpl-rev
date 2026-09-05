// `erpl-rev setup` and `erpl-rev doctor`.
//
// Getting the binary is now one command; getting the SAP side ready was an
// afternoon of reading four documents. These two commands close that gap as far
// as it can honestly be closed:
//
//   doctor  diagnoses the system and never writes to it. "Never writes" means
//           no repository object, destination, config or table is created or
//           changed: the only thing it executes on the system is the classrun
//           ZCL_ERPL_REV_DIAG, whose whole body is a STFC_CONNECTION ping.
//   setup   diagnoses, shows what it intends to change, asks, does it, and then
//           proves a round trip actually worked before claiming success.
//           `--dry-run` stops after printing the plan and is read-only in the
//           same sense as doctor. Every write is preceded by either a terminal
//           confirmation or an explicit `--yes`; `--non-interactive` alone means
//           "do not prompt", not "yes".
//
// Two things genuinely cannot be automated from a client, and pretending
// otherwise would produce confusing failures rather than a working system: the
// gateway's `reginfo` allow-list (an OS file, or SMGW in the GUI) and the
// `gw/acl_mode` / `gw/reg_info` profile parameters (which need an instance
// restart). For those, setup emits a filled-in handout for whoever holds Basis
// rights -- the exact lines, for this host, with nothing left to compose.
#pragma once

#include <functional>

#include "cli_common.hpp"
#include <string>
#include <vector>

namespace erpl_rev::setup {

// Resolved from flags > environment > config file > prompt. The *_set flags
// record "came from the command line" so the resolver can apply that precedence
// without confusing an explicit empty value with an absent one.
struct Options : cli::ConnOptions {
    std::string package;      // "$TMP" or e.g. ZERPL_CORE; empty = decide by diagnosis
    std::string program_id;   // gateway PROGRAM_ID, default ERPL_REV
    std::string gwhost, gwserv;
    bool package_set = false, program_set = false;
    bool gwhost_set = false, gwserv_set = false;
    // Name of the erpl-tunnel secret the SERVER reaches the gateway through,
    // if any. Empty is the normal case. doctor cannot probe such a gateway --
    // the forward lives in the server process, not this one -- so this exists
    // to stop it claiming it can.
    std::string tunnel_secret;
    bool tunnel_secret_set = false;

    bool print_runbook = false;
    bool save_password = false;
};

// Consume one option if it belongs to setup/doctor. Returns false if the flag is
// none of ours, so the caller can report it as unknown.
bool ParseOption(const std::string &key, bool has_inline,
                 const std::function<std::string()> &take_value, Options &o);

// Help text for the setup/doctor flags, appended to the main --help.
void PrintHelp();

// Exit codes: 0 ready / work done, 1 something needs attention, 2 misuse.
int RunDoctor(Options o);
int RunSetup(Options o);

// ---------------------------------------------------------------------------
// Exposed for tests: the diagnosis model and the pure functions over it. The
// planner and the runbook are decided entirely by these structs, so every branch
// can be exercised without an SAP system.
// ---------------------------------------------------------------------------

// The check model is setup's, but the status vocabulary is shared with every
// other command that prints a report line.
using Status = cli::Status;

struct Check {
    std::string id;      // stable identifier, e.g. "sap.adt"
    std::string title;   // one line, what was checked
    std::string detail;  // what was found
    std::string remedy;  // what to do about it; empty when Ok
    Status status = Status::Unknown;
};

struct Diagnosis {
    std::vector<Check> checks;
    bool have_uvx = false;
    bool adt_reachable = false;
    bool objects_present = false;
    bool probe_present = false;   // ZCL_ERPL_REV_DIAG, the round-trip probe
    bool destination_ok = false;
    bool gateway_reachable = false;
    // True when reachability could not be established either way, because the
    // gateway is reached through a tunnel this process does not hold. Distinct
    // from `not reachable`: it must not produce a remedy telling the operator
    // to go and look at firewalls.
    bool gateway_unknown = false;
    bool stms_available = false;
    // Can the ADT user create and activate a class? setup deploys ABAP, and the
    // sync/replicate commands generate one, so both need S_DEVELOP. Unknown
    // until the probe class is deployed and has been asked.
    Status develop_auth = Status::Unknown;

    const Check *Find(const std::string &id) const;
    bool AnyFailed() const;
};

// What setup intends to do, derived from a Diagnosis. Pure; no I/O.
struct Plan {
    bool deploy_objects = false;
    bool create_function_group = false;
    bool run_mkfm = false;
    bool run_setup_class = false;
    std::string target_package;        // resolved "$TMP" or ZERPL_CORE
    bool needs_transport = false;
    std::vector<std::string> manual_steps;  // what a human must still do, in order
    bool nothing_to_do = false;
};

Plan MakePlan(const Diagnosis &d, const Options &o);

// The Basis handout, rendered from the diagnosis. `server_host` is how the SAP
// gateway will see this machine -- it goes into the reginfo line verbatim.
std::string RenderBasisHandout(const Diagnosis &d, const Options &o,
                               const std::string &server_host);

// The Z_DUCKDB_* modules ZCL_ERPL_REV_MKFM is expected to leave behind, by
// name. Counting them is not enough: one module reporting success eight times
// counts the same as eight modules reporting once.
const std::vector<std::string> &FunctionModuleNames();

// Did the classruns actually do their job?
//
// Neither ABAP class fails the process when its work fails: ZCL_ERPL_REV_SETUP
// swallows RFC_MODIFY_TCPIP_DESTINATION behind `EXCEPTIONS OTHERS = 9` and
// ZCL_ERPL_REV_MKFM catches cx_root, so both exit 0 having achieved nothing.
// Trusting the exit code would let setup report success over a system with no
// destination and no function modules. Both classes do print what they found
// afterwards, so the outcome is read out of the output instead.
//
// `why` receives a human-readable reason when the answer is false.
bool SetupClassSucceeded(const std::string &output, const std::string &program_id,
                         const std::string &gwservice, std::string &why);
bool MkfmSucceeded(const std::string &output, const std::vector<std::string> &expected,
                   std::string &why);

// Read the S_DEVELOP verdict out of ZCL_ERPL_REV_DIAG's output. Unknown when
// the probe said nothing about it -- an older probe, or one that never ran.
Status DevelopAuthFromProbe(const std::string &output);

} // namespace erpl_rev::setup
