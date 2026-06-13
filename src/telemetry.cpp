#include "telemetry.hpp"

#include <httplib.h>
#include <openssl/evp.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace erpl_rev {

namespace {

// PostHog project — same Datazoo project erpl-adt / flapi report to.
const char *kApiKey   = "phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t";
const char *kEndpoint = "https://eu.posthog.com";
const char *kPath     = "/batch/";
const char *kAppName  = "erpl-rev";

// Drop events once the backlog passes this — a long-running server must never
// grow memory unbounded when the endpoint is unreachable.
constexpr size_t kMaxQueue = 1024;

// --- Global state -----------------------------------------------------------
std::atomic<bool>   g_enabled{false};
std::string         g_version;
Telemetry::Backend  g_backend;
std::mutex          g_mutex;          // guards g_version / g_backend

struct Event {
    std::string                  name;
    std::vector<Telemetry::Prop> props;
};

std::mutex              g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<Event>       g_queue;
bool                    g_stop = false;
std::thread             g_worker;

bool EnvDisabled(const char *name) {
    const char *val = std::getenv(name);
    if (!val) return false;
    std::string s{val};
    return s == "1" || s == "true" || s == "yes";
}

std::string Sha256Hex(const std::string &input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    EVP_MD_CTX   *ctx        = EVP_MD_CTX_new();
    if (!ctx) return {};
    std::string out;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) &&
        EVP_DigestUpdate(ctx, input.data(), input.size()) &&
        EVP_DigestFinal_ex(ctx, digest, &digest_len)) {
        std::ostringstream oss;
        oss << std::hex;
        for (unsigned int i = 0; i < digest_len; ++i) {
            oss << (digest[i] >> 4) << (digest[i] & 0xF);
        }
        out = oss.str();
    }
    EVP_MD_CTX_free(ctx);
    return out;
}

// --- Machine id (anonymous, hashed before it leaves the process) ------------
#ifdef __linux__
std::string GetMachineId() {
    for (const char *path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        std::ifstream f(path);
        if (!f) continue;
        std::string id;
        std::getline(f, id);
        if (!id.empty()) return id;
    }
    return {};
}
#elif defined(__APPLE__)
std::string GetMachineId() {
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
    if (!service) return {};
    CFStringRef uuid_ref = static_cast<CFStringRef>(
        IORegistryEntryCreateCFProperty(service, CFSTR("IOPlatformUUID"),
                                        kCFAllocatorDefault, 0));
    IOObjectRelease(service);
    if (!uuid_ref) return {};
    char buf[64];
    bool ok = CFStringGetCString(uuid_ref, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(uuid_ref);
    return ok ? std::string(buf) : std::string{};
}
#elif defined(_WIN32)
std::string GetMachineId() {
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
    }
    char  buf[64];
    DWORD size = sizeof(buf);
    DWORD type;
    bool  ok = (RegQueryValueExA(key, "MachineGuid", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS);
    RegCloseKey(key);
    return ok ? std::string(buf) : std::string{};
}
#else
std::string GetMachineId() { return {}; }
#endif

std::string NowISO8601() {
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%FT%TZ", &tm);
    return std::string(buf);
}

std::string JsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// Anonymous, cached SHA256(machine id); "unknown" when no id is available.
std::string DistinctId() {
    static std::string    cached;
    static std::once_flag flag;
    std::call_once(flag, [] {
        std::string id = GetMachineId();
        cached = id.empty() ? "unknown" : Sha256Hex(id);
    });
    return cached;
}

std::string DetectPlatform() {
#if   defined(_WIN32) && defined(_M_ARM64)
    return "windows_arm64";
#elif defined(_WIN32)
    return "windows_amd64";
#elif defined(__APPLE__) && defined(__arm64__)
    return "osx_arm64";
#elif defined(__APPLE__)
    return "osx_amd64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux_arm64";
#elif defined(__linux__)
    return "linux_amd64";
#else
    return "unknown";
#endif
}

void SendToPostHog(const std::string &event,
                   const std::vector<Telemetry::Prop> &props) {
    // Test hook: let unit tests capture events without real HTTP.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_backend) {
            g_backend(event, props);
            return;
        }
    }

    // Re-check the cross-product kill switch at send time (parity with the
    // posthog-telemetry library: an operator can disable mid-run).
    if (EnvDisabled("DATAZOO_DISABLE_TELEMETRY")) return;

    std::string version;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        version = g_version;
    }

    std::string extra;
    for (const auto &p : props) {
        extra += ",\"" + JsonEscape(p.key) + "\":";
        if (p.numeric) extra += p.value;
        else           extra += "\"" + JsonEscape(p.value) + "\"";
    }

    std::string payload =
        "{"
        "\"api_key\":\"" + std::string(kApiKey) + "\","
        "\"batch\":[{"
        "\"event\":\"" + JsonEscape(event) + "\","
        "\"distinct_id\":\"" + JsonEscape(DistinctId()) + "\","
        "\"properties\":{"
        "\"app_name\":\"" + std::string(kAppName) + "\","
        "\"app_version\":\"" + JsonEscape(version) + "\","
        "\"platform\":\"" + DetectPlatform() + "\""
        + extra +
        "},"
        "\"timestamp\":\"" + NowISO8601() + "\""
        "}]}";

    try {
        httplib::Client cli(kEndpoint);
        cli.set_connection_timeout(3);
        cli.set_read_timeout(5);
        auto res = cli.Post(kPath, payload, "application/json");
        (void)res; // best-effort: silent on failure, telemetry must never throw
    } catch (...) {
        // Never propagate — telemetry must not affect the server.
    }
}

void WorkerLoop() {
    for (;;) {
        Event ev;
        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
            if (g_queue.empty()) {
                if (g_stop) return;
                continue;
            }
            ev = std::move(g_queue.front());
            g_queue.pop_front();
        }
        SendToPostHog(ev.name, ev.props);
    }
}

} // namespace

// ---------------------------------------------------------------------------

void Telemetry::Initialize(bool user_disabled, const std::string &version) {
    if (user_disabled || EnvDisabled("ERPL_REV_NO_TELEMETRY") ||
        EnvDisabled("DATAZOO_DISABLE_TELEMETRY")) {
        g_enabled.store(false);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_version = version;
    }
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_stop = false;
        if (!g_worker.joinable()) g_worker = std::thread(WorkerLoop);
    }
    g_enabled.store(true);
}

bool Telemetry::IsEnabled() noexcept { return g_enabled.load(); }

void Telemetry::SetBackendForTesting(Backend backend) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backend = std::move(backend);
}

void Telemetry::Track(const std::string &event, std::vector<Prop> props) {
    if (!g_enabled.load()) return;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (g_queue.size() >= kMaxQueue) return;   // backlog: drop, never block
        g_queue.push_back(Event{event, std::move(props)});
    }
    g_queue_cv.notify_one();
}

void Telemetry::Shutdown() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (!g_worker.joinable()) return;
        g_stop = true;
        worker = std::move(g_worker);
    }
    g_queue_cv.notify_all();
    worker.join();
    g_enabled.store(false);
}

} // namespace erpl_rev
