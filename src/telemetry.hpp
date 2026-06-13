// Lightweight, fire-and-forget telemetry for the erpl-rev RFC server.
//
// Sends anonymous usage events to the Datazoo PostHog project. The distinct id
// is a SHA256 of the machine id — no SQL, table names, paths, hostnames, or
// credentials are ever included. Events:
//   application_start       once, when the server starts listening
//   query_execution         on Z_DUCKDB_QUERY / Z_DUCKDB_OPEN (kind, row_count)
//   replication_execution   on Z_DUCKDB_SNAPSHOT_MERGE / CDC_APPLY (mode, rows_affected)
//
// Opt-out (checked in order):
//   1. --no-telemetry CLI flag → Initialize(/*user_disabled=*/true, ...)
//   2. ERPL_REV_NO_TELEMETRY=1|true|yes  env var
//   3. DATAZOO_DISABLE_TELEMETRY=1|true|yes  env var (cross-product)
//
// Unlike a one-shot CLI, the server is long-running: a single background worker
// drains a bounded queue, so Track() never blocks an RFC handler thread and the
// server never spawns an unbounded number of HTTP threads.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace erpl_rev {

class Telemetry {
public:
    // One event property. `numeric` emits the value as a bare JSON number
    // (e.g. row counts); otherwise it is sent as a quoted string.
    struct Prop {
        std::string key;
        std::string value;
        bool        numeric = false;
    };

    // Call once at startup. Applies all opt-out checks; starts the background
    // worker only when telemetry ends up enabled.
    static void Initialize(bool user_disabled, const std::string &version);

    static bool IsEnabled() noexcept;

    // Enqueue an anonymous event. Non-blocking; safe to call from any thread
    // (including SAP RFC handler threads). No-op when disabled. Events are
    // dropped (not blocked) if the queue backs up because the endpoint is down.
    static void Track(const std::string &event, std::vector<Prop> props = {});

    // Drain the queue (best-effort, bounded by the per-request timeouts) and
    // stop the worker. Call once at server shutdown.
    static void Shutdown();

    // Replace the HTTP backend in unit tests (pass nullptr to restore default).
    using Backend = std::function<void(const std::string &event,
                                       const std::vector<Prop> &props)>;
    static void SetBackendForTesting(Backend backend);
};

} // namespace erpl_rev
