#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "telemetry.hpp"   // duckdb::PostHogTelemetry, PropertyMap, PropertyValue

namespace erpl_rev {

// ─────────────────────────────────────────────────────────────────────────────
// erpl-rev telemetry facade
//
// erpl-rev is a long-running *server* (a registered SAP RFC server bridging ABAP
// calls into DuckDB), so it emits against the shared DataZoo telemetry schema
// (telemetry_schema: 2) with install_kind="server" and one $session_id per
// uptime (the library mints a per-process session id). See TELEMETRY.md for the
// exact catalogue of events and properties.
//
// Every property is a bounded enum, a function-module name from a fixed set, or
// a number — NEVER SAP data, SQL text, table/field names, row data, connection
// strings, or error messages. That contract is enforced by construction (call
// sites only pass enums/numbers) and by the library's 512-byte string clamp.
// ─────────────────────────────────────────────────────────────────────────────

// Pure interface — inject a fake in unit tests, PostHogBackend in production.
// Takes the library's typed PropertyMap so numeric/boolean props keep their JSON
// type (load-bearing for HogQL sum()/avg() and boolean filters).
struct ITelemetryBackend {
    virtual ~ITelemetryBackend() = default;
    virtual void setProduct(const std::string &name, const std::string &version,
                            const std::string &edition) = 0;
    virtual void associateGroup(const std::string &type, const std::string &key) = 0;
    virtual void capture(const std::string &event, duckdb::PropertyMap props) = 0;
    virtual void captureFeature(const std::string &feature, duckdb::PropertyMap props) = 0;
    virtual void captureError(const std::string &error_class, duckdb::PropertyMap props) = 0;
    virtual void flush() = 0;
};

// Production backend: delegates to the shared duckdb::PostHogTelemetry singleton.
class PostHogBackend : public ITelemetryBackend {
public:
    void setProduct(const std::string &name, const std::string &version,
                    const std::string &edition) override;
    void associateGroup(const std::string &type, const std::string &key) override;
    void capture(const std::string &event, duckdb::PropertyMap props) override;
    void captureFeature(const std::string &feature, duckdb::PropertyMap props) override;
    void captureError(const std::string &error_class, duckdb::PropertyMap props) override;
    void flush() override;
};

// erpl-rev telemetry facade. Anonymous, privacy-conscious, default-on with
// opt-out. A single guard (`active()`) short-circuits everything, so one
// opt-out — setEnabled(false), ERPL_REV_NO_TELEMETRY, or
// DATAZOO_DISABLE_TELEMETRY — disables all events. Every emit is non-blocking:
// the library enqueues onto a background worker and returns immediately, which
// matters because the RFC bridge handlers run on SAP SDK worker threads.
class ErplRevTelemetry {
public:
    // Production: creates a PostHogBackend.
    ErplRevTelemetry();

    // Test injection: takes ownership of the provided backend.
    explicit ErplRevTelemetry(std::unique_ptr<ITelemetryBackend> backend);

    // Programmatic opt-out (CLI flag resolves to this).
    void setEnabled(bool enabled);
    bool isEnabled() const;

    // Client-side sampling for the hot per-call path (rfc_call). rate in (0,1];
    // 1.0 (default) emits every event. Surviving events are stamped sample_rate
    // so counts scale back up. Lifecycle events are always emitted.
    void setSampling(double rate);

    // ── Boot ────────────────────────────────────────────────────────────────
    void configureProduct(const std::string &version, const std::string &edition);
    void associateDeployment();
    void associateAccount(const std::string &license_id);
    void serverStarted(bool quack_enabled, int reg_count);

    // ── Runtime (hot path — sampled) ──────────────────────────────────────────
    // One rfc_call feature per bridge FM invocation. `fm` is a bounded enum
    // (ping|query|ingest|snapshot_merge|cursor_open|cursor_fetch|cursor_close|
    //  cdc_plan|cdc_apply); NEVER the SQL, target, or handle.
    void rfcCall(const std::string &fm, bool ok, double duration_ms);

    // ── Runtime (low volume — always emitted) ────────────────────────────────
    // error_class is an enumerated class only — never a message or SAP data.
    void error(const std::string &error_class, const std::string &feature);

    // Synchronously drain buffered events (call on clean shutdown — the
    // library's at-exit handler discards by design).
    void flush();

    static constexpr const char *PRODUCT = "erpl_rev";
    static constexpr const char *INSTALL_KIND = "server";

private:
    // enabled_ AND not disabled via ERPL_REV_NO_TELEMETRY / DATAZOO_DISABLE_TELEMETRY.
    bool active() const;
    // Decimate a hot-path event; returns true if this call should be emitted.
    bool sampleHot();
    // Stamp install_kind (and, when sampling, sample_rate) onto a prop map.
    void stampCommon(duckdb::PropertyMap &props, bool hot) const;

    std::unique_ptr<ITelemetryBackend> backend_;
    bool enabled_ = true;

    // Deterministic 1-of-N decimation; atomic because RFC handlers are concurrent.
    double sample_rate_ = 1.0;
    uint64_t sample_stride_ = 1;
    std::atomic<uint64_t> sample_counter_{0};
};

// Process-wide instance. The RFC bridge handlers (rfc_handlers.cpp) run on SAP
// SDK worker threads with no access to main's local, so they emit through this.
// main() configures it at boot and flushes it on shutdown. Never destroyed
// (leaked like the underlying library singleton) so late captures are safe.
ErplRevTelemetry &GlobalTelemetry();

} // namespace erpl_rev
