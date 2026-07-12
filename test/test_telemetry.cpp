#include <catch2/catch_test_macros.hpp>

#include "erpl_rev_telemetry.hpp"
#include "telemetry.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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

// Clear both opt-out env vars so enabled-path cases aren't suppressed by an
// ambient DATAZOO_DISABLE_TELEMETRY in the CI environment.
struct ClearOptOut {
    EnvGuard a{"ERPL_REV_NO_TELEMETRY", "0"};
    EnvGuard b{"DATAZOO_DISABLE_TELEMETRY", "0"};
};

// Records every call so tests can assert on event names and property maps.
struct FakeBackend : public erpl_rev::ITelemetryBackend {
    struct Call {
        std::string kind;   // "capture" | "feature" | "error"
        std::string name;
        duckdb::PropertyMap props;
    };
    std::vector<Call> calls;
    std::vector<std::pair<std::string, std::string>> groups;
    std::string product_name, product_version, product_edition;
    int flushes = 0;

    void setProduct(const std::string &name, const std::string &version,
                    const std::string &edition) override {
        product_name = name; product_version = version; product_edition = edition;
    }
    void associateGroup(const std::string &type, const std::string &key) override {
        groups.emplace_back(type, key);
    }
    void capture(const std::string &event, duckdb::PropertyMap props) override {
        calls.push_back({"capture", event, std::move(props)});
    }
    void captureFeature(const std::string &feature, duckdb::PropertyMap props) override {
        calls.push_back({"feature", feature, std::move(props)});
    }
    void captureError(const std::string &error_class, duckdb::PropertyMap props) override {
        calls.push_back({"error", error_class, std::move(props)});
    }
    void flush() override { flushes++; }

    const Call *find(const std::string &name) const {
        for (const auto &c : calls) if (c.name == name) return &c;
        return nullptr;
    }
};

bool hasStringProp(const duckdb::PropertyMap &p, const std::string &key,
                   const std::string &value) {
    auto it = p.find(key);
    return it != p.end() &&
           it->second.kind == duckdb::PropertyValue::Kind::String &&
           it->second.s == value;
}

} // namespace

TEST_CASE("server_started carries only bounded counts/kinds + install_kind", "[telemetry]") {
    ClearOptOut guard;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));

    tel.configureProduct("2026.07.12", "oss");
    tel.serverStarted(/*quack_enabled=*/true, /*reg_count=*/5);

    REQUIRE(raw->product_name == "erpl_rev");
    REQUIRE(raw->product_version == "2026.07.12");
    REQUIRE(raw->product_edition == "oss");

    const auto *ev = raw->find("server_started");
    REQUIRE(ev != nullptr);
    REQUIRE(ev->kind == "capture");
    REQUIRE(ev->props.at("quack_enabled").kind == duckdb::PropertyValue::Kind::Bool);
    REQUIRE(ev->props.at("quack_enabled").b == true);
    REQUIRE(ev->props.at("reg_count").kind == duckdb::PropertyValue::Kind::Int);
    REQUIRE(ev->props.at("reg_count").i == 5);
    REQUIRE(hasStringProp(ev->props, "install_kind", "server"));
}

TEST_CASE("associateDeployment associates the deployment group", "[telemetry]") {
    ClearOptOut guard;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));

    tel.associateDeployment();
    REQUIRE(raw->groups.size() == 1);
    REQUIRE(raw->groups[0].first == "deployment");
}

TEST_CASE("associateAccount hashes the license id (never raw)", "[telemetry]") {
    ClearOptOut guard;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));

    tel.associateAccount("ACME-LICENSE-123");
    REQUIRE(raw->groups.size() == 1);
    REQUIRE(raw->groups[0].first == "account");
    REQUIRE(raw->groups[0].second != "ACME-LICENSE-123");
    REQUIRE(raw->groups[0].second.size() == 64);
}

TEST_CASE("rfc_call emits fm enum + status/duration + install_kind only", "[telemetry]") {
    ClearOptOut guard;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));

    tel.rfcCall("query", /*ok=*/true, 12.5);

    const auto *ev = raw->find("rfc_call");
    REQUIRE(ev != nullptr);
    REQUIRE(ev->kind == "feature");
    REQUIRE(hasStringProp(ev->props, "fm", "query"));
    REQUIRE(hasStringProp(ev->props, "status_class", "2xx"));
    REQUIRE(ev->props.at("duration_ms").kind == duckdb::PropertyValue::Kind::Double);
    REQUIRE(hasStringProp(ev->props, "install_kind", "server"));

    // Guard against leaks: only these keys may ever be present.
    for (const auto &kv : ev->props) {
        const std::string &k = kv.first;
        REQUIRE((k == "fm" || k == "status_class" || k == "duration_ms" ||
                 k == "install_kind"));
    }
}

TEST_CASE("rfc_call marks failures 5xx", "[telemetry]") {
    ClearOptOut guard;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));

    tel.rfcCall("ingest", /*ok=*/false, 3.0);
    const auto *ev = raw->find("rfc_call");
    REQUIRE(ev != nullptr);
    REQUIRE(hasStringProp(ev->props, "status_class", "5xx"));
}

TEST_CASE("error emits enumerated class + feature only", "[telemetry]") {
    ClearOptOut guard;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));

    tel.error("cdc_error", "cdc_apply");
    const auto *ev = raw->find("cdc_error");
    REQUIRE(ev != nullptr);
    REQUIRE(ev->kind == "error");
    REQUIRE(hasStringProp(ev->props, "feature", "cdc_apply"));
}

TEST_CASE("ERPL_REV_NO_TELEMETRY short-circuits every emit", "[telemetry]") {
    EnvGuard guard("ERPL_REV_NO_TELEMETRY", "1");
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));

    tel.configureProduct("1.0.0", "oss");
    tel.associateDeployment();
    tel.serverStarted(false, 1);
    tel.rfcCall("query", true, 1.0);
    tel.error("sql_error", "query");
    tel.flush();

    REQUIRE(raw->calls.empty());
    REQUIRE(raw->groups.empty());
    REQUIRE(raw->product_name.empty());
    REQUIRE(raw->flushes == 0);
}

TEST_CASE("DATAZOO_DISABLE_TELEMETRY short-circuits every emit", "[telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "1");
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));

    tel.rfcCall("query", true, 1.0);
    REQUIRE(raw->calls.empty());
    REQUIRE(tel.isEnabled() == false);
}

TEST_CASE("setEnabled(false) short-circuits every emit", "[telemetry]") {
    ClearOptOut guard;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));
    tel.setEnabled(false);

    tel.serverStarted(false, 1);
    tel.rfcCall("query", true, 1.0);
    REQUIRE(raw->calls.empty());
    REQUIRE(tel.isEnabled() == false);
}

TEST_CASE("sampling decimates the hot path and stamps sample_rate", "[telemetry]") {
    ClearOptOut guard;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend *raw = fake.get();
    erpl_rev::ErplRevTelemetry tel(std::move(fake));
    tel.setSampling(0.5);   // emit 1 of every 2

    for (int i = 0; i < 10; ++i) tel.rfcCall("query", true, 1.0);
    REQUIRE(raw->calls.size() == 5);
    REQUIRE(raw->calls.front().props.at("sample_rate").kind ==
            duckdb::PropertyValue::Kind::Double);

    // Low-volume events are never sampled out.
    tel.serverStarted(false, 1);
    REQUIRE(raw->find("server_started") != nullptr);
}

// End-to-end no-leak check against the REAL library transport: drive the
// production PostHogBackend through a test transport that captures the exact
// serialized batch, and assert the outgoing JSON contains only bounded props —
// no SQL, target/table name, handle, or error message ever appears.
TEST_CASE("real transport payload contains no SAP/SQL leaks", "[telemetry]") {
    ClearOptOut guard;

    auto &lib = duckdb::PostHogTelemetry::Instance();
    lib.ResetShutdownForTesting();
    lib.SetEnabled(true);
    lib.SetAutoFlushEnabledForTesting(false);

    std::vector<std::string> payloads;
    lib.SetTransportForTesting(
        [&](const std::string &, const std::string &,
            const std::vector<duckdb::PostHogEvent> &evs) {
            for (const auto &e : evs) payloads.push_back(e.GetPropertiesJson());
        });

    {
        erpl_rev::ErplRevTelemetry tel;   // real PostHogBackend
        tel.setEnabled(true);
        tel.configureProduct("9.9.9", "oss");
        tel.serverStarted(true, 5);
        tel.rfcCall("query", true, 4.2);
        tel.error("sql_error", "query");
        tel.flush();
    }

    lib.SetTransportForTesting({});   // restore real transport

    REQUIRE_FALSE(payloads.empty());
    std::string all;
    for (const auto &p : payloads) all += p;

    // Bounded, expected content is present. GetPropertiesJson() serialises the
    // event PROPERTIES (not the event name), so assert on property values:
    // `rfc_call` is the feature value of feature_used; `reg_count` is a
    // server_started property; product/install_kind ride the envelope.
    REQUIRE(all.find("rfc_call") != std::string::npos);
    REQUIRE(all.find("reg_count") != std::string::npos);
    REQUIRE(all.find("\"install_kind\"") != std::string::npos);
    REQUIRE(all.find("erpl_rev") != std::string::npos);

    // Simulated sensitive material that the API never accepts must be absent.
    REQUIRE(all.find("SELECT") == std::string::npos);       // SQL
    REQUIRE(all.find("IV_SQL") == std::string::npos);       // RFC param name
    REQUIRE(all.find("MANDT") == std::string::npos);        // SAP table/field
    REQUIRE(all.find("password") == std::string::npos);
}
