#include <catch2/catch_test_macros.hpp>

#include "telemetry.hpp"

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

using namespace erpl_rev;

namespace {

struct Captured {
    std::string                  event;
    std::vector<Telemetry::Prop> props;
};

// Thread-safe sink for the test backend (the worker calls it off-thread).
struct Sink {
    std::mutex            mu;
    std::vector<Captured> events;

    void clear() {
        std::lock_guard<std::mutex> l(mu);
        events.clear();
    }
    size_t size() {
        std::lock_guard<std::mutex> l(mu);
        return events.size();
    }
};

// Find a property value by key in a captured event.
std::string PropValue(const Captured &c, const std::string &key) {
    for (const auto &p : c.props)
        if (p.key == key) return p.value;
    return {};
}

// Make sure no ambient opt-out env var is set for the enabled-path tests.
void ClearOptOutEnv() {
    ::unsetenv("ERPL_REV_NO_TELEMETRY");
    ::unsetenv("DATAZOO_DISABLE_TELEMETRY");
}

} // namespace

TEST_CASE("telemetry forwards events and properties to the backend", "[telemetry]") {
    ClearOptOutEnv();
    Sink sink;
    Telemetry::SetBackendForTesting(
        [&sink](const std::string &event, const std::vector<Telemetry::Prop> &props) {
            std::lock_guard<std::mutex> l(sink.mu);
            sink.events.push_back({event, props});
        });

    Telemetry::Initialize(/*user_disabled=*/false, "1.2.3");
    REQUIRE(Telemetry::IsEnabled());

    Telemetry::Track("application_start");
    Telemetry::Track("query_execution",
                     {{"kind", "query"}, {"row_count", "42", true}});
    Telemetry::Track("replication_execution",
                     {{"mode", "cdc_apply"}, {"rows_affected", "7", true}});

    // Shutdown drains the queue and joins the worker, so all events are visible.
    Telemetry::Shutdown();
    REQUIRE_FALSE(Telemetry::IsEnabled());

    REQUIRE(sink.size() == 3);
    CHECK(sink.events[0].event == "application_start");
    CHECK(sink.events[0].props.empty());

    CHECK(sink.events[1].event == "query_execution");
    CHECK(PropValue(sink.events[1], "kind") == "query");
    CHECK(PropValue(sink.events[1], "row_count") == "42");

    CHECK(sink.events[2].event == "replication_execution");
    CHECK(PropValue(sink.events[2], "mode") == "cdc_apply");
    CHECK(PropValue(sink.events[2], "rows_affected") == "7");

    Telemetry::SetBackendForTesting(nullptr);
}

TEST_CASE("telemetry is a no-op when the user disables it", "[telemetry]") {
    ClearOptOutEnv();
    Sink sink;
    Telemetry::SetBackendForTesting(
        [&sink](const std::string &event, const std::vector<Telemetry::Prop> &props) {
            (void)event; (void)props;
            std::lock_guard<std::mutex> l(sink.mu);
            sink.events.push_back({event, props});
        });

    Telemetry::Initialize(/*user_disabled=*/true, "1.2.3");
    CHECK_FALSE(Telemetry::IsEnabled());

    Telemetry::Track("application_start");
    Telemetry::Shutdown();
    CHECK(sink.size() == 0);

    Telemetry::SetBackendForTesting(nullptr);
}

TEST_CASE("telemetry respects the ERPL_REV_NO_TELEMETRY env var", "[telemetry]") {
    ClearOptOutEnv();
    ::setenv("ERPL_REV_NO_TELEMETRY", "1", /*overwrite=*/1);

    Telemetry::Initialize(/*user_disabled=*/false, "1.2.3");
    CHECK_FALSE(Telemetry::IsEnabled());

    ::unsetenv("ERPL_REV_NO_TELEMETRY");
}
