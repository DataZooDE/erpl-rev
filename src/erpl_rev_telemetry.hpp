#pragma once

#include <memory>
#include <string>

namespace erpl_rev {

// Pure interface — inject a mock in unit tests, PostHogBackend in production.
struct ITelemetryBackend {
    virtual ~ITelemetryBackend() = default;
    virtual void captureStart(const std::string &app_name,
                              const std::string &app_version) = 0;
    virtual void captureStop(const std::string &app_name,
                             const std::string &app_version) = 0;
};

// Production backend: delegates to the shared duckdb::PostHogTelemetry singleton
// (DataZooDE/posthog-telemetry), which emits the `application_start` /
// `application_stop` events with {app_name, app_version, platform,
// duckdb_version} into the same PostHog project as flapi.
class PostHogBackend : public ITelemetryBackend {
public:
    void captureStart(const std::string &app_name,
                      const std::string &app_version) override;
    void captureStop(const std::string &app_name,
                     const std::string &app_version) override;
};

// erpl-rev telemetry facade. Anonymous, privacy-conscious, default-on with
// opt-out. Disabled when setEnabled(false) OR any of the env vars
// ERPL_REV_NO_TELEMETRY / DATAZOO_DISABLE_TELEMETRY is truthy (1/true/yes).
// Never emits SAP data, query text, or table/field names — only the app
// lifecycle events with the properties above.
class ErplRevTelemetry {
public:
    // Production: creates a PostHogBackend.
    ErplRevTelemetry();

    // Test injection: takes ownership of the provided backend.
    explicit ErplRevTelemetry(std::unique_ptr<ITelemetryBackend> backend);

    // Emit the application_start event (app_version passed through verbatim;
    // the caller strips any leading "v" from the git tag).
    void notifyStart(const std::string &version);

    // Emit the application_stop event.
    void notifyStop(const std::string &version);

    // Programmatic opt-out (CLI flag).
    void setEnabled(bool enabled);

    // The breakdown value that sits next to "flapi" in PostHog.
    static constexpr const char *APP_NAME = "erpl-rev";

private:
    static bool IsTelemetryDisabled(bool enabled);

    std::unique_ptr<ITelemetryBackend> backend_;
    bool enabled_ = true;
};

} // namespace erpl_rev
