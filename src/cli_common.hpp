// Pieces shared by every erpl-rev subcommand.
//
// These all began life inside sap_setup.cpp's anonymous namespace, which was
// fine while `setup` and `doctor` were the only commands. They are here now
// because a terminal prompt, a config file and a connection flag are not
// setup's private business -- every command needs them, and the alternative to
// moving them was copying them.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include "adt.hpp"

namespace erpl_rev::cli {

// ---------------------------------------------------------------------------
// Terminal
// ---------------------------------------------------------------------------

enum class Status { Ok, Warn, Fail, Unknown };

// The six-character badge a check line starts with, e.g. "[  ok  ]".
const char *Mark(Status s);

bool IsTty();
std::string Env(const char *key);

// Prompt with a default; an empty line takes the default.
std::string Prompt(const std::string &label, const std::string &fallback);

// Read a secret without echoing it. A password typed in the clear is a password
// shoulder-surfed, and one passed as a flag is visible in the process list to
// every user on the box.
std::string PromptSecret(const std::string &label);

bool Confirm(const std::string &question, bool assume_yes);

// The one line of `output` starting with `prefix`, or empty. Deciding anything
// from a substring found *somewhere* in a tool's output is how a diagnostic
// echo of an expected value turns into a false pass.
std::string LineStartingWith(const std::string &output, const std::string &prefix);

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

// Can this machine open a TCP connection there? Answered before anything is
// deployed, because "the destination exists but nothing ever connects" is the
// most expensive failure to debug after the fact.
bool TcpReachable(const std::string &host, const std::string &port);

// How the outside world sees this machine; "<this-host>" if it cannot be told.
std::string LocalHostname();

// ---------------------------------------------------------------------------
// Config file: `key = value`, one per line, `#` comments. Deliberately not a
// TOML/YAML parser -- this holds a handful of scalars, and adding a dependency
// and a parse-error surface to store a handful of scalars is a poor trade.
// ---------------------------------------------------------------------------

using ConfigMap = std::map<std::string, std::string>;

std::filesystem::path ConfigPath();
ConfigMap ReadConfig();
bool WriteConfig(const ConfigMap &kv);

// ---------------------------------------------------------------------------
// Connection options, shared by every command that can reach SAP.
//
// `setup::Options` derives from this, so `--sap-host` and friends are declared
// once and mean the same thing everywhere.
// ---------------------------------------------------------------------------

struct ConnOptions {
    std::string host, port, client, user, password;
    // "came from the command line", so the resolver can tell an explicit empty
    // value from an absent one.
    bool host_set = false, port_set = false, client_set = false;
    bool user_set = false, password_set = false;

    bool dry_run = false;
    bool non_interactive = false;
    bool assume_yes = false;
    bool json = false;        // legacy alias for --format json
    int timeout_s = 600;      // erpl-adt --timeout; its own default of 120 truncates real work
    std::string adt_path;     // --adt-path: an explicit erpl-adt executable
};

// Consume one connection/behaviour flag. Returns false if the key is none of
// ours, so the caller can offer it to the next module and ultimately report it
// as unknown.
bool ParseConnOption(const std::string &key,
                     const std::function<std::string()> &take_value, ConnOptions &o);

// The precedence rule, in one place: flag (set and non-empty) > environment >
// config file > built-in default.
std::string Pick(const ConfigMap &cfg, const std::string &flag, bool flag_set,
                 const char *env_key, const char *cfg_key, const std::string &def);

// Resolve host/port/client/user/password only. Prompts for user and password
// when allowed and still missing.
void ResolveConn(ConnOptions &o, const ConfigMap &cfg, bool allow_prompt);

adt::Conn ToAdtConn(const ConnOptions &o);

// ---------------------------------------------------------------------------
// Server state file -- how a CLI process finds the running server.
//
// This exists because of one awkward fact: when quack generates its own token,
// that token lives *only* in a startup log line. Without a file to read it
// from, `erpl-rev sql` cannot authenticate against a server started with no
// arguments -- which is the configuration almost everyone will have.
//
// Written 0600 (it holds the token), atomically, at quack start; removed at
// shutdown. A stale file whose pid is dead is treated as absent and deleted.
// ---------------------------------------------------------------------------

struct ServerState {
    std::string db_path;
    std::string quack_listen;   // e.g. "quack:localhost:9494"
    std::string quack_token;
    std::string version;
    std::string started_at;     // ISO-8601 UTC
    long pid = 0;
};

// $XDG_RUNTIME_DIR/erpl-rev/server.json, else $XDG_STATE_HOME, else
// ~/.local/state; %LOCALAPPDATA%\erpl-rev\server.json on Windows.
std::filesystem::path ServerStatePath();

bool WriteServerState(const ServerState &s);

// False when the file is absent, unparseable, or names a pid that is no longer
// alive -- in the last case the stale file is removed first.
bool ReadServerState(ServerState &out);

void RemoveServerState();

// Is this pid a live process owned by us?
bool ProcessAlive(long pid);

} // namespace erpl_rev::cli
