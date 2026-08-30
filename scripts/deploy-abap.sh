#!/bin/bash
# Deploy erpl-rev's ABAP objects to the A4H trial.
#
# These objects ARE erpl-rev: the function group, the Z_DUCKDB_* function modules,
# the replication and delta classes, the reports and the test drivers are what a
# customer imports as a transport. They live in abap/ in this repository, and this
# script deploys them without a checkout of any other repository.
#
#   scripts/deploy-abap.sh
#
# Ordering note. erpl's provisioner (erpl/scripts/sap/provision.sh) sequences the
# steps that span repositories -- notably that activating the BW Modeling ADT
# services RESTARTS the ABAP instance, and a restart re-materialises every PSE from
# the database, destroying the SNC and wsRFC certificate trust erpl-proto's live
# tests depend on. Nothing here restarts the instance, so this script is safe to run
# on its own; on a freshly built system run it AFTER that provisioning, because the
# restart also reverts $TMP objects and would discard whatever this deployed.
#
# The trial periodically wipes $TMP, so re-run this whenever E2E starts failing on
# objects that "do not exist".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AB="$HERE/abap"
# Credentials from the environment -- never hardcode. Export SAP_PASSWORD first.
: "${SAP_PASSWORD:?set SAP_PASSWORD in the environment before running}"
A=(--host localhost --port 50000 --user DEVELOPER --client 001 --password-env SAP_PASSWORD
   # Activating the wider objects -- the CDS views especially -- runs past
   # erpl-adt's 120s default read timeout, which surfaces as an opaque
   # "Failed to read connection" while the work continues server-side.
   # See DataZooDE/erpl-adt#42.
   --timeout 600)
adt() { uvx erpl-adt "${A[@]}" "$@"; }

rc=0
dep() {  # dep <adt-type> <NAME> <file> <desc> [<source-type>]
    local t="$1" n="$2" f="$3" d="$4" st="${5:-}"
    if [ ! -f "$AB/$f" ]; then echo "  MISS $n ($f)"; rc=1; return 1; fi
    adt object create --type "$t" --name "$n" --package '$TMP' --description "$d" >/dev/null 2>&1
    local args=(source write "$n" --file "$AB/$f" --activate)
    [ -n "$st" ] && args=(source write "$n" --type "$st" --file "$AB/$f" --activate)
    # "Nothing to activate" is what an UNCHANGED object returns -- success, not
    # failure. Matching only "Activated" made every re-run report the whole
    # catalogue as broken, which trains you to ignore the warnings.
    local out
    out="$(adt "${args[@]}" 2>&1)"
    if grep -aiqE 'Activated|Nothing to activate' <<<"$out"; then
        echo "  OK   $n"
    else
        echo "  WARN $n -- $(grep -aviE '^\s*$' <<<"$out" | tail -1)"; rc=1
    fi
}
cls()  { dep CLAS/OC "$1" "$2" "$3"; }
prog() { dep PROG/P "$1" "$2" "$3"; }
tabl() { dep TABL/DT "$1" "$2" "$3" TABL; }
intf() { dep INTF/OI "$1" "$2" "$3" INTF; }
ddls() { dep DDLS/DF "$1" "$2" "$3" DDLS; }

echo "== DDIC fixtures =="
tabl ZWIDE_BSEG zwide_bseg.ddl "wide BSEG repro (erpl-rev)"
tabl ZDELTA_WM  zdelta_wm.ddl  "delta watermark test table (erpl-rev)"

echo "== interfaces (before util -- replicate's signature references it) =="
intf ZIF_ERPL_REV_PROGRESS zif_erpl_rev_progress.intf.abap "replicate progress callback"

echo "== runtime classes (typemap before util -- util depends on it) =="
cls ZCL_WIDE_BSEG          zcl_wide_bseg.abap          "populate ZWIDE_BSEG"
cls ZCL_ERPL_REV_TYPEMAP   zcl_erpl_rev_typemap.abap   "DDIC<->DuckDB type map"
cls ZCL_ERPL_REV_UTIL      zcl_erpl_rev_util.abap      "query/describe/replicate"
cls ZCL_ERPL_REV_DELTA     zcl_erpl_rev_delta.abap     "delta engine (state + 4 readers)"
cls ZCL_ERPL_REV_DELTADRV  zcl_erpl_rev_deltadrv.abap  "delta change-injection driver"
cls ZCL_ERPL_REV_CDC       zcl_erpl_rev_cdc.abap       "trigger-CDC executor"
cls ZCL_ERPL_REV_MKFM      zcl_erpl_rev_mkfm.abap      "create RFC FMs"
cls ZCL_ERPL_REV_SETUP     zcl_erpl_rev_setup.abap     "create registered dest"

echo "== CDS fixtures (before CDSTEST, which selects from them) =="
ddls ZERPL_C_FLIGHTS  zerpl_c_flights.ddls.abap  "CDS view over SFLIGHT (erpl-rev fixture)"
ddls ZERPL_CP_FLIGHTS zerpl_cp_flights.ddls.abap "CDS view WITH PARAMETERS (erpl-rev fixture)"

echo "== test drivers =="
cls ZCL_ERPL_REV_TYPETEST    zcl_erpl_rev_typetest.abap    "typemap tests"
cls ZCL_ERPL_REV_UTILTEST    zcl_erpl_rev_utiltest.abap    "util tests"
cls ZCL_ERPL_REV_REPLTEST    zcl_erpl_rev_repltest.abap    "replicate tests"
cls ZCL_ERPL_REV_WIDETEST    zcl_erpl_rev_widetest.abap    "wide replicate tests"
cls ZCL_ERPL_REV_CONSOLETEST zcl_erpl_rev_consoletest.abap "console realistic query tests"
cls ZCL_ERPL_REV_SLTTEST     zcl_erpl_rev_slttest.abap     "SLT-like replication tests"
cls ZCL_ERPL_REV_DIFFTEST    zcl_erpl_rev_difftest.abap    "data-identity check (every cell)"
cls ZCL_ERPL_REV_PARTEST     zcl_erpl_rev_partest.abap     "partitioned full-load tests"
cls ZCL_ERPL_REV_PUBTEST     zcl_erpl_rev_pubtest.abap     "external target publish"
cls ZCL_ERPL_REV_CDSTEST     zcl_erpl_rev_cdstest.abap     "CDS view source tests"
cls ZCL_ERPL_REV_BWTEST      zcl_erpl_rev_bwtest.abap      "BW/native (ADBC) source tests"
cls ZCL_ERPL_REV_DELTATEST   zcl_erpl_rev_deltatest.abap   "delta E2E"
cls ZCL_ERPL_REV_CDCTEST     zcl_erpl_rev_cdctest.abap     "trigger-CDC E2E"

echo "== reports (worker before the report -- the parallel branch SUBMITs it) =="
prog Z_ERPL_REV_REPL_WORKER   z_erpl_rev_repl_worker.prog.abap   "parallel-replication worker"
prog Z_ERPL_REV_REPLICATE     z_erpl_rev_replicate.prog.abap     "replicate SAP table -> DuckDB"
prog Z_ERPL_REV_SQL           z_erpl_rev_sql.prog.abap           "DuckDB SQL console"
prog Z_ERPL_REV_DELTA         z_erpl_rev_delta.prog.abap         "delta orchestration loop"
prog Z_ERPL_REV_DELTA_SFLIGHT z_erpl_rev_delta_sflight.prog.abap "SFLIGHT delta demo"
cls  ZCL_ERPL_REV_REPLRUN     zcl_erpl_rev_replrun.abap          "report parallel-branch E2E"

# RS_FUNCTIONMODULE_INSERT fails with invalid_function_pool unless the group already
# exists, and nothing else creates it -- so on a fresh container every FM is silently
# lost. This must run BEFORE ZCL_ERPL_REV_MKFM.
echo "== function group ZERPL_REV (host of the RFC FMs) =="
adt object create --type FUGR/F --name ZERPL_REV --package '$TMP' \
    --description "erpl-rev RFC function group" >/dev/null 2>&1 || true

echo "== destination + generated RFC FMs =="
if adt object run ZCL_ERPL_REV_SETUP >/dev/null 2>&1; then echo "  OK   ZERPL_REV destination"
else echo "  WARN ZERPL_REV destination"; rc=1; fi
if adt object run ZCL_ERPL_REV_MKFM  >/dev/null 2>&1; then echo "  OK   RFC FMs (Z_DUCKDB_*)"
else echo "  WARN RFC FMs (Z_DUCKDB_*)"; rc=1; fi

# Seed the wide fixture. The data-identity E2E stage compares ZWIDE_BSEG cell by
# cell, so an unseeded table fails it with `rows=0 cols=390` -- a result that points
# at nothing. The class DELETEs first, so this is idempotent; 100k rows is ~20s.
echo "== seed ZWIDE_BSEG =="
if adt object run ZCL_WIDE_BSEG 2>&1 | grep -aqE 'populated'; then echo "  OK   ZWIDE_BSEG seeded"
else echo "  WARN ZWIDE_BSEG seed"; rc=1; fi

if [ "$rc" -eq 0 ]; then echo "== deploy complete =="; else echo "== deploy finished WITH WARNINGS =="; fi
exit $rc
