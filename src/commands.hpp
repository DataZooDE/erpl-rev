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
int RunReplicate(Options o);

} // namespace erpl_rev::cmd
