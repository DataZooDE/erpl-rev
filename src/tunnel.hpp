// Optional erpl-tunnel forward for the SAP gateway leg.
//
// erpl-rev registers OUTBOUND at the gateway, which is firewall-friendly right up
// until the server cannot sit in the SAP network zone -- then that leg has to
// cross a boundary, and today the operator bridges it by hand:
//
//   ERPL_REV_GWHOST=127.0.0.1 erpl-rev serve --init-sql "... CALL tunnel_import(
//       'sap', 'sapgw.internal', 3300, 3300);"
//
// That works, and it quietly corrupts everything downstream that reads the
// configuration. `doctor` probes 127.0.0.1:3300 -- the LOCAL end of the forward,
// which is bound whether or not the far side is alive -- and reports the gateway
// as reachable. `setup` persists gwhost=127.0.0.1 as the recorded address of the
// SAP system. The Basis handout renders `ACCESS=127.0.0.1 CANCEL=127.0.0.1`. And
// the forward's local port has to be kept in step with ERPL_REV_GWSERV by hand,
// with no error when they drift.
//
// So the split here is deliberate: **erpl-rev owns the wiring, erpl-tunnel owns
// the credential.** The operator still writes `CREATE SECRET ... (TYPE tunnel,
// backend 'tailscale', auth_key '...')` in erpl-tunnel's own SQL, in --init-file,
// where every backend-specific option belongs and where no flag of ours has to
// track their releases. We take only the part they should never have been doing
// by hand: choosing the local port, pointing the RFC registration at it, and
// keeping `gwhost` meaning the real gateway everywhere a human reads it.
//
// Everything here is inert unless a secret is named. No secret means Resolve()
// returns the gateway itself as the dial target, which is what the server did
// before this file existed.
#pragma once

#include <string>

namespace erpl_rev::tunnel {

// Where the RFC registration actually dials, and what it should be CALLED.
//
// The distinction is the whole point. `gwhost`/`gwserv` stay the true gateway --
// they are what doctor prints, what setup persists, what the reginfo handout puts
// in ACCESS/CANCEL. `dial_host`/`dial_port` are what RfcCreateServer connects to,
// which is the near end of the forward when a tunnel is in play. With no tunnel
// the two pairs are identical and nothing anywhere behaves differently.
struct Plan {
    bool enabled = false;
    std::string secret;       // erpl-tunnel secret name; empty => disabled
    std::string gwhost;       // the REAL gateway, always
    std::string gwserv;
    std::string dial_host;    // what RfcCreateServer connects to
    std::string dial_port;
    // Seconds tunnel_import waits for the listener. Bounded so a mesh that never
    // comes up fails the boot instead of hanging it.
    int timeout_s = 30;
};

// Split "host:port" for --tunnel-target. Rejects an empty half, a missing colon
// and a non-numeric port, because a typo here surfaces much later as a refused
// registration with nothing pointing back at this string. IPv6 literals must be
// bracketed ("[::1]:3300").
bool SplitTarget(const std::string &target, std::string &host, std::string &port,
                 std::string &err);

// Build the plan. `secret` empty => disabled, and the plan dials the gateway
// directly. `target` empty => the tunnel's far end IS the configured gateway,
// which is the case worth optimising for; pass it only to forward somewhere else.
// `local_port` is the near end (0 is rejected -- the caller picks a free port).
bool Resolve(const std::string &secret, const std::string &target,
             const std::string &gwhost, const std::string &gwserv,
             int local_port, Plan &out, std::string &err);

// The one statement erpl-rev issues on the operator's behalf. A PRAGMA with named
// arguments, which is the form erpl-tunnel registers -- `CALL tunnel_import(...)`
// fails with "Table Function with name tunnel_import does not exist". The secret
// name and host reach DuckDB as SQL, so both are quoted: a secret name is operator
// input and has no business terminating a literal.
std::string ImportSql(const Plan &p);

// ---------------------------------------------------------------------------
// Scaffolding, for `erpl-rev setup`
// ---------------------------------------------------------------------------

// The three backends erpl-tunnel offers, in the order setup should present them:
// a mesh first, because the SSH backend does not pin the server's host key
// (erpl-tunnel's own docs/guides/security.md says so), which makes a bastion
// MITM-able while looking encrypted. The intuitive ordering is the wrong one.
enum class Backend { Tailscale, NetBird, Ssh };

// Parse a backend name; false when it is not one of the three.
bool ParseBackend(const std::string &name, Backend &out);
const char *BackendName(Backend b);

// What the operator needs to hand over for each backend. `key` is the auth key /
// setup key / SSH password, and never appears anywhere but the generated file.
struct SecretSpec {
    Backend backend = Backend::Tailscale;
    std::string secret;      // the name erpl-rev will pass to --tunnel-secret
    std::string key;         // auth_key / setup_key / password; may be empty
    std::string hostname;    // mesh node name, or the SSH host
    std::string ssh_user;    // SSH only
    std::string ssh_port;    // SSH only, default 22
    std::string state_dir;   // mesh only; where the node identity persists
};

// Render the CREATE SECRET block for --init-file. This is erpl-tunnel's own SQL,
// generated rather than copied out of a doc, so a typo in a backend option cannot
// silently produce a secret that fails only at boot.
//
// An empty `key` renders a clearly-marked placeholder rather than an empty
// literal: a secret that parses but cannot authenticate is worse than one that
// obviously needs filling in.
std::string RenderSecretSql(const SecretSpec &s);

} // namespace erpl_rev::tunnel
