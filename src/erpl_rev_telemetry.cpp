#include "erpl_rev_telemetry.hpp"

#include "telemetry.hpp"

#include <openssl/sha.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

// Bundled DuckDB engine version, baked in by CMake (matches DUCKDB_DIST). Sent
// as the event's `duckdb_version` envelope property so it isn't "unknown".
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

// SHA-256 hex digest — hashes a license id into the (non-PII) account group key.
std::string Sha256Hex(const std::string &input) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    ::SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(),
             digest.data());
    static const char *const kHex = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

} // namespace

// ── PostHogBackend ──────────────────────────────────────────────────────────

void PostHogBackend::setProduct(const std::string &name, const std::string &version,
                                const std::string &edition) {
    // Populate duckdb_version once, before the first event; platform and the
    // PostHog project key are left to the lib's defaults (do NOT override).
    duckdb::PostHogTelemetry::Instance().SetDuckDBVersion(ERPL_REV_DUCKDB_VERSION);
    duckdb::PostHogTelemetry::Instance().SetProduct(name, version, edition);
}

void PostHogBackend::associateGroup(const std::string &type, const std::string &key) {
    duckdb::PostHogTelemetry::Instance().AssociateGroup(type, key);
}

void PostHogBackend::capture(const std::string &event, duckdb::PropertyMap props) {
    duckdb::PostHogTelemetry::Instance().Capture(event, std::move(props));
}

void PostHogBackend::captureFeature(const std::string &feature, duckdb::PropertyMap props) {
    duckdb::PostHogTelemetry::Instance().CaptureFeature(feature, std::move(props));
}

void PostHogBackend::captureError(const std::string &error_class, duckdb::PropertyMap props) {
    duckdb::PostHogTelemetry::Instance().CaptureError(error_class, std::move(props));
}

void PostHogBackend::flush() {
    duckdb::PostHogTelemetry::Instance().Flush();
}

// ── ErplRevTelemetry ────────────────────────────────────────────────────────

ErplRevTelemetry::ErplRevTelemetry()
    : backend_(std::make_unique<PostHogBackend>()) {}

ErplRevTelemetry::ErplRevTelemetry(std::unique_ptr<ITelemetryBackend> backend)
    : backend_(std::move(backend)) {}

void ErplRevTelemetry::setEnabled(bool enabled) { enabled_ = enabled; }

bool ErplRevTelemetry::isEnabled() const { return active(); }

// Disabled by the programmatic flag OR either opt-out env var. The CLI flag and
// DATAZOO_DISABLE_TELEMETRY are resolved by main into setEnabled; the env vars
// are also re-checked here so the facade is opt-out on its own.
bool ErplRevTelemetry::active() const {
    if (!enabled_) return false;
    return !(EnvTruthy("ERPL_REV_NO_TELEMETRY") || EnvTruthy("DATAZOO_DISABLE_TELEMETRY"));
}

void ErplRevTelemetry::setSampling(double rate) {
    if (!(rate > 0.0) || rate >= 1.0 || std::isnan(rate)) {
        sample_rate_ = 1.0;
        sample_stride_ = 1;
        return;
    }
    sample_rate_ = rate;
    sample_stride_ = static_cast<uint64_t>(std::llround(1.0 / rate));
    if (sample_stride_ < 1) sample_stride_ = 1;
}

bool ErplRevTelemetry::sampleHot() {
    if (sample_stride_ <= 1) return true;
    // Emit exactly one of every `sample_stride_` calls. Deterministic and
    // atomic so it is testable and correct across concurrent RFC handlers.
    return (sample_counter_.fetch_add(1, std::memory_order_relaxed) % sample_stride_) == 0;
}

void ErplRevTelemetry::stampCommon(duckdb::PropertyMap &props, bool hot) const {
    props["install_kind"] = INSTALL_KIND;
    if (hot && sample_stride_ > 1) {
        props["sample_rate"] = sample_rate_;   // JSON number, scales counts back up
    }
}

void ErplRevTelemetry::configureProduct(const std::string &version,
                                        const std::string &edition) {
    if (!active()) return;
    backend_->setProduct(PRODUCT, version, edition);
}

void ErplRevTelemetry::associateDeployment() {
    if (!active()) return;
    backend_->associateGroup("deployment", duckdb::PostHogTelemetry::GetDistinctId());
}

void ErplRevTelemetry::associateAccount(const std::string &license_id) {
    if (!active() || license_id.empty()) return;
    backend_->associateGroup("account", Sha256Hex(license_id));
}

void ErplRevTelemetry::serverStarted(bool quack_enabled, int reg_count) {
    if (!active()) return;
    duckdb::PropertyMap props;
    props["quack_enabled"] = quack_enabled;   // JSON bool
    props["reg_count"] = reg_count;           // JSON number
    stampCommon(props, /*hot=*/false);
    backend_->capture("server_started", std::move(props));
}

void ErplRevTelemetry::rfcCall(const std::string &fm, bool ok, double duration_ms) {
    if (!active() || !sampleHot()) return;
    duckdb::PropertyMap props;
    props["fm"] = fm;                                 // bounded FM enum
    props["status_class"] = ok ? "2xx" : "5xx";
    props["duration_ms"] = duration_ms;               // JSON number
    stampCommon(props, /*hot=*/true);
    backend_->captureFeature("rfc_call", std::move(props));
}

void ErplRevTelemetry::error(const std::string &error_class, const std::string &feature) {
    if (!active()) return;
    duckdb::PropertyMap props;
    props["feature"] = feature;                       // the FM enum
    stampCommon(props, /*hot=*/false);
    // error_class must be an enumerated class — never a message or SAP data.
    backend_->captureError(error_class, std::move(props));
}

void ErplRevTelemetry::flush() {
    if (!active()) return;
    backend_->flush();
}

// ── Process-wide instance ─────────────────────────────────────────────────────

ErplRevTelemetry &GlobalTelemetry() {
    // Intentionally leaked (never destroyed): matches the underlying library
    // singleton lifetime and keeps late captures during teardown safe.
    static ErplRevTelemetry *instance = new ErplRevTelemetry();
    return *instance;
}

} // namespace erpl_rev
