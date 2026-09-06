#include "metrics.hpp"

#include <string>
#include <vector>

#include "json_util.hpp"

namespace erpl_rev {
namespace metrics {
namespace {

// One field out of a row rendered as a flat JSON object by QueryResult.
std::string Field(const std::string &row, const std::string &key) {
    const auto rows = json::ParseRows("[" + row + "]");
    if (rows.empty()) return {};
    for (const auto &c : rows[0])
        if (c.key == key) return c.is_null ? std::string() : c.value;
    return {};
}

void Help(std::string &out, const std::string &name, const std::string &help,
          const std::string &type) {
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
}

}  // namespace

std::string EscapeLabel(const std::string &v) {
    std::string out;
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string Render(DuckDbBridge &db) {
    std::string out;

    // --- per target ---------------------------------------------------------
    Help(out, "erpl_rev_target_lag_seconds",
         "Seconds since this target's last cycle. Absent when it has never run.", "gauge");
    Help(out, "erpl_rev_target_healthy",
         "1 when the target ran, is not blocked or parked, and has no failures.", "gauge");
    Help(out, "erpl_rev_target_fail_count",
         "Consecutive failures; drives the backoff and eventually parks the target.", "gauge");
    Help(out, "erpl_rev_target_rows_applied", "Rows applied by the last cycle.", "gauge");
    Help(out, "erpl_rev_target_blocked",
         "1 when the registration cannot run and the planner skips it.", "gauge");
    Help(out, "erpl_rev_target_parked", "1 when backoff has parked the target.", "gauge");

    const auto targets = db.Query(
        "SELECT target, lag_seconds, is_healthy, fail_count, last_rows, is_blocked, is_parked "
        "FROM erpl_rev_targets ORDER BY target");
    for (const auto &row : targets.rows) {
        const auto label = "{target=\"" + EscapeLabel(Field(row, "target")) + "\"} ";
        const auto lag = Field(row, "lag_seconds");
        // Absent, not zero. Zero reads as "perfectly current" on every dashboard
        // and alert rule there is; a missing series is no data, which is what a
        // target that has never run actually has.
        if (!lag.empty()) out += "erpl_rev_target_lag_seconds" + label + lag + "\n";

        auto flag = [&](const char *name, const std::string &v) {
            out += std::string(name) + label + (v == "true" ? "1" : "0") + "\n";
        };
        flag("erpl_rev_target_healthy", Field(row, "is_healthy"));
        flag("erpl_rev_target_blocked", Field(row, "is_blocked"));
        flag("erpl_rev_target_parked", Field(row, "is_parked"));
        // A number, always. An empty value renders "metric{...} " with no
        // sample, which a scraper rejects as a parse error -- and it rejects the
        // WHOLE response, so one odd row voids every metric in the scrape.
        auto num = [&](const char *name, const std::string &v) {
            out += std::string(name) + label + (v.empty() ? "0" : v) + "\n";
        };
        num("erpl_rev_target_fail_count", Field(row, "fail_count"));
        num("erpl_rev_target_rows_applied", Field(row, "last_rows"));
    }

    // --- the system as a whole ----------------------------------------------
    Help(out, "erpl_rev_targets", "Registered targets.", "gauge");
    Help(out, "erpl_rev_targets_healthy", "Targets with nothing wrong with them.", "gauge");
    Help(out, "erpl_rev_worst_lag_seconds", "The largest lag across all targets.", "gauge");
    Help(out, "erpl_rev_daemon_up", "1 when the streaming daemon reports itself RUNNING.",
         "gauge");
    Help(out, "erpl_rev_daemon_heartbeat_age_seconds",
         "Seconds since the daemon last beat. Rising means it is stuck, not busy.", "gauge");
    Help(out, "erpl_rev_daemon_ticks", "Ticks completed by the current daemon instance.",
         "counter");

    const auto health = db.Query(
        "SELECT targets, healthy, worst_lag_seconds, daemon_status, "
        "daemon_heartbeat_age_s, daemon_ticks FROM erpl_rev_health");
    if (!health.rows.empty()) {
        const auto &h = health.rows[0];
        auto gnum = [&](const char *name, const std::string &v) {
            out += std::string(name) + " " + (v.empty() ? "0" : v) + "\n";
        };
        gnum("erpl_rev_targets", Field(h, "targets"));
        gnum("erpl_rev_targets_healthy", Field(h, "healthy"));
        const auto worst = Field(h, "worst_lag_seconds");
        if (!worst.empty()) out += "erpl_rev_worst_lag_seconds " + worst + "\n";
        out += std::string("erpl_rev_daemon_up ") +
               (Field(h, "daemon_status") == "RUNNING" ? "1" : "0") + "\n";
        const auto age = Field(h, "daemon_heartbeat_age_s");
        if (!age.empty()) out += "erpl_rev_daemon_heartbeat_age_seconds " + age + "\n";
        gnum("erpl_rev_daemon_ticks", Field(h, "daemon_ticks"));
    }
    return out;
}

}  // namespace metrics
}  // namespace erpl_rev

// --- the listener -----------------------------------------------------------

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define ERPL_CLOSESOCK closesocket
#define ERPL_BADSOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define ERPL_CLOSESOCK ::close
#define ERPL_BADSOCK (-1)
#endif

#include <atomic>
#include <thread>

namespace erpl_rev {
namespace metrics {
namespace server {
namespace {

std::atomic<bool> g_run{false};
std::thread g_thread;
socket_t g_sock = ERPL_BADSOCK;

void Serve(DuckDbBridge *db) {
    while (g_run.load()) {
        socket_t c = ::accept(g_sock, nullptr, nullptr);
        if (c == ERPL_BADSOCK) {
            if (!g_run.load()) break;
            continue;
        }
        // A receive timeout, because this loop serves one client at a time: a
        // half-open connection that never sends a request would otherwise hold
        // the endpoint shut for every scraper behind it. Two seconds is far
        // longer than a local scrape and far shorter than a scrape interval.
#ifdef _WIN32
        DWORD tv = 2000;
#else
        timeval tv{2, 0};
#endif
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));

        char buf[1024] = {0};
        const auto n = ::recv(c, buf, sizeof(buf) - 1, 0);
        std::string body, status = "200 OK", ctype = "text/plain; version=0.0.4";
        if (n > 0 && std::string(buf, static_cast<size_t>(n)).rfind("GET /metrics", 0) == 0) {
            try {
                body = Render(*db);
            } catch (const std::exception &e) {
                // A failed render is a 500, not a crash and not a silent empty
                // page: an empty exposition reads to a scraper as "everything is
                // zero", which is worse than no answer at all.
                status = "500 Internal Server Error";
                body = std::string("render failed: ") + e.what() + "\n";
            }
        } else {
            status = "404 Not Found";
            body = "erpl-rev serves metrics at /metrics\n";
        }
        const std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: " + ctype +
                                 "\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + body;
        ::send(c, resp.data(), static_cast<int>(resp.size()), 0);
        ERPL_CLOSESOCK(c);
    }
}

}  // namespace

bool Start(DuckDbBridge &db, int port, std::string &error) {
    if (port <= 0) return false;
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    g_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == ERPL_BADSOCK) { error = "socket() failed"; return false; }
    int yes = 1;
    ::setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes),
                 sizeof(yes));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    // Loopback only unless something in front forwards it. A replication server
    // binding a metrics port on every interface by default is an exposure the
    // operator did not ask for.
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(g_sock, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
        error = "cannot bind 127.0.0.1:" + std::to_string(port);
        ERPL_CLOSESOCK(g_sock);
        g_sock = ERPL_BADSOCK;
        return false;
    }
    if (::listen(g_sock, 8) != 0) {
        error = "listen() failed on port " + std::to_string(port);
        ERPL_CLOSESOCK(g_sock);
        g_sock = ERPL_BADSOCK;
        return false;
    }
    g_run.store(true);
    g_thread = std::thread(Serve, &db);
    return true;
}

void Stop() {
    if (!g_run.exchange(false)) return;
    if (g_sock != ERPL_BADSOCK) {
        // shutdown() BEFORE close(). Closing a descriptor another thread is
        // blocked in accept() on is a race: the number can be reused by any
        // thread that opens something in the window, and the accept then
        // returns a connection on an unrelated socket. shutdown wakes the
        // accept without freeing the descriptor, and only then is it closed --
        // after the thread that was using it has been joined.
#ifdef _WIN32
        ::shutdown(g_sock, SD_BOTH);
#else
        ::shutdown(g_sock, SHUT_RDWR);
#endif
    }
    if (g_thread.joinable()) g_thread.join();
    if (g_sock != ERPL_BADSOCK) { ERPL_CLOSESOCK(g_sock); g_sock = ERPL_BADSOCK; }
}

}  // namespace server
}  // namespace metrics
}  // namespace erpl_rev
