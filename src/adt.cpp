#include "adt.hpp"

#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define ERPL_POPEN  _popen
#define ERPL_PCLOSE _pclose
#else
#include <sys/wait.h>   // WIFEXITED / WEXITSTATUS on the pclose status
#define ERPL_POPEN  popen
#define ERPL_PCLOSE pclose
#endif

namespace erpl_rev::adt {
namespace {

// Quote one argv element for the shell. popen goes through a shell, so anything
// user-supplied -- a password, a host, an object name -- has to be quoted or a
// stray space or semicolon becomes command injection.
std::string Quote(const std::string &s) {
#ifdef _WIN32
    // cmd.exe: wrap in double quotes and double any embedded ones.
    //
    // This is NOT fully general, and pretending otherwise would be the bug:
    // cmd.exe still expands %VAR% inside double quotes, and there is no escape
    // for `%` on the command line (the `%%` form only works in batch files). A
    // value containing `%` would therefore be silently rewritten into something
    // else. Nothing erpl-rev passes here legitimately contains one -- hosts,
    // clients, packages, ABAP object names and temp paths -- so the honest
    // handling is to refuse rather than to mangle. (The password never reaches
    // argv at all; it goes through the environment.)
    for (char ch : s) {
        if (ch == '%' || ch == '\n' || ch == '\r') return std::string();
    }
    std::string out = "\"";
    for (char ch : s) { if (ch == '"') out += '"'; out += ch; }
    return out + "\"";
#else
    // POSIX: single quotes protect everything except a single quote itself.
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    return out + "'";
#endif
}

// Returns empty if any element could not be quoted safely; the caller must
// treat that as a refusal to run, never as an empty command line.
std::string Join(const std::vector<std::string> &argv, bool &ok) {
    std::string cmd;
    ok = true;
    for (size_t i = 0; i < argv.size(); i++) {
        const std::string q = Quote(argv[i]);
        if (q.empty()) { ok = false; return std::string(); }
        if (i) cmd += ' ';
        cmd += q;
    }
    return cmd;
}

} // namespace

Result RunCapture(const std::vector<std::string> &argv) {
    Result r;
    // 2>&1: erpl-adt writes diagnostics to stderr, and a caller trying to work
    // out why a step failed needs them interleaved with the rest.
    bool quotable = false;
    const std::string joined = Join(argv, quotable);
    if (!quotable) {
        r.spawn_failed = true;
        r.output = "erpl-rev: an argument contains a character that cannot be quoted "
                   "safely for this shell (on Windows: '%', newline). Refusing to run.";
        return r;
    }
    const std::string cmd = joined + " 2>&1";
    FILE *p = ERPL_POPEN(cmd.c_str(), "r");
    if (!p) {
        r.spawn_failed = true;
        return r;
    }
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p)) r.output += buf.data();
    const int rc = ERPL_PCLOSE(p);
#ifdef _WIN32
    r.exit_code = rc;
#else
    r.exit_code = (rc == -1) ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
#endif
    // 127 is the shell's "command not found": the tool is absent rather than
    // failing, which callers report very differently.
    if (r.exit_code == 127) r.spawn_failed = true;
    return r;
}

// How to invoke erpl-adt on this machine.
//
// `uvx erpl-adt` is the normal answer and needs no setup: uv downloads the
// tool from PyPI on first use. But erpl-rev also ships as a standalone binary
// and in Docker, where uv may be absent, and the old code simply assumed it --
// which meant every SAP-touching command failed with "command not found"
// wearing a costume.
//
// Resolved once and cached: an explicit override, then a plain erpl-adt on
// PATH (the pip/Homebrew/Docker install), then uvx.
static std::vector<std::string> g_launcher;
static bool g_launcher_resolved = false;
static std::string g_launcher_override;

void SetToolPath(const std::string &path) {
    g_launcher_override = path;
    g_launcher_resolved = false;
}

const std::vector<std::string> &Launcher() {
    if (g_launcher_resolved) return g_launcher;
    g_launcher_resolved = true;

    auto works = [](const std::vector<std::string> &argv) {
        auto probe = argv;
        probe.push_back("--version");
        return RunCapture(probe).ok();
    };

    if (!g_launcher_override.empty() && works({g_launcher_override})) {
        g_launcher = {g_launcher_override};
    } else if (works({"erpl-adt"})) {
        g_launcher = {"erpl-adt"};
    } else {
        // Last, because it is the slowest: uvx re-resolves the environment on
        // every call even when the wheel is already cached.
        g_launcher = {"uvx", "erpl-adt"};
    }
    return g_launcher;
}

std::string ToolHint() {
    return "erpl-adt is required for the SAP side. Install it with one of:\n"
           "    uv tool install erpl-adt     (or have `uv` on PATH; uvx fetches it)\n"
           "    pip install erpl-adt\n"
           "  or point at an existing binary with --adt-path.";
}

bool ToolAvailable() { return !ToolVersion().empty(); }

std::string ToolVersion() {
    auto argv = Launcher();
    argv.push_back("--version");
    auto r = RunCapture(argv);
    if (!r.ok()) return "";
    // Trim to the first line; the version banner may be followed by other output.
    auto nl = r.output.find('\n');
    std::string v = nl == std::string::npos ? r.output : r.output.substr(0, nl);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

Result Run(const Conn &c, const std::vector<std::string> &args) {
    std::vector<std::string> argv = Launcher();
    for (const std::string &a : {std::string("--host"), c.host,
                                 std::string("--port"), c.port,
                                 std::string("--user"), c.user,
                                 std::string("--client"), c.client,
                                 // --password-env, never --password: an argument
                                 // is visible in the process list to every user
                                 // on the machine.
                                 std::string("--password-env"),
                                 std::string("ERPL_REV_SAP_PASSWORD"),
                                 std::string("--timeout"), std::to_string(c.timeout_s)})
        argv.push_back(a);
    argv.insert(argv.end(), args.begin(), args.end());

    // Pass the password through the environment for the child only.
#ifdef _WIN32
    _putenv_s("ERPL_REV_SAP_PASSWORD", c.password.c_str());
#else
    setenv("ERPL_REV_SAP_PASSWORD", c.password.c_str(), 1);
#endif
    auto r = RunCapture(argv);
#ifdef _WIN32
    _putenv_s("ERPL_REV_SAP_PASSWORD", "");
#else
    unsetenv("ERPL_REV_SAP_PASSWORD");
#endif
    return r;
}

Result CreateObject(const Conn &c, const std::string &adt_type, const std::string &name,
                    const std::string &package, const std::string &description,
                    const std::string &transport) {
    std::vector<std::string> a{"object", "create", "--type", adt_type, "--name", name,
                               "--package", package, "--description", description};
    if (!transport.empty()) { a.push_back("--transport"); a.push_back(transport); }
    return Run(c, a);
}

Result WriteSource(const Conn &c, const std::string &name, const std::string &file,
                   const std::string &src_type, const std::string &transport) {
    std::vector<std::string> a{"source", "write", name};
    if (!src_type.empty()) { a.push_back("--type"); a.push_back(src_type); }
    a.push_back("--file");
    a.push_back(file);
    a.push_back("--activate");
    if (!transport.empty()) { a.push_back("--transport"); a.push_back(transport); }
    return Run(c, a);
}

Result RunClass(const Conn &c, const std::string &name) {
    return Run(c, {"object", "run", name});
}

Result DeleteObject(const Conn &c, const std::string &uri) {
    return Run(c, {"object", "delete", uri});
}

bool ActivationSucceeded(const Result &r) {
    if (r.spawn_failed) return false;
    const std::string &o = r.output;
    auto has = [&](const char *needle) { return o.find(needle) != std::string::npos; };
    return has("Activated") || has("Nothing to activate");
}

} // namespace erpl_rev::adt
