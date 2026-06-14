#include <catch2/catch_test_macros.hpp>

#include "erpl_rev_telemetry.hpp"

#include <cstdlib>
#include <memory>
#include <string>

// MSVC has no POSIX setenv/unsetenv — provide portable shims (via _putenv_s) so the
// env-guard cases below build on Windows as well as Linux/macOS.
#ifdef _WIN32
static int setenv(const char *name, const char *value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
static int unsetenv(const char *name) {
    return _putenv_s(name, "");   // empty value removes the variable on Windows
}
#endif

namespace {

// RAII helper to set/restore an env var so cases don't leak global state.
struct EnvGuard {
    EnvGuard(const char *name, const char *value) : name_(name) {
        const char *existing = std::getenv(name);
        has_prev_ = (existing != nullptr);
        prev_     = existing ? existing : "";
        ::setenv(name, value, 1);
    }
    ~EnvGuard() {
        if (has_prev_) ::setenv(name_, prev_.c_str(), 1);
        else           ::unsetenv(name_);
    }
    const char *name_;
    std::string prev_;
    bool        has_prev_;
};

// Records start/stop call counts and the args of the last call.
struct CountingBackend : public erpl_rev::ITelemetryBackend {
    int         start_calls = 0;
    int         stop_calls  = 0;
    std::string last_start_app, last_start_ver;
    std::string last_stop_app,  last_stop_ver;

    void captureStart(const std::string &app, const std::string &ver) override {
        start_calls++;
        last_start_app = app;
        last_start_ver = ver;
    }
    void captureStop(const std::string &app, const std::string &ver) override {
        stop_calls++;
        last_stop_app = app;
        last_stop_ver = ver;
    }
};

// Make sure no ambient opt-out env var is set for enabled-path cases.
void ClearOptOutEnv() {
    ::unsetenv("ERPL_REV_NO_TELEMETRY");
    ::unsetenv("DATAZOO_DISABLE_TELEMETRY");
}

} // namespace

using erpl_rev::ErplRevTelemetry;

TEST_CASE("telemetry: enabled path forwards start/stop with app_name=erpl-rev", "[telemetry]") {
    ClearOptOutEnv();
    auto mock = std::make_unique<CountingBackend>();
    auto *raw = mock.get();

    ErplRevTelemetry tel(std::move(mock));
    tel.notifyStart("2026.06.13");
    tel.notifyStop("2026.06.13");

    REQUIRE(raw->start_calls == 1);
    REQUIRE(raw->stop_calls == 1);
    CHECK(raw->last_start_app == "erpl-rev");
    CHECK(raw->last_stop_app == "erpl-rev");
    CHECK(raw->last_start_ver == "2026.06.13");
    CHECK(raw->last_stop_ver == "2026.06.13");
}

TEST_CASE("telemetry: setEnabled(false) suppresses emission", "[telemetry]") {
    ClearOptOutEnv();
    auto mock = std::make_unique<CountingBackend>();
    auto *raw = mock.get();

    ErplRevTelemetry tel(std::move(mock));
    tel.setEnabled(false);
    tel.notifyStart("1.0.0");
    tel.notifyStop("1.0.0");

    CHECK(raw->start_calls == 0);
    CHECK(raw->stop_calls == 0);
}

TEST_CASE("telemetry: ERPL_REV_NO_TELEMETRY suppresses emission", "[telemetry]") {
    ClearOptOutEnv();
    EnvGuard guard("ERPL_REV_NO_TELEMETRY", "1");
    auto mock = std::make_unique<CountingBackend>();
    auto *raw = mock.get();

    ErplRevTelemetry tel(std::move(mock));
    tel.notifyStart("1.0.0");
    tel.notifyStop("1.0.0");

    CHECK(raw->start_calls == 0);
    CHECK(raw->stop_calls == 0);
}

TEST_CASE("telemetry: DATAZOO_DISABLE_TELEMETRY suppresses emission", "[telemetry]") {
    ClearOptOutEnv();
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "true");
    auto mock = std::make_unique<CountingBackend>();
    auto *raw = mock.get();

    ErplRevTelemetry tel(std::move(mock));
    tel.notifyStart("1.0.0");
    tel.notifyStop("1.0.0");

    CHECK(raw->start_calls == 0);
    CHECK(raw->stop_calls == 0);
}

TEST_CASE("telemetry: falsey env var leaves telemetry enabled", "[telemetry]") {
    ClearOptOutEnv();
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    auto mock = std::make_unique<CountingBackend>();
    auto *raw = mock.get();

    ErplRevTelemetry tel(std::move(mock));
    tel.notifyStart("1.0.0");
    tel.notifyStop("1.0.0");

    CHECK(raw->start_calls == 1);
    CHECK(raw->stop_calls == 1);
}

TEST_CASE("telemetry: production constructor instantiates without throwing", "[telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "1");   // ensure no real HTTP
    REQUIRE_NOTHROW(ErplRevTelemetry{});
}
