#!/bin/bash
# End-to-end test for erpl-rev against the live A4H docker system.
#
# Assumes one-time setup (see README / docs/enable-rfc-registration.md):
#   - A4H up with gw/acl_mode=0                 (scripts/start-sap.sh)
#   - destination ERPL_REV exists method='R' (deploy+run zcl_erpl_rev_setup)
#   - FMs Z_DUCKDB_QUERY / Z_DUCKDB_INGEST exist (deploy+run zcl_erpl_rev_mkfm)
#
# Builds, starts the server, deploys+runs the caller classruns, asserts results.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NW="${SAPNWRFC_HOME:-$HERE/nwrfcsdk/linux}/lib"
# Which NW RFC C ABI to exercise: `sdk` (SAP's) or `proto` (erpl-proto's
# pure-Rust shim). Both must produce identical results -- that equality is the
# acceptance test for removing the SDK.
RFC_BACKEND="${RFC_BACKEND:-sdk}"
# shared or static, proto backend only. Static puts the shim inside the binary,
# so `ldd` shows no RFC library at all.
RFC_LINK="${RFC_LINK:-shared}"
ERPL_PROTO_ROOT="${ERPL_PROTO_ROOT:-}"
if [ "$RFC_BACKEND" = proto ]; then RFC_LIB_DIR="$ERPL_PROTO_ROOT/target/release"; else RFC_LIB_DIR="$NW"; fi
VCPKG="${VCPKG_ROOT:-$HOME/.local/share/vcpkg}"
TRIPLET="${VCPKG_TRIPLET:-x64-linux}"
# Credentials come from the environment — never hardcode. Export SAP_PASSWORD before
# running (for the A4H trial it's the SAP-published developer-edition default).
: "${SAP_PASSWORD:?set SAP_PASSWORD in the environment before running}"
ADT=(--host localhost --port 50000 --user DEVELOPER --client 001 --password-env SAP_PASSWORD)
# The ABAP sources moved to erpl (see scripts/deploy-abap.sh and DataZooDE/erpl#123),
# so they are no longer under this repo's abap/. Resolve them from an erpl checkout
# the same way deploy-abap.sh does, and say so loudly if it is missing -- silently
# skipping the drivers would make the suite fail with an activation error that names
# nothing, or worse, read green.
ERPL_DIR="${ERPL_DIR:-$HERE/../erpl}"
ABAP_DIR="$ERPL_DIR/scripts/sap/assets/rev/abap"
if [ ! -d "$ABAP_DIR" ]; then
    echo "E2E FAIL: no erpl checkout with the ABAP assets" >&2
    echo "  looked in: $ABAP_DIR" >&2
    echo "  set ERPL_DIR to an erpl checkout, or clone erpl beside this repository." >&2
    exit 1
fi

cd "$HERE"
SRV=""
fail() { echo "E2E FAIL: $*" >&2; [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; exit 1; }

echo "== build =="
if [ "$RFC_BACKEND" = proto ]; then
  cargo build --release -p erpl-proto-nwrfc \
        --manifest-path "$ERPL_PROTO_ROOT/Cargo.toml" >/dev/null 2>&1 \
    || fail "build erpl-proto's nwrfc shim"
fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DRFC_BACKEND="$RFC_BACKEND" -DRFC_LINK="$RFC_LINK" \
      -DERPL_PROTO_ROOT="$ERPL_PROTO_ROOT" \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG/scripts/buildsystems/vcpkg.cmake" \
      -DVCPKG_TARGET_TRIPLET="$TRIPLET" -DVCPKG_HOST_TRIPLET="$TRIPLET" \
      >/dev/null 2>&1 || fail "configure"
cmake --build build >/dev/null 2>&1 || fail "build"
./build/erpl_rev_tests >/dev/null 2>&1 || fail "unit tests"
echo "   build + unit tests OK (incl. delta merge/snapshot/state cases)"

echo "== start server =="
rm -f /tmp/erpl_rev_sap_export.parquet
# Isolate the DB: the server is now file-backed by default, so give e2e a fresh
# throwaway file (removed around the run) — never the persistent default.
E2E_DB="/tmp/erpl_e2e_$$.duckdb"
rm -f "$E2E_DB" "$E2E_DB".wal
LD_LIBRARY_PATH="$RFC_LIB_DIR" ./build/erpl_rev_server --db "$E2E_DB" >/tmp/erpl_e2e_srv.log 2>&1 &
SRV=$!
sleep 6
ps -p "$SRV" >/dev/null || { cat /tmp/erpl_e2e_srv.log; fail "server died"; }

run() {  # cls file -> $OUT (cleaned). Uses ABSOLUTE --file path.
  local cls="$1" file="$ABAP_DIR/$(basename "$2")"
  uvx erpl-adt "${ADT[@]}" object create --type CLAS/OC --name "$cls" \
      --package '$TMP' --description e2e >/dev/null 2>&1
  uvx erpl-adt "${ADT[@]}" source write "$cls" --file "$file" --activate >/dev/null 2>&1 \
      || fail "activate $cls"
  OUT=$(uvx erpl-adt "${ADT[@]}" object run "$cls" 2>&1 \
        | tr -cd 'a-zA-Z0-9 =_:.,;{}[]()"/ \n*-')
}

echo "== Query E2E (taxi parquet) =="
run ZCL_ERPL_REV_QUERY abap/zcl_erpl_rev_query.abap
echo "$OUT" | grep -q 'ROW_COUNT=2' || fail "query row_count ($OUT)"
echo "$OUT" | grep -q '"payment_type":"CARD","c":4,"s":86.00' || fail "query CARD"
echo "$OUT" | grep -q '"payment_type":"CASH","c":2,"s":12.50' || fail "query CASH"
echo "   query OK"

echo "== Ingest E2E (SAP T000 -> parquet, UPSERT) =="
run ZCL_ERPL_REV_INGEST abap/zcl_erpl_rev_ingest.abap
echo "$OUT" | grep -q 'ERPL-REV-UPSERT' || fail "ingest upsert verify ($OUT)"
echo "$OUT" | grep -q 'affected=' || fail "ingest affected"
[ -f /tmp/erpl_rev_sap_export.parquet ] || fail "parquet not written"
echo "   ingest OK (parquet written, upsert verified)"

echo "== Console E2E (arbitrary SQL -> arbitrary result column names) =="
# Realistic console queries: count(*), unaliased arith/func/CASE, >30-char
# expression name, colliding sanitized names, NULL, mixed types, unicode.
# Catches the class of bug where DuckDB column names (count_star(), (1 + 2), ...)
# are invalid ABAP component names and crash the RTTS structure build.
run ZCL_ERPL_REV_CONSOLETEST abap/zcl_erpl_rev_consoletest.abap
echo "$OUT" | grep -q 'CONSOLE RESULT pass=9 fail=0' || fail "console test ($OUT)"
echo "   console OK (9/9 realistic queries, no dump)"

echo "== SLT-like replication E2E (field selection + source filter) =="
# Projection keeps only the chosen columns (keys auto-retained), the filter is
# applied at the SAP source (only matching rows transferred), UPSERT dedups on
# re-run, and a bad column / bad WHERE returns a clean error instead of a dump.
run ZCL_ERPL_REV_SLTTEST abap/zcl_erpl_rev_slttest.abap
echo "$OUT" | grep -q 'SLT RESULT pass=37 fail=0' || fail "slt test ($OUT)"
echo "   slt OK (37/37: projection, source filter, key-retention, error-safety, value-help, display)"

echo "== Data-identity E2E (replicated == SAP source, every cell) =="
# Exhaustively compares the replicated DuckDB target against the SAP source:
# SFLIGHT every row x every column (direct), ZWIDE_BSEG 3000 rows x 390 cols
# (per-row md5), REPOSRC 200 rows x 34 cols (large multi-chunk RSTR DATA + blank
# DATS), plus a negative control proving the compare detects a change.
run ZCL_ERPL_REV_DIFFTEST abap/zcl_erpl_rev_difftest.abap
echo "$OUT" | grep -q 'DIFF RESULT pass=4 fail=0' || fail "diff test ($OUT)"
echo "   diff OK (SFLIGHT 94x14 + ZWIDE 3000x390 + REPOSRC 200x34 identical; corruption detected)"

echo "== Delta (incremental) E2E (watermark / snapshot / change-doc / insert-only / orchestration) =="
# Proves delta against REAL SAP transactions: a direct Open SQL change to ZDELTA_WM
# (watermark merge + idempotent re-run), a physical DELETE reflected only via the
# snapshot anti-join, a real BAPI_MATERIAL_SAVEDATA (MM02) writing genuine CDHDR/CDPOS
# picked up by the change-doc re-read + the insert-only 2-step, and the orchestration
# lease/granularity-gate/due-catch-up. Needs the delta classes + ZDELTA_WM + the new
# Z_DUCKDB_SNAPSHOT_MERGE FM deployed (scripts/deploy-abap.sh).
run ZCL_ERPL_REV_DELTATEST abap/zcl_erpl_rev_deltatest.abap
echo "$OUT" | grep -qE 'DELTA RESULT pass=[0-9]+ fail=0' || fail "delta test ($OUT)"
echo "   delta OK (watermark+idempotent, snapshot delete, real material change-doc/insert-only, orchestration)"

# Trigger-CDC (opt-in physical-delete tier, ADR-0004): provisions REAL HANA triggers
# on the source via the server-generated DDL, physically deletes rows, and proves one
# CDC cycle reflects the deletes in the DuckDB target; idempotent re-run; teardown
# leaves no orphan objects. Needs ZCL_ERPL_REV_CDC[TEST] + the CDC FMs (mkfm).
echo "== Trigger-CDC E2E (real HANA triggers, physical deletes) =="
run ZCL_ERPL_REV_CDCTEST abap/zcl_erpl_rev_cdctest.abap
echo "$OUT" | grep -qE 'CDC RESULT pass=[0-9]+ fail=0' || fail "cdc test ($OUT)"
echo "   trigger-CDC OK (provision real triggers, capture physical deletes, idempotent, teardown)"

echo "== Partitioned full-load E2E (coordinator heap + workers + deferred PK) =="
# Two workers append DISJOINT key ranges into one heap (iv_create=false,
# iv_build_pk=false); the coordinator builds the PRIMARY KEY once. Proves the merge
# + deferred-PK contract behind parallel background-job replication.
run ZCL_ERPL_REV_PARTEST abap/zcl_erpl_rev_partest.abap
echo "$OUT" | grep -q 'PARTITION RESULT pass=9 fail=0' || fail "partition test ($OUT)"
echo "   partition OK (2 disjoint workers -> one heap, PK built once, count parity;"
echo "                 auto partition-col pick + auto job-count recommend)"

echo "== Report parallel-branch E2E (Z_ERPL_REV_REPLICATE, real background jobs) =="
# SUBMITs the end-user report with the parallel checkbox set; the report auto-picks
# the partition column (BELNR) and runs 4 background worker jobs into one target,
# building the PK once. Needs the report + worker PROGs deployed (deploy-abap.sh)
# and >=1 free batch WP. Reads the report's list from memory and asserts parity.
run ZCL_ERPL_REV_REPLRUN abap/zcl_erpl_rev_replrun.abap
echo "$OUT" | grep -q 'REPLRUN RESULT pass=6 fail=0' || fail "report parallel test ($OUT)"
echo "   report parallel OK (auto BELNR partition, 4 jobs, verify + parity, worker job-log progress)"

echo "== External-target E2E (stage-then-publish: parquet / dataset / attached catalog) =="
# Replicate a SAP slice into a local DuckDB holding table, then publish it via one
# DuckDB statement to: a parquet file, a partitioned parquet dataset, and a table in
# an ATTACHed catalog (a 2nd DuckDB file = the same SQL path as postgres/ducklake/
# bigquery/iceberg). Plus the end-user report (Z_ERPL_REV_REPLICATE) writing parquet.
run ZCL_ERPL_REV_PUBTEST abap/zcl_erpl_rev_pubtest.abap
echo "$OUT" | grep -q 'PUBTEST RESULT pass=6 fail=0' || fail "publish test ($OUT)"
echo "   publish OK (parquet file + dataset, attached-catalog full+append, report->parquet)"

echo "== CDS view source E2E (DDIF resolves CDS; NODE filtered, keys auto-detected) =="
# Replicate a CDS view entity (ZERPL_C_FLIGHTS over SFLIGHT) through the normal path:
# describe + auto keys + count/value parity + CDS->parquet + WITH PARAMETERS + F4.
# Needs the DDLS fixtures (one-time, see deploy-abap.sh).
run ZCL_ERPL_REV_CDSTEST abap/zcl_erpl_rev_cdstest.abap
echo "$OUT" | grep -q 'CDS RESULT pass=8 fail=0' || fail "cds test ($OUT)"
echo "   cds OK (describe/NODE-filter, auto keys, count+value parity, parquet, params, F4, is_cds)"

echo "== BW/native (ADBC) source E2E (HANA-view stand-in for a calc view) =="
# replicate_native reads via ADBC native SQL; a HANA VIEW created in-test stands in
# for a _SYS_BIC calc view (no BW on this box). Count + value parity + native->parquet.
run ZCL_ERPL_REV_BWTEST abap/zcl_erpl_rev_bwtest.abap
echo "$OUT" | grep -q 'BW RESULT pass=5 fail=0' || fail "bw test ($OUT)"
echo "   bw OK (ADBC read -> DuckDB: view + parameterized SQLScript table function, parity, parquet)"

kill "$SRV" 2>/dev/null; sleep 1
rm -f "$E2E_DB" "$E2E_DB".wal
if [ "$RFC_BACKEND" = proto ]; then
  echo "== SDK-absence check =="
  # Matched on the resolved *path*, not the SONAME: erpl-proto's shim is called
  # libsapnwrfc.so on purpose, so a name check cannot tell it from SAP's and
  # would fail on exactly the binary we want.
  if ldd ./build/erpl_rev_server | grep -F "$NW/"; then
    fail "a library is still being loaded from the SAP NW RFC SDK at $NW"
  fi
  if ldd ./build/erpl_rev_server | grep -Ei 'libsapucum|libicu'; then
    fail "libsapucum or ICU is still linked"
  fi
  echo "   nothing loaded from $NW; no libsapucum, no ICU"
  if [ "$RFC_LINK" = static ]; then
    # The shim is inside the binary, so there must be no RFC shared object at
    # all -- not even erpl-proto's own.
    if ldd ./build/erpl_rev_server | grep -i sapnwrfc; then
      fail "RFC_LINK=static but an RFC shared object is still loaded"
    fi
    echo "   RFC backend linked statically; no RFC shared object at all"
  fi
fi

echo "== E2E PASSED =="
