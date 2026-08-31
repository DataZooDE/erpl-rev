#include "db_client.hpp"

#include "cli_common.hpp"

namespace erpl_rev::dbc {

std::string SqlLiteral(const std::string &v) {
    if (v.find('\0') != std::string::npos)
        throw ConnectError("value contains a NUL byte and cannot be used in SQL");
    std::string out = "'";
    for (char c : v) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}

std::string SqlIdentifier(const std::string &v) {
    if (v.find('\0') != std::string::npos)
        throw ConnectError("identifier contains a NUL byte");
    std::string out = "\"";
    for (char c : v) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

Endpoint Detect(const std::string &db_flag, const std::string &quack_url_flag,
                const std::string &quack_token_flag) {
    Endpoint e;

    auto token_for = [&](const std::string &from_state) {
        if (!quack_token_flag.empty()) return quack_token_flag;
        const std::string env = cli::Env("ERPL_REV_QUACK_TOKEN");
        if (!env.empty()) return env;
        return from_state;
    };

    if (!quack_url_flag.empty()) {
        e.kind = Endpoint::Kind::Quack;
        e.uri = quack_url_flag;
        e.token = token_for("");
        e.why = "using the quack endpoint given on the command line (" + e.uri + ")";
        return e;
    }

    if (!db_flag.empty()) {
        e.kind = Endpoint::Kind::LocalFile;
        e.db_path = db_flag;
        e.why = "opening " + e.db_path + " directly (--db was given)";
        return e;
    }

    // ReadServerState removes the file and returns false when its pid is dead,
    // so a crashed server does not leave the CLI dialling a listener that is
    // no longer there.
    cli::ServerState st;
    if (cli::ReadServerState(st)) {
        e.kind = Endpoint::Kind::Quack;
        e.uri = st.quack_listen;
        e.token = token_for(st.quack_token);
        e.why = "using the running server at " + e.uri + " (pid " +
                std::to_string(st.pid) + ")";
        return e;
    }

    e.kind = Endpoint::Kind::LocalFile;
    e.db_path = cli::Env("ERPL_REV_DB_PATH");
    if (e.db_path.empty()) e.db_path = "erpl-rev.duckdb";
    e.why = "opening " + e.db_path + " directly (no server running)";
    return e;
}

Db::~Db() = default;
Db::Db(Db &&) noexcept = default;
Db &Db::operator=(Db &&) noexcept = default;

Db Db::Open(const Endpoint &e) {
    Db d;
    d.ep_ = e;

    if (e.kind == Endpoint::Kind::LocalFile) {
        try {
            d.db_ = std::make_unique<DuckDbBridge>(e.db_path);
        } catch (const std::exception &ex) {
            const std::string what = ex.what();
            if (what.find("lock") != std::string::npos ||
                what.find("Conflicting") != std::string::npos) {
                // Deliberately do not retry read-only: DuckDB would allow it and
                // hand back a stale snapshot, which is worse than an error
                // because nothing would look wrong.
                throw ConnectError(
                    "cannot open " + e.db_path + ": a server already has it open.\n"
                    "  Expected to find its details at " +
                    cli::ServerStatePath().string() + ", but there is no live state "
                    "file there.\n"
                    "  Start the server normally (quack is on by default), or point "
                    "this command at it with --quack-url / --quack-token.\n"
                    "  Original error: " + what);
            }
            throw ConnectError("cannot open " + e.db_path + ": " + what);
        }
        return d;
    }

    // Quack: a throwaway in-memory database used purely as a client. Queries
    // are forwarded to the server with quack_query(), so they run against the
    // live in-process data rather than a copy.
    try {
        d.db_ = std::make_unique<DuckDbBridge>("");
    } catch (const std::exception &ex) {
        throw ConnectError(std::string("cannot start a local DuckDB: ") + ex.what());
    }

    try {
        d.db_->Execute("INSTALL quack");
        d.db_->Execute("LOAD quack");
    } catch (const std::exception &ex) {
        throw ConnectError(
            std::string("the quack extension could not be loaded: ") + ex.what() +
            "\n  It is downloaded on first use, so this needs network access once."
            "\n  Alternatively stop the server and use --db <path>.");
    }

    // Probe the connection now rather than at the first query, so a bad token
    // or a dead listener is reported by the command that chose the endpoint.
    try {
        d.db_->Query(d.RemoteWrap("SELECT 1"));
    } catch (const std::exception &ex) {
        throw ConnectError(
            "cannot reach the running server at " + e.uri + ": " + ex.what() +
            (e.token.empty()
                 ? "\n  No token was available. Pass --quack-token, or set "
                   "ERPL_REV_QUACK_TOKEN."
                 : "\n  The token may be stale; restart the server or pass "
                   "--quack-token."));
    }
    return d;
}

// Wrap a statement so it executes on the remote server.
//
// Note this is quack_query(), not ATTACH: this build of the quack extension
// exposes a table function, and `ATTACH 'quack:...'` fails with
// `Catalog "r" does not exist`. quack_query carries DDL as happily as it
// carries a SELECT, so it works as a general execution channel.
std::string Db::RemoteWrap(const std::string &sql) const {
    std::string q = "SELECT * FROM quack_query(" + SqlLiteral(ep_.uri) + ", " +
                    SqlLiteral(sql);
    if (!ep_.token.empty()) q += ", token => " + SqlLiteral(ep_.token);
    return q + ")";
}

QueryResult Db::Query(const std::string &sql, long long max_rows, bool want_total) {
    if (ep_.kind == Endpoint::Kind::Quack)
        return db_->Query(RemoteWrap(sql), max_rows, want_total);
    return db_->Query(sql, max_rows, want_total);
}

void Db::Execute(const std::string &sql) {
    if (ep_.kind == Endpoint::Kind::Quack) { db_->Query(RemoteWrap(sql)); return; }
    db_->Execute(sql);
}

} // namespace erpl_rev::dbc
