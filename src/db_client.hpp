// Reaching the DuckDB a running erpl-rev owns.
//
// The server holds an exclusive lock on its database file, so a second process
// cannot simply open it. It talks to the server over quack instead -- and when
// no server is running, opens the file directly.
//
// The rule that matters: never guess. A read-only open behind a running server
// is permitted by DuckDB and returns a *stale snapshot*, so `sync ls` would
// quietly show pre-server state. Every command therefore prints which endpoint
// it chose before it does anything.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include "duckdb_bridge.hpp"

namespace erpl_rev::dbc {

struct Endpoint {
    enum class Kind { LocalFile, Quack };
    Kind kind = Kind::LocalFile;

    std::string db_path;   // LocalFile
    std::string uri;       // Quack, e.g. "quack:localhost:9494"
    std::string token;     // Quack

    // One sentence naming the choice and the reason, for stderr.
    std::string why;
};

// Selection order, with no silent fallback:
//   1. --quack-url        -> quack
//   2. --db <path>        -> that file, no probing (the explicit escape hatch)
//   3. live server state  -> quack at its recorded URI and token
//   4. stale server state -> removed, continue
//   5.                    -> ERPL_REV_DB_PATH, else ./erpl-rev.duckdb
Endpoint Detect(const std::string &db_flag, const std::string &quack_url_flag,
                const std::string &quack_token_flag);

// Thrown by Open with a message that names the convention rather than the
// symptom -- "a server is running but no state file was found at X" beats a
// raw DuckDB lock error.
struct ConnectError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Db {
public:
    static Db Open(const Endpoint &e);
    ~Db();
    Db(Db &&) noexcept;
    Db &operator=(Db &&) noexcept;

    QueryResult Query(const std::string &sql, long long max_rows = 0,
                      bool want_total = false);
    void Execute(const std::string &sql);

    const Endpoint &endpoint() const { return ep_; }

private:
    Db() = default;
    // Wrap a statement in quack_query() so it runs on the remote server.
    std::string RemoteWrap(const std::string &sql) const;

    Endpoint ep_;
    std::unique_ptr<DuckDbBridge> db_;
};

// '...' with the apostrophe doubled. Rejects an embedded NUL.
std::string SqlLiteral(const std::string &v);
// "..." with the double quote doubled. Rejects an embedded NUL.
std::string SqlIdentifier(const std::string &v);

} // namespace erpl_rev::dbc
