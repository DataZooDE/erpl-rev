// RFC server function handlers (extern "C" shims dispatching into C++).
#pragma once

#include <string>

#include "sapnwrfc.h"

namespace erpl_rev {

// Install all server function handlers (STFC_CONNECTION ping + the two DuckDB
// bridge FMs). Creates the shared DuckDbBridge backed by `db_path` (empty =>
// in-memory). init_sql is boot SQL (ATTACH/secret/extension setup) for external
// replication targets; empty => env ERPL_REV_DUCKDB_INIT.
void InstallHandlers(const std::string &db_path = "", const std::string &init_sql = "");

// Release the shared DuckDbBridge (closes DuckDB and any attached catalogs).
// Call once during shutdown, AFTER the RFC server is stopped so no handler can
// touch the bridge — this runs the DuckDB/extension teardown while the runtime
// is alive, instead of leaving it to the global's atexit destructor (where some
// extensions, e.g. MotherDuck, crash logging from their own destructors).
void ShutdownHandlers();

// Start/stop the quack network server on the shared DuckDbBridge. Both take the
// same mutex as the RFC handlers so DuckDB access stays serialised.
// StartQuackServer returns quack_serve's result (JSON array, incl. auth token).
// A non-empty `token` pins the client auth token instead of a random one.
std::string StartQuackServer(const std::string &listen, bool allow_other_host,
                             const std::string &token = "");
void        StopQuackServer(const std::string &listen);

} // namespace erpl_rev

// RFC_SERVER_FUNCTION callbacks must have C linkage to match the SDK typedef.
extern "C" {
RFC_RC SAP_API ZPingImpl  (RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
RFC_RC SAP_API ZQueryImpl (RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
RFC_RC SAP_API ZIngestImpl(RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
RFC_RC SAP_API ZSnapshotMergeImpl(RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
RFC_RC SAP_API ZOpenImpl  (RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
RFC_RC SAP_API ZFetchImpl (RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
RFC_RC SAP_API ZCloseImpl (RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
RFC_RC SAP_API ZCdcPlanImpl (RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
RFC_RC SAP_API ZCdcApplyImpl(RFC_CONNECTION_HANDLE, RFC_FUNCTION_HANDLE, RFC_ERROR_INFO *);
}
