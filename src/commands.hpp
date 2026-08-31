// The subcommands that operate a running erpl-rev.
//
// Everything the product can do was reachable only from SAP GUI: a report to
// run SQL, a report to load a table, a report to register and run sync jobs.
// On a headless server none of that is reachable, which is what these close.
#pragma once

#include <functional>
#include <string>
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

    // Positional words after the verb, in order.
    std::vector<std::string> args;
};

bool ParseOption(const std::string &key, const std::function<std::string()> &take,
                 Options &o);

void PrintHelp();

// Exit codes: 0 verified success, 1 verified failure, 2 misuse, 3 unknown.
int RunSql(Options o);

} // namespace erpl_rev::cmd
