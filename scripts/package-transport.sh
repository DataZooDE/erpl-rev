#!/bin/bash
# Package erpl-rev's ABAP into a transportable SAP package hierarchy and a
# workbench request, ready to RELEASE into a shippable transport (cofile + data).
#
# Run this on a DEV system that has STMS configured (transport routes / TP profile).
# On such a system the final `transport release` writes K9*/R9* to /usr/sap/trans;
# zip those with docs/INSTALL.md to deliver (the Theobald model — see docs/INSTALL.md).
#
# NB (trial limitation): a minimal AS-ABAP trial with a TMS *domain* but no transport
# *routes/TP profile* can create the packages, assign objects, and release LOGICALLY,
# but will NOT emit the physical cofile/data. Configure STMS routes (or run on a real
# dev system) to get the files.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; AB="$HERE/abap"
# Credentials from the environment — never hardcode (A4H trial = SAP-published default).
: "${SAP_PASSWORD:?set SAP_PASSWORD in the environment before running}"
A=(--host "${SAP_HOST:-localhost}" --port "${SAP_PORT:-50000}" --user "${SAP_USER:-DEVELOPER}" --client "${SAP_CLIENT:-001}" --password-env SAP_PASSWORD)
adt() { uvx erpl-adt "${A[@]}" "$@"; }
clean() { sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' | tr -d '\r'; }

# --- production objects -> ZERPL_CORE ---------------------------------------
#   "<NAME> <TYPE> <file>"   TYPE in {INTF,CLAS,PROG}
# This list MUST equal zcl_erpl_rev_footprint.abap's `expected` list, which is the
# checked-in statement of everything erpl-rev delivers into a customer system. It did
# not: the transport carried seven of the fourteen, and the six it omitted were the
# ones that make the product work without S_DEVELOP --
# ZCL_ERPL_REV_CLIDRV above all, whose absence silently turned every CLI verb back
# into the temporary-class path the docs promise it avoids. ZCL_ERPL_REV_DIAG was
# missing too, which is what docs/INSTALL.md tells you to run as the smoke test.
#
# scripts/check-transport-complete.sh asserts the equality, and e2e runs it, so the
# two lists cannot drift apart again.
CORE=(
  "ZIF_ERPL_REV_PROGRESS INTF zif_erpl_rev_progress.intf.abap"
  "ZCL_ERPL_REV_TYPEMAP  CLAS zcl_erpl_rev_typemap.abap"
  "ZCL_ERPL_REV_UTIL     CLAS zcl_erpl_rev_util.abap"
  "ZCL_ERPL_REV_DELTA    CLAS zcl_erpl_rev_delta.abap"
  "ZCL_ERPL_REV_CDC      CLAS zcl_erpl_rev_cdc.abap"
  "ZCL_ERPL_REV_SETUP    CLAS zcl_erpl_rev_setup.abap"
  "ZCL_ERPL_REV_MKFM     CLAS zcl_erpl_rev_mkfm.abap"
  "ZCL_ERPL_REV_DIAG     CLAS zcl_erpl_rev_diag.abap"
  "ZCL_ERPL_REV_CLIDRV   CLAS zcl_erpl_rev_clidrv.abap"
  "Z_ERPL_REV_REPLICATE   PROG z_erpl_rev_replicate.prog.abap"
  "Z_ERPL_REV_SQL         PROG z_erpl_rev_sql.prog.abap"
  "Z_ERPL_REV_REPL_WORKER PROG z_erpl_rev_repl_worker.prog.abap"
  "Z_ERPL_REV_DELTA       PROG z_erpl_rev_delta.prog.abap"
  "Z_ERPL_REV_DAEMON      PROG z_erpl_rev_daemon.prog.abap"
)
# Function group ZERPL_REV + its nine RFC FMs are created by ZCL_ERPL_REV_MKFM (run
# with the transport so they land in ZERPL_CORE as transported objects — see step 4).

# --- test/demo/fixtures -> ZERPL_TEST (NOT in the production transport) ------
TEST=(
  "ZCL_ERPL_REV_TYPETEST   CLAS zcl_erpl_rev_typetest.abap"
  "ZCL_ERPL_REV_UTILTEST   CLAS zcl_erpl_rev_utiltest.abap"
  "ZCL_ERPL_REV_REPLTEST   CLAS zcl_erpl_rev_repltest.abap"
  "ZCL_ERPL_REV_WIDETEST   CLAS zcl_erpl_rev_widetest.abap"
  "ZCL_ERPL_REV_CONSOLETEST CLAS zcl_erpl_rev_consoletest.abap"
  "ZCL_ERPL_REV_SLTTEST    CLAS zcl_erpl_rev_slttest.abap"
  "ZCL_ERPL_REV_DIFFTEST   CLAS zcl_erpl_rev_difftest.abap"
  "ZCL_ERPL_REV_PARTEST    CLAS zcl_erpl_rev_partest.abap"
  "ZCL_ERPL_REV_PUBTEST    CLAS zcl_erpl_rev_pubtest.abap"
  "ZCL_ERPL_REV_CDSTEST    CLAS zcl_erpl_rev_cdstest.abap"
  "ZCL_ERPL_REV_BWTEST     CLAS zcl_erpl_rev_bwtest.abap"
  "ZCL_ERPL_REV_STREAMTEST CLAS zcl_erpl_rev_streamtest.abap"
  "ZCL_ERPL_REV_REPLRUN    CLAS zcl_erpl_rev_replrun.abap"
  "ZCL_ERPL_REV_QUERY      CLAS zcl_erpl_rev_query.abap"
  "ZCL_ERPL_REV_INGEST     CLAS zcl_erpl_rev_ingest.abap"
  "ZCL_ERPL_REV_DIAG       CLAS zcl_erpl_rev_diag.abap"
  "ZCL_ERPL_REV_PARDEMO    CLAS zcl_erpl_rev_pardemo.abap"
  "ZCL_WIDE_BSEG           CLAS zcl_wide_bseg.abap"
  "ZCL_STOCK_MOVE_FILL     CLAS zcl_stock_move_fill.abap"
  "Z_WIDE_BSEG_FILL        PROG z_wide_bseg_fill.prog.abap"
)
# DDIC + CDS fixtures (separate types): ZWIDE_BSEG (TABL), ZERPL_C_FLIGHTS /
# ZERPL_CP_FLIGHTS (DDLS) -> ZERPL_TEST (see deploy-abap.sh for type strings).

deploy() {           # <pkg> <req> "<NAME> <TYPE> <file>"
  local pkg="$1" req="$2"; read -r name type file <<<"$3"
  local t; case "$type" in INTF) t=INTF/OI;; CLAS) t=CLAS/OC;; PROG) t=PROG/P;; esac
  adt object create --type "$t" --name "$name" --package "$pkg" --transport "$req" \
      --description erpl-rev >/dev/null 2>&1
  adt source write "$name" --type "$type" --file "$AB/$file" --transport "$req" --activate 2>&1 \
      | clean | grep -aiE 'activated|cancelled|error' | head -1
}

echo "== 1. bootstrap packages (ZERPL / ZERPL_CORE / ZERPL_TEST) =="
adt object create --type CLAS/OC --name ZCL_ERPL_REV_PKG --package '$TMP' --description bootstrap >/dev/null 2>&1
adt source write ZCL_ERPL_REV_PKG --file "$AB/zcl_erpl_rev_pkg.abap" --activate >/dev/null 2>&1
adt object run ZCL_ERPL_REV_PKG 2>&1 | clean | grep -aiE 'created|exists|REQ='

echo "== 2. transport request for the production set =="
REQ=$(adt transport create --package ZERPL_CORE --desc "erpl-rev ${VERSION:-dev} production" 2>&1 | clean | grep -aoE 'A4HK[0-9]+|[A-Z0-9]{3}K[0-9]+' | head -1)
echo "   request: $REQ"

echo "== 3. deploy production objects -> ZERPL_CORE =="
for o in "${CORE[@]}"; do echo -n "   $o -> "; deploy ZERPL_CORE "$REQ" "$o"; done

echo "== 4. function group ZERPL_REV + RFC FMs (transported) =="
# create empty FUGR in ZERPL_CORE first (SE80/erpl-adt), then MKFM inserts the FMs
# with the transport so they ride ZERPL_CORE rather than landing in \$TMP.
adt object create --type FUGR/F --name ZERPL_REV --package ZERPL_CORE --transport "$REQ" \
    --description "erpl-rev RFC FMs" 2>&1 | clean | tail -1
adt object run ZCL_ERPL_REV_MKFM 2>&1 | clean | grep -aiE 'insert|tfdir' | head -6

echo "== 5. deploy tests/fixtures -> ZERPL_TEST =="
for o in "${TEST[@]}"; do echo -n "   $o -> "; deploy ZERPL_TEST "$REQ" "$o"; done

echo "== 6. RELEASE the request (on a DEV system with STMS this writes K9*/R9*) =="
adt transport release "$REQ" 2>&1 | clean | tail -2
echo "   On a TP-profile system: docker exec / cp /usr/sap/trans/{cofiles/K*,data/R*} -> transport/"
echo "== done: request $REQ =="
