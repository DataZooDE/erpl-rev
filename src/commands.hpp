// The subcommands that operate a running erpl-rev.
//
// Everything the product can do was reachable only from SAP GUI: a report to
// run SQL, a report to load a table, a report to register and run sync jobs.
// On a headless server none of that is reachable, which is what these close.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cli_common.hpp"
#include "table_render.hpp"
#include "tui_model.hpp"

#include <ftxui/dom/elements.hpp>

namespace erpl_rev::cmd {

struct Options : cli::ConnOptions {
    // Where the data is. Empty means "work it out" -- see dbc::Detect.
    std::string db_path;
    std::string quack_url;
    std::string quack_token;

    render::Format format = render::Format::Table;
    bool format_set = false;

    long long limit = -1;      // -1 = per-format default, 0 = unbounded
    bool count = false;        // drain for an exact total
    std::string file;          // sql --file
    bool print_abap = false;
    bool keep_generated = false;
    bool quiet = false;
    // Queue the command and return without contacting SAP at all. The periodic
    // ERPL_REV_DELTA job drains the queue, so this path needs no SAP
    // authorisation whatsoever -- not even the right to run a classrun.
    bool queue_only = false;

    // Positional words after the verb, in order.
    std::vector<std::string> args;
};

bool ParseOption(const std::string &key, const std::function<std::string()> &take,
                 Options &o);

void PrintHelp();

// Exit codes: 0 verified success, 1 verified failure, 2 misuse, 3 unknown.
// Build the JSON a queued command carries. Exposed for tests: these values are
// user input, and they travel through a SQL literal and then an ABAP JSON
// reader, so the escaping has to survive both.
std::string BuildParams(const std::vector<std::pair<std::string, std::string>> &kv);

// The first `--word` in `args` that subcommand `sub` never reads, or "" when
// every one of them is recognised. `sub` is the command as typed: "replicate",
// "sync create", "sync schedule", ...
//
// main() collects these words without knowing them -- sync and replicate mirror
// a many-tab SAP selection screen, and redeclaring thirty flags there would
// duplicate the whole surface -- so the check has to live with the command that
// does know them. Exposed for tests.
std::string UnknownFlag(const std::vector<std::string> &args, const std::string &sub);

// Whether `sync create --method` names a runnable delta method, matched
// case-insensitively. CDC is deliberately absent: the trigger tier has its own
// `erpl-rev cdc` verb. Exposed for tests.
bool IsSyncMethod(const std::string &method);

// The `error=` value from a driver classrun console line
// (`ERPL-DRV ...;status=..;error=..`), or "" when the output carries none.
// RunViaDriver prefers it over a bare row status: when the RFC leg fails
// before the driver claims the queued command, the row stays PENDING and
// says nothing, while the console output says everything. Exposed for tests.
std::string DriverErrorFromOutput(const std::string &output);

// Single-verb entry points, exposed for tests so dry-run and validation can
// be asserted without a server: with --dry-run they must return 0 having
// written nothing and contacted nothing.
int SyncSetWm(Options &o, const std::string &target);
int SyncPreview(Options &o, const std::string &target);
int SyncValidate(Options &o, const std::string &target);
int SyncUnpark(Options &o, const std::string &target);
int SyncDrop(Options &o, const std::string &target);

// Whether `sync <sub>` can touch SAP. The read-only verbs inspect local
// DuckDB state only and must not trigger credential resolution (which prompts
// on an interactive terminal). Exposed for tests.
bool SyncNeedsSapConn(const std::string &sub);

// The `sync --help` / `sync <sub> --help` text. Returned, not printed, so
// tests can assert on the exact words operators read. Unknown subs get the
// overview rather than an error: asking for help must never fail.
std::string SyncHelpText(const std::string &sub);

int RunSql(Options o);
int RunSync(Options o);

// The operator verbs. Everything they do reaches DuckDB, but they go through
// the SAP command queue like every other verb: one path for all operator
// commands, drained by the ABAP driver, which then calls a server-side PLAN
// action. The work still happens in one server-side transaction -- the queue
// carries the request, not the work.
int RunDaemon(Options o);   // start | stop | status
int RunSub(Options o);      // create | advance | ls
int RunRetain(Options o);   // prune a target's change log
int RunCdc(Options o);      // status | repair
int RunMass(Options o);     // run --split
int RunTop(Options o);      // the replication monitor

// The monitor's one frame as an element tree, factored out of RunTop so a
// script can assert on exactly what `--once` prints: rendering through
// Screen::Create(Dimension::Fit) like the real path, not a second formatter
// that could drift from the first. `throughput_box` is the graph pane
// builder (unused when `graph_on` is false); `term_cols` is the live width,
// ignored when `once` fixes the frame at 80.
ftxui::Element RenderTopDocument(const tui::Snapshot &snap, int selected,
                                 const std::string &action_note, bool graph_on,
                                 const std::function<ftxui::Element(int)> &throughput_box,
                                 bool once, int term_cols);

// `abap export <dir>` -- write the embedded ABAP sources out as files, for any
// delivery route that is not `setup` pushing them over ADT.
int RunAbap(Options o);
int RunReplicate(Options o);

} // namespace erpl_rev::cmd
