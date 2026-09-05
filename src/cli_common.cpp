#include "cli_common.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
// winsock2 must precede windows.h, or windows.h pulls in the winsock 1.1
// declarations and every socket symbol collides.
#include <winsock2.h>
#include <ws2tcpip.h>   // addrinfo / getaddrinfo
#include <io.h>         // _isatty
#include <windows.h>
#else
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#endif

#include "json_util.hpp"

namespace erpl_rev::cli {

// ---------------------------------------------------------------------------
// Terminal
// ---------------------------------------------------------------------------

const char *Mark(Status s) {
    switch (s) {
        case Status::Ok:      return "  ok  ";
        case Status::Warn:    return " warn ";
        case Status::Fail:    return " FAIL ";
        default:              return "  ?   ";
    }
}

bool IsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

std::string Env(const char *k) {
    const char *v = std::getenv(k);
    return v ? std::string(v) : std::string();
}

std::string Prompt(const std::string &label, const std::string &fallback) {
    std::cout << label;
    if (!fallback.empty()) std::cout << " [" << fallback << "]";
    std::cout << ": " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) return fallback;
    return line;
}

std::string PromptSecret(const std::string &label) {
    std::cout << label << ": " << std::flush;
    std::string pw;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
    std::getline(std::cin, pw);
    SetConsoleMode(h, mode);
#else
    termios old{};
    if (tcgetattr(STDIN_FILENO, &old) == 0) {
        termios noecho = old;
        noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
        std::getline(std::cin, pw);
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
    } else {
        std::getline(std::cin, pw);
    }
#endif
    std::cout << "\n";
    return pw;
}

bool Confirm(const std::string &question, bool assume_yes) {
    if (assume_yes) return true;
    std::cout << question << " [y/N]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return line == "y" || line == "Y" || line == "yes";
}

std::string LineStartingWith(const std::string &output, const std::string &prefix) {
    size_t pos = 0;
    while (pos <= output.size()) {
        const size_t eol = output.find('\n', pos);
        const size_t len = (eol == std::string::npos ? output.size() : eol) - pos;
        if (output.compare(pos, prefix.size(), prefix) == 0)
            return output.substr(pos, len);
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

#ifdef _WIN32
// Winsock refuses every call until the process has initialised it. Without
// this, getaddrinfo() fails with WSANOTINITIALISED and doctor reports a
// perfectly healthy gateway as unreachable -- a wrong diagnosis is worse than
// no diagnosis, because it sends people to look at firewalls.
struct WinsockInit {
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockInit() { WSACleanup(); }
};
#endif

bool TcpReachable(const std::string &host, const std::string &port) {
#ifdef _WIN32
    static WinsockInit winsock;   // once per process, torn down at exit
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    bool ok = false;
    for (addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = static_cast<int>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (fd < 0) continue;
        ok = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) == 0;
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }
    freeaddrinfo(res);
    return ok;
}

int FreeLoopbackPort() {
#ifdef _WIN32
    static WinsockInit winsock;
#endif
    // Bind port 0 and ask the kernel what it gave us. Portable, and cheaper than
    // guessing a range and probing it.
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
        sockaddr_in got{};
        socklen_t len = sizeof(got);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&got), &len) == 0)
            port = ntohs(got.sin_port);
    }
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
    return port;
}

std::string LocalHostname() {
#ifdef _WIN32
    static WinsockInit winsock;   // gethostname needs it too
#endif
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "<this-host>";
}

// ---------------------------------------------------------------------------
// Config file
// ---------------------------------------------------------------------------

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// Write `body` to `p` so that it is never briefly world-readable and never
// left half-written. The obvious version -- truncate, write, chmod -- has a
// window in which a file holding a secret is readable by anyone, and leaves a
// truncated file if the process dies mid-write. Rename is atomic.
bool WriteSecretFile(const std::filesystem::path &p, const std::string &body) {
    if (p.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
#ifndef _WIN32
    const auto tmp = p.parent_path() / (p.filename().string() + ".tmp");
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < body.size()) {
        const ssize_t n = ::write(fd, body.data() + off, body.size() - off);
        if (n <= 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
        off += static_cast<size_t>(n);
    }
    // The rename can outrun the data otherwise, leaving a file of NULs after a
    // crash -- cheap insurance for a file written about once.
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), p.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
#else
    std::ofstream out(p, std::ios::trunc);
    if (!out) return false;
    out << body;
    return out.good();
#endif
}

} // namespace

std::filesystem::path ConfigPath() {
#ifdef _WIN32
    std::string base = Env("APPDATA");
    if (base.empty()) base = Env("USERPROFILE");
    if (base.empty()) return {};
    return std::filesystem::path(base) / "erpl-rev" / "config";
#else
    std::string base = Env("XDG_CONFIG_HOME");
    if (base.empty()) {
        const std::string home = Env("HOME");
        if (home.empty()) return {};
        base = home + "/.config";
    }
    return std::filesystem::path(base) / "erpl-rev" / "config";
#endif
}

ConfigMap ReadConfig() {
    ConfigMap kv;
    const auto p = ConfigPath();
    if (p.empty()) return kv;
    std::ifstream in(p);
    if (!in) return kv;
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = Trim(line.substr(0, eq));
        const std::string v = Trim(line.substr(eq + 1));
        if (!k.empty()) kv[k] = v;
    }
    return kv;
}

bool WriteConfig(const ConfigMap &kv) {
    std::string body =
        "# erpl-rev setup. Written by `erpl-rev setup`; safe to edit by hand.\n"
        "# Precedence: command-line flags > environment > this file.\n";
    for (const auto &[k, v] : kv) body += k + " = " + v + "\n";
    return WriteSecretFile(ConfigPath(), body);
}

// ---------------------------------------------------------------------------
// Connection options
// ---------------------------------------------------------------------------

bool ParseConnOption(const std::string &key,
                     const std::function<std::string()> &take, ConnOptions &o) {
    if (key == "--sap-host")        { o.host = take();   o.host_set = true; }
    else if (key == "--sap-port")   { o.port = take();   o.port_set = true; }
    else if (key == "--sap-client") { o.client = take(); o.client_set = true; }
    else if (key == "--sap-user")   { o.user = take();   o.user_set = true; }
    else if (key == "--timeout")    { o.timeout_s = std::atoi(take().c_str()); }
    else if (key == "--adt-path")   { o.adt_path = take(); adt::SetToolPath(o.adt_path); }
    else if (key == "--dry-run")         { o.dry_run = true; }
    else if (key == "--non-interactive") { o.non_interactive = true; }
    else if (key == "--json")            { o.json = true; }
    else if (key == "-y" || key == "--yes") { o.assume_yes = true; }
    else return false;
    return true;
}

std::string Pick(const ConfigMap &cfg, const std::string &flag, bool flag_set,
                 const char *env_key, const char *cfg_key, const std::string &def) {
    if (flag_set && !flag.empty()) return flag;
    const std::string e = Env(env_key);
    if (!e.empty()) return e;
    auto it = cfg.find(cfg_key);
    if (it != cfg.end() && !it->second.empty()) return it->second;
    return def;
}

void ResolveConn(ConnOptions &o, const ConfigMap &cfg, bool allow_prompt) {
    o.host     = Pick(cfg, o.host, o.host_set, "SAP_HOST", "host", "localhost");
    o.port     = Pick(cfg, o.port, o.port_set, "SAP_PORT", "port", "50000");
    o.client   = Pick(cfg, o.client, o.client_set, "SAP_CLIENT", "client", "001");
    o.user     = Pick(cfg, o.user, o.user_set, "SAP_USER", "user", "");
    o.password = Pick(cfg, o.password, o.password_set, "SAP_PASSWORD", "password", "");

    if (!allow_prompt) return;
    if (o.user.empty())     o.user     = Prompt("SAP user", "DEVELOPER");
    if (o.password.empty()) o.password = PromptSecret("SAP password (not echoed)");
}

adt::Conn ToAdtConn(const ConnOptions &o) {
    return adt::Conn{o.host, o.port, o.client, o.user, o.password, false, o.timeout_s};
}

// ---------------------------------------------------------------------------
// Server state file
// ---------------------------------------------------------------------------

std::filesystem::path ServerStatePath() {
#ifdef _WIN32
    std::string base = Env("LOCALAPPDATA");
    if (base.empty()) base = Env("APPDATA");
    if (base.empty()) base = Env("USERPROFILE");
    if (base.empty()) return {};
    return std::filesystem::path(base) / "erpl-rev" / "server.json";
#else
    // XDG_RUNTIME_DIR first: it is tmpfs, per-user, 0700, and cleared on
    // logout -- exactly the lifetime a token wants. The state directory is the
    // fallback for systems without a session runtime dir (containers, cron).
    std::string base = Env("XDG_RUNTIME_DIR");
    if (base.empty()) {
        base = Env("XDG_STATE_HOME");
        if (base.empty()) {
            const std::string home = Env("HOME");
            if (home.empty()) return {};
            base = home + "/.local/state";
        }
    }
    return std::filesystem::path(base) / "erpl-rev" / "server.json";
#endif
}

bool ProcessAlive(long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    const DWORD w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return w == WAIT_TIMEOUT;   // still running
#else
    // Signal 0 performs the permission and existence checks without sending
    // anything. EPERM means the pid exists but is not ours -- treat that as
    // "not our server" rather than alive, since we could not talk to it anyway.
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

bool WriteServerState(const ServerState &s) {
    std::string body = "{\n";
    auto field = [&](const char *k, const std::string &v, bool last = false) {
        body += "  \"" + std::string(k) + "\": \"" + json::EscapeString(v) + "\"";
        body += last ? "\n" : ",\n";
    };
    field("db_path", s.db_path);
    field("quack_listen", s.quack_listen);
    field("quack_token", s.quack_token);
    field("version", s.version);
    field("started_at", s.started_at);
    body += "  \"pid\": " + std::to_string(s.pid) + "\n}\n";
    return WriteSecretFile(ServerStatePath(), body);
}

namespace {

// Pull one "key": "value" or "key": number out of the state file. This file is
// written by us, one field per line, so a full JSON parser would be ceremony;
// anything unexpected simply yields an empty value and the caller treats the
// file as unusable.
std::string StateField(const std::string &body, const std::string &key) {
    const std::string needle = "\"" + key + "\":";
    const auto k = body.find(needle);
    if (k == std::string::npos) return {};
    auto p = body.find_first_not_of(" \t", k + needle.size());
    if (p == std::string::npos) return {};
    if (body[p] == '"') {
        std::string out;
        for (++p; p < body.size() && body[p] != '"'; ++p) {
            if (body[p] == '\\' && p + 1 < body.size()) ++p;
            out += body[p];
        }
        return out;
    }
    const auto e = body.find_first_of(",}\n", p);
    return Trim(body.substr(p, (e == std::string::npos ? body.size() : e) - p));
}

} // namespace

bool ReadServerState(ServerState &out) {
    const auto p = ServerStatePath();
    if (p.empty()) return false;

    std::string body;
    {
        // Scoped so the handle is closed before the possible remove below:
        // Windows refuses to delete a file that is still open, so leaving this
        // stream in scope would leak a stale state file forever.
        std::ifstream in(p);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        body = ss.str();
    }

    out.db_path      = StateField(body, "db_path");
    out.quack_listen = StateField(body, "quack_listen");
    out.quack_token  = StateField(body, "quack_token");
    out.version      = StateField(body, "version");
    out.started_at   = StateField(body, "started_at");
    out.pid          = std::atol(StateField(body, "pid").c_str());

    if (out.quack_listen.empty()) return false;

    // A file left behind by a crashed or kill -9'd server names a pid that is
    // gone. Reporting that as a live server would make the CLI hang connecting
    // to nothing, so treat it as absent and clean up.
    if (!ProcessAlive(out.pid)) {
        std::error_code ec;
        std::filesystem::remove(p, ec);
        return false;
    }
    return true;
}

void RemoveServerState() {
    const auto p = ServerStatePath();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

} // namespace erpl_rev::cli
