// Hard-coded RFC function-module metadata for the erpl-rev server.
//
// These descriptions MUST mirror the ABAP-side FM interfaces exactly — same
// parameter names, kinds and types. RfcInstallServerFunction() does NOT copy the
// description, so each handle is built once and cached in static storage.
#pragma once

#include "sapnwrfc.h"

namespace erpl_rev {

// STFC_CONNECTION: standard ping FM (CHAR(255) params). Walking-skeleton check.
RFC_FUNCTION_DESC_HANDLE BuildPingDesc();

// Z_DUCKDB_QUERY: IMPORTING IV_SQL; EXPORTING EV_COLUMNS, EV_ROWS,
// EV_ROW_COUNT, EV_ERROR. All TYPE STRING.
RFC_FUNCTION_DESC_HANDLE BuildQueryDesc();

// Z_DUCKDB_INGEST: IMPORTING IV_TARGET, IV_MODE, IV_KEYS, IV_PARQUET_OUT,
// IV_DATA, IV_OP_COL; EXPORTING EV_ROWS_AFFECTED, EV_ERROR. All TYPE STRING.
// IV_OP_COL names the I/U/D control column for IV_MODE='MERGE' (delta apply).
RFC_FUNCTION_DESC_HANDLE BuildIngestDesc();

// Z_DUCKDB_SNAPSHOT_MERGE: IMPORTING IV_TARGET, IV_STAGING, IV_KEYS; EXPORTING
// EV_INS, EV_UPD, EV_DEL, EV_ERROR. All TYPE STRING. Diff-merges a fresh full
// snapshot onto the target (upsert + delete-missing) for the SNAPSHOT method.
RFC_FUNCTION_DESC_HANDLE BuildSnapshotMergeDesc();

// Streaming cursor FMs (fixed-memory paging; BXML payload is XSTRING):
//  Z_DUCKDB_OPEN : IMPORTING IV_SQL → EXPORTING EV_HANDLE, EV_COLUMNS, EV_ERROR.
//  Z_DUCKDB_FETCH: IMPORTING IV_HANDLE, IV_PAGE_ROWS → EXPORTING EV_XDATA
//                  (XSTRING binary sXML), EV_FETCHED, EV_DONE, EV_ERROR.
//  Z_DUCKDB_CLOSE: IMPORTING IV_HANDLE → EXPORTING EV_ERROR.
RFC_FUNCTION_DESC_HANDLE BuildOpenDesc();
RFC_FUNCTION_DESC_HANDLE BuildFetchDesc();
RFC_FUNCTION_DESC_HANDLE BuildCloseDesc();

} // namespace erpl_rev
