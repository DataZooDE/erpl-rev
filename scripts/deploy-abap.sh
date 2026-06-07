#!/bin/bash
# Idempotent (re)deploy of all erpl-rev ABAP objects to the A4H trial.
#
# The trial periodically wipes/reverts runtime + $TMP objects (FMs, classes),
# silently breaking E2E. Re-run this any time to restore a known-good state:
#   - classes (typemap, util, mkfm, setup, reports, test drivers)
#   - the registered-server destination ERPL_REV (method='R')
#   - the RFC FMs Z_DUCKDB_QUERY / Z_DUCKDB_INGEST (run zcl_erpl_rev_mkfm)
#
# Usage: scripts/deploy-abap.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AB="$HERE/abap"
# Credentials from the environment — never hardcode. Export SAP_PASSWORD first.
: "${SAP_PASSWORD:?set SAP_PASSWORD in the environment before running}"
A=(--host localhost --port 50000 --user DEVELOPER --client 001 --password-env SAP_PASSWORD)
adt() { uvx erpl-adt "${A[@]}" "$@"; }

cls() {  # cls <NAME> <file> [<DESC>]
  adt object create --type CLAS/OC --name "$1" --package '$TMP' --description "${3:-erpl-rev}" >/dev/null 2>&1
  if adt source write "$1" --file "$AB/$2" --activate 2>&1 | grep -aiqE 'Activated'; then
    echo "  OK   $1"; else echo "  WARN $1 (activation — check)"; fi
}
prog() {  # prog <NAME> <file> [<DESC>]
  adt object create --type PROG/P --name "$1" --package '$TMP' --description "${3:-erpl-rev}" >/dev/null 2>&1
  if adt source write "$1" --file "$AB/$2" --activate 2>&1 | grep -aiqE 'Activated'; then
    echo "  OK   $1"; else echo "  WARN $1 (activation — check)"; fi
}
tabl() {  # tabl <NAME> <file> [<DESC>]
  adt object create --type TABL/DT --name "$1" --package '$TMP' --description "${3:-erpl-rev}" >/dev/null 2>&1
  if adt source write "$1" --type TABL --file "$AB/$2" --activate 2>&1 | grep -aiqE 'Activated'; then
    echo "  OK   $1"; else echo "  WARN $1 (activation — check)"; fi
}
intf() {  # intf <NAME> <file> [<DESC>]
  adt object create --type INTF/OI --name "$1" --package '$TMP' --description "${3:-erpl-rev}" >/dev/null 2>&1
  if adt source write "$1" --type INTF --file "$AB/$2" --activate 2>&1 | grep -aiqE 'Activated'; then
    echo "  OK   $1"; else echo "  WARN $1 (activation — check)"; fi
}
run() { adt object run "$1" 2>&1 | tr -cd 'A-Za-z0-9 ={}_.:()[] \n-'; }

echo "== example data: wide BSEG-shaped table (420 cols) =="
tabl ZWIDE_BSEG zwide_bseg.ddl "wide BSEG repro (erpl-rev)"
cls  ZCL_WIDE_BSEG zcl_wide_bseg.abap "populate ZWIDE_BSEG"

echo "== delta test table (numeric watermark column) =="
tabl ZDELTA_WM zdelta_wm.ddl "delta watermark test table (erpl-rev)"
# Populate only if empty/absent (100k rows ~ tens of seconds). Uncomment to seed:
#   run ZCL_WIDE_BSEG | grep -aiE 'populated|rows'

echo "== interfaces (before util — replicate signature references it) =="
intf ZIF_ERPL_REV_PROGRESS zif_erpl_rev_progress.intf.abap "replicate progress callback"

echo "== classes (typemap + interface before util — util depends on them) =="
cls ZCL_ERPL_REV_TYPEMAP  zcl_erpl_rev_typemap.abap  "DDIC<->DuckDB type map"
cls ZCL_ERPL_REV_UTIL     zcl_erpl_rev_util.abap     "query/describe/replicate"
cls ZCL_ERPL_REV_DELTA    zcl_erpl_rev_delta.abap    "delta engine (state + 4 readers)"
cls ZCL_ERPL_REV_DELTADRV zcl_erpl_rev_deltadrv.abap "delta change-injection driver"
cls ZCL_ERPL_REV_MKFM     zcl_erpl_rev_mkfm.abap     "create RFC FMs"
cls ZCL_ERPL_REV_SETUP    zcl_erpl_rev_setup.abap    "create registered dest"
cls ZCL_ERPL_REV_TYPETEST zcl_erpl_rev_typetest.abap "typemap tests"
cls ZCL_ERPL_REV_UTILTEST zcl_erpl_rev_utiltest.abap "util tests"
cls ZCL_ERPL_REV_REPLTEST zcl_erpl_rev_repltest.abap "replicate tests"
cls ZCL_ERPL_REV_WIDETEST zcl_erpl_rev_widetest.abap "wide replicate tests"
cls ZCL_ERPL_REV_CONSOLETEST zcl_erpl_rev_consoletest.abap "console realistic query tests (arbitrary column names)"
cls ZCL_ERPL_REV_SLTTEST  zcl_erpl_rev_slttest.abap  "SLT-like replication tests (projection + source filter)"
cls ZCL_ERPL_REV_DIFFTEST zcl_erpl_rev_difftest.abap "data-identity check (replicated == SAP source, every cell)"
cls ZCL_ERPL_REV_PARTEST  zcl_erpl_rev_partest.abap  "partitioned full-load + auto partition-col/job-count tests"
cls ZCL_ERPL_REV_PUBTEST  zcl_erpl_rev_pubtest.abap  "external target publish (parquet/dataset/attached catalog)"
cls ZCL_ERPL_REV_CDSTEST  zcl_erpl_rev_cdstest.abap  "CDS view source (describe/keys/params/discovery/publish)"
cls ZCL_ERPL_REV_BWTEST   zcl_erpl_rev_bwtest.abap   "BW/native (ADBC) source vs a HANA-view stand-in"
cls ZCL_ERPL_REV_DELTATEST zcl_erpl_rev_deltatest.abap "delta E2E (watermark/snapshot/changedoc/insert-only/orchestration)"
# NB: the CDS source tests need the DDLS fixtures ZERPL_C_FLIGHTS + ZERPL_CP_FLIGHTS
# (abap/zerpl_c_flights.ddls.abap, abap/zerpl_cp_flights.ddls.abap) deployed once via
# `erpl-adt object create --type DDLS/DF` + `source write --type DDLS --activate`
# (DDLS activation can exceed the ADT HTTP timeout; it still completes server-side).

echo "== reports (worker before the report — the parallel report SUBMITs it) =="
prog Z_ERPL_REV_REPL_WORKER z_erpl_rev_repl_worker.prog.abap "parallel-replication worker (one key range)"
prog Z_ERPL_REV_REPLICATE z_erpl_rev_replicate.prog.abap "replicate SAP table -> DuckDB (serial + parallel)"
prog Z_ERPL_REV_SQL       z_erpl_rev_sql.prog.abap       "DuckDB SQL console (docking-container UI)"
prog Z_ERPL_REV_DELTA     z_erpl_rev_delta.prog.abap     "delta orchestration loop (cadence + lease)"
prog Z_ERPL_REV_DELTA_SIM z_erpl_rev_delta_sim.prog.abap "delta simulator (inject change + run one cycle)"
prog Z_ERPL_REV_DELTA_SFLIGHT z_erpl_rev_delta_sflight.prog.abap "SFLIGHT delta demo (load/change/run/inspect, GUI)"

echo "== report-path E2E classrun (SUBMITs the report's parallel branch) =="
cls ZCL_ERPL_REV_REPLRUN  zcl_erpl_rev_replrun.abap  "Z_ERPL_REV_REPLICATE parallel-branch E2E"

echo "== registered-server destination ERPL_REV (method='R') =="
run ZCL_ERPL_REV_SETUP | grep -aiE 'OPTS|subrc' | head -2

echo "== RFC function modules Z_DUCKDB_QUERY / Z_DUCKDB_INGEST =="
run ZCL_ERPL_REV_MKFM | grep -aiE 'insert subrc|tfdir' | head -4

echo "== done =="
