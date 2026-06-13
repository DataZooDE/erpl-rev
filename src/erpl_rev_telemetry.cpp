#include "erpl_rev_telemetry.hpp"

#include "telemetry.hpp"

#include <cstdlib>
#include <string>

// Bundled DuckDB engine version, baked in by CMake (matches DUCKDB_DIST). Sent
// as the event's `duckdb_version` property so it isn't reported as "unknown".
#ifndef ERPL_REV_DUCKDB_VERSION
#define ERPL_REV_DUCKDB_VERSION "unknown"
#endif

namespace erpl_rev {

namespace {

// Truthy env value: 1 / true / yes (matches flapi + the rest of erpl-rev).
bool EnvTruthy(const char *name) {
    const char *val = std::getenv(name);
    if (!val) return false;
    std::string s(val);
    return s == "1" || s == "true" || s == "yes";
}

} // namespace

// ── PostHogBackend ──────────────────────────────────────────────────────────

void PostHogBackend::captureStart(const std::string &app_name,
                                  const std::string &app_version) {
    // Populate duckdb_version once, before the first event; platform and the
    // PostHog project key are left to the lib's defaults (do NOT override).
    duckdb::PostHogTelemetry::Instance().SetDuckDBVersion(ERPL_REV_DUCKDB_VERSION);
    duckdb::PostHogTelemetry::Instance().CaptureApplicationStart(app_name, app_version);
}

void PostHogBackend::captureStop(const std::string &app_name,
                                 const std::string &app_version) {
    duckdb::PostHogTelemetry::Instance().SetDuckDBVersion(ERPL_REV_DUCKDB_VERSION);
    duckdb::PostHogTelemetry::Instance().CaptureApplicationStop(app_name, app_version);
}

// ── ErplRevTelemetry ────────────────────────────────────────────────────────

ErplRevTelemetry::ErplRevTelemetry()
    : backend_(std::make_unique<PostHogBackend>()), enabled_(true) {}

ErplRevTelemetry::ErplRevTelemetry(std::unique_ptr<ITelemetryBackend> backend)
    : backend_(std::move(backend)), enabled_(true) {}

void ErplRevTelemetry::setEnabled(bool enabled) { enabled_ = enabled; }

// Disabled by the programmatic flag OR either opt-out env var. The CLI flag
// (#42) and DATAZOO_DISABLE_TELEMETRY are resolved by main into setEnabled;
// the env vars are also re-checked here so the facade is opt-out on its own.
bool ErplRevTelemetry::IsTelemetryDisabled(bool enabled) {
    if (!enabled) return true;
    return EnvTruthy("ERPL_REV_NO_TELEMETRY") || EnvTruthy("DATAZOO_DISABLE_TELEMETRY");
}

void ErplRevTelemetry::notifyStart(const std::string &version) {
    if (IsTelemetryDisabled(enabled_)) return;
    backend_->captureStart(APP_NAME, version);
}

void ErplRevTelemetry::notifyStop(const std::string &version) {
    if (IsTelemetryDisabled(enabled_)) return;
    backend_->captureStop(APP_NAME, version);
}

} // namespace erpl_rev
