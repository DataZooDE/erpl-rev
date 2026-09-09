// Prometheus exposition for the operational views.
//
// A text rendering of erpl_rev_targets and erpl_rev_health, which is what every
// other operator surface reads too -- so a dashboard, the TUI and the CLI cannot
// disagree about whether a target is healthy.
//
// Pure with respect to the database: give it a bridge, get the text. The
// listener that serves it owns no logic of its own.
#pragma once

#include <string>

#include "duckdb_bridge.hpp"

namespace erpl_rev {
namespace metrics {

// The full exposition, ready to return from GET /metrics.
std::string Render(DuckDbBridge &db);

// A Prometheus label value: backslash, quote and newline escaped. Target names
// are customer-chosen, and an unescaped quote ends the label early -- every
// metric after it is silently discarded by the scraper, which is a monitoring
// outage caused by a table name.
std::string EscapeLabel(const std::string &v);


// A minimal HTTP listener that serves Render() at GET /metrics.
//
// Its own socket rather than a route on the quack listener: quack is a DuckDB
// extension serving its own protocol, and a scraper speaks HTTP. Deliberately
// tiny -- one thread, one request at a time, no keep-alive. A metrics endpoint
// that needs a web framework is a liability on a replication server.
//
// Off unless a port is configured. Start returns false if the port cannot be
// bound, which the caller reports rather than dying: failing to expose metrics
// must never stop replication.
namespace server {
bool Start(DuckDbBridge &db, int port, std::string &error);
void Stop();
}  // namespace server

}  // namespace metrics
}  // namespace erpl_rev
