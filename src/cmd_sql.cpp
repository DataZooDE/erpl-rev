// `erpl-rev sql` — the CLI counterpart of the Z_ERPL_REV_SQL report.
//
// The report is a TextEdit and an ALV grid in SAP GUI; this is the same thing
// for a terminal. It talks to the DuckDB directly rather than routing through
// SAP, because the data is on this machine and a round trip through an ERP
// system to read a local table would be absurd.
#include <fstream>
#include <iostream>
#include <sstream>

#include "commands.hpp"
#include "db_client.hpp"

namespace erpl_rev::cmd {

bool ParseOption(const std::string &key, const std::function<std::string()> &take,
                 Options &o) {
    if (cli::ParseConnOption(key, take, o)) return true;

    if (key == "--db")               { o.db_path = take(); }
    else if (key == "--quack-url")   { o.quack_url = take(); }
    else if (key == "--quack-token") { o.quack_token = take(); }
    else if (key == "--file" || key == "-f") { o.file = take(); }
    else if (key == "--print-abap")     { o.print_abap = true; }
    else if (key == "--keep-generated") { o.keep_generated = true; }
    else if (key == "--quiet")          { o.quiet = true; }
    else if (key == "--queue-only")     { o.queue_only = true; }
    else if (key == "--count")          { o.count = true; }
    else if (key == "--limit") {
        const std::string v = take();
        try {
            size_t used = 0;
            o.limit = std::stoll(v, &used);
            if (used != v.size()) throw std::invalid_argument("trailing");
        } catch (...) {
            // The common cause is `erpl-rev sql --limit "SELECT 1"`, where the
            // statement was swallowed as the flag's value.
            std::fprintf(stderr,
                         "erpl-rev: --limit wants a number, got '%s'.\n"
                         "  If that was your SQL, put it before the flags or after `--`.\n",
                         v.c_str());
            return false;
        }
    }
    else if (key == "--format") {
        const std::string v = take();
        if (!render::ParseFormat(v, o.format)) {
            std::fprintf(stderr, "erpl-rev: --format wants table, csv or json; got '%s'.\n",
                         v.c_str());
            return false;
        }
        o.format_set = true;
    }
    else return false;
    return true;
}

void PrintHelp() {
    std::printf(
        "\nsql options:\n"
        "  --file, -f <path>        Read the statement(s) from a file instead of argv.\n"
        "  --format table|csv|json  Output format (default table).\n"
        "  --limit <n>              Row cap; 0 = all. Default 100 for table,\n"
        "                           unbounded for csv/json.\n"
        "  --count                  Report the exact total even when capped\n"
        "                           (drains the result; slower on huge queries).\n"
        "  --db <path>              Query this DuckDB file instead of auto-detecting.\n"
        "  --quack-url <uri>        Query a running server at this quack URI.\n"
        "  --quack-token <token>    Token for that server.\n"
        "\n"
        "  `sql` is a DuckDB console, as powerful as the duckdb CLI: it can ATTACH,\n"
        "  INSTALL, COPY TO a file and DROP a table. It is not sandboxed.\n");
}

int RunSql(Options o) {
    std::string statement;
    if (!o.file.empty()) {
        std::ifstream in(o.file);
        if (!in) {
            std::fprintf(stderr, "erpl-rev sql: cannot read %s\n", o.file.c_str());
            return 2;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        statement = ss.str();
    } else if (!o.args.empty()) {
        statement = o.args.front();
    }

    if (statement.empty()) {
        std::fprintf(stderr,
                     "erpl-rev sql: nothing to run.\n"
                     "  erpl-rev sql \"SELECT 1\"   or   erpl-rev sql --file query.sql\n");
        return 2;
    }

    const auto ep = dbc::Detect(o.db_path, o.quack_url, o.quack_token);
    if (!o.quiet) std::fprintf(stderr, "erpl-rev: %s\n", ep.why.c_str());

    if (o.dry_run) {
        std::printf("%s\n", statement.c_str());
        return 0;
    }

    long long limit = o.limit;
    if (limit < 0) limit = (o.format == render::Format::Table) ? 100 : 0;

    try {
        auto db = dbc::Db::Open(ep);
        const QueryResult r = db.Query(statement, limit, o.count);
        std::fputs(render::Render(r, o.format).c_str(), stdout);
        return 0;
    } catch (const dbc::ConnectError &e) {
        std::fprintf(stderr, "erpl-rev sql: %s\n", e.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "erpl-rev sql: %s\n", e.what());
        return 1;
    }
}

} // namespace erpl_rev::cmd
