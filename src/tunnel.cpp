#include "tunnel.hpp"

#include <cctype>
#include <string>

namespace erpl_rev::tunnel {

namespace {

// Single-quote for a DuckDB string literal, doubling embedded quotes. Same rule
// as duckdb_bridge's SqlQuote; duplicated rather than exported because this file
// deliberately does not depend on DuckDB, so the pure logic stays unit-testable
// without an engine.
std::string Quote(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out + "'";
}

bool AllDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

} // namespace

bool SplitTarget(const std::string &target, std::string &host, std::string &port,
                 std::string &err) {
    if (target.empty()) { err = "empty"; return false; }

    // A bracketed IPv6 literal keeps its own colons: [::1]:3300.
    size_t colon;
    if (target[0] == '[') {
        const size_t close = target.find(']');
        if (close == std::string::npos) { err = "unclosed '[' in " + target; return false; }
        colon = target.find(':', close);
    } else {
        colon = target.rfind(':');
    }
    if (colon == std::string::npos) {
        err = "expected host:port, got '" + target + "'";
        return false;
    }
    host = target.substr(0, colon);
    port = target.substr(colon + 1);
    if (host.empty()) { err = "no host in '" + target + "'"; return false; }
    if (!AllDigits(port)) {
        // The gateway leg is numeric here on purpose: erpl-tunnel forwards a TCP
        // port, so `sapgw00` -- valid for gwserv, resolved by the RFC SDK through
        // /etc/services -- cannot be handed to tunnel_import.
        err = "port must be numeric, got '" + port + "' (use 3300, not sapgw00)";
        return false;
    }
    return true;
}

bool Resolve(const std::string &secret, const std::string &target,
             const std::string &gwhost, const std::string &gwserv,
             int local_port, Plan &out, std::string &err) {
    out = Plan{};
    out.gwhost = gwhost;
    out.gwserv = gwserv;

    if (secret.empty()) {
        // No tunnel: dial the gateway, exactly as the server always has. Any
        // --tunnel-target without a secret is a half-finished configuration and
        // is refused rather than silently ignored.
        if (!target.empty()) {
            err = "--tunnel-target needs --tunnel-secret; a target alone forwards nothing";
            return false;
        }
        if (local_port != 0) {
            err = "--tunnel-local-port needs --tunnel-secret; there is no forward to bind";
            return false;
        }
        out.dial_host = gwhost;
        out.dial_port = gwserv;
        return true;
    }

    if (local_port <= 0 || local_port > 65535) {
        err = "local port out of range";
        return false;
    }

    std::string th = gwhost, tp = gwserv;
    if (!target.empty()) {
        if (!SplitTarget(target, th, tp, err)) return false;
    } else if (!AllDigits(gwserv)) {
        // Same numeric constraint as above, but reached via the default rather
        // than an explicit --tunnel-target, so say which knob fixes it.
        err = "cannot tunnel to gateway service '" + gwserv +
              "': erpl-tunnel forwards a numeric TCP port. Set --gwserv 3300, or "
              "name the far end with --tunnel-target host:port";
        return false;
    }

    out.enabled = true;
    out.secret = secret;
    // The far end is what we forward TO; gwhost/gwserv stay the gateway's
    // identity for every human-facing consumer even when --tunnel-target
    // deliberately points somewhere else.
    out.gwhost = th;
    out.gwserv = tp;
    out.dial_host = "127.0.0.1";
    out.dial_port = std::to_string(local_port);
    return true;
}

std::string ImportSql(const Plan &p) {
    if (!p.enabled) return {};
    return "PRAGMA tunnel_import(secret = " + Quote(p.secret) +
           ", remote_host = " + Quote(p.gwhost) +
           ", remote_port = " + p.gwserv +
           ", local_port = " + p.dial_port +
           ", timeout = " + std::to_string(p.timeout_s) + ")";
}

} // namespace erpl_rev::tunnel
