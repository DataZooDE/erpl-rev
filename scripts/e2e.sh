#!/bin/bash
# End-to-end test for erpl-rev against the live A4H docker system.
#
# Assumes one-time setup (see README / docs/enable-rfc-registration.md):
#   - A4H up with gw/acl_mode=0                 (scripts/start-sap.sh)
#   - destination ERPL_REV exists method='R' (deploy+run zcl_erpl_rev_setup)
#   - FMs Z_DUCKDB_QUERY / Z_DUCKDB_INGEST exist (deploy+run zcl_erpl_rev_mkfm)
#
# Builds, starts the server, deploys+runs the caller classruns, asserts results.
#
# REMOTE SERVER (proving a platform this box is not, issue #87)
# ------------------------------------------------------------
# The server does not have to run here. It registers at the SAP gateway over the
# network, so it can sit on a Mac or a Windows box while SAP and this script stay
# put -- which is the only way a Linux workstation can prove the registered-server
# path on another platform, and the gap CI cannot close.
#
#   ERPL_REV_E2E_REMOTE=mac \
#   ERPL_REV_E2E_REMOTE_BIN='~/erpl-rev/erpl-rev-macos-arm64' \
#   ERPL_REV_E2E_GWHOST=100.119.230.107 \
#   SAP_PASSWORD=... scripts/e2e.sh
#
#   ERPL_REV_E2E_REMOTE      ssh target the server runs on. Unset = this machine.
#   ERPL_REV_E2E_REMOTE_BIN  path to the erpl-rev binary THERE (already installed;
#                            this script cannot cross-compile one for it).
#   ERPL_REV_E2E_GWHOST      gateway address as the REMOTE host reaches it. The
#                            default `localhost` is this box, so remote runs must
#                            set it. Ports 3300 (gateway) and 50000 (ADT) have to
#                            be reachable from there.
#
# Remote mode skips the local build and the unit suite (they say nothing about the
# other platform's binary) and runs `--smoke` there instead; the ABAP stages are
# unchanged, because they are driven through SAP either way.
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

cd "$HERE"

# --- where the server runs -------------------------------------------------
REMOTE="${ERPL_REV_E2E_REMOTE:-}"
REMOTE_BIN="${ERPL_REV_E2E_REMOTE_BIN:-}"
GWHOST="${ERPL_REV_E2E_GWHOST:-localhost}"
LOCAL_BIN=./build/erpl_rev_server
SRV=""            # local pid, or the remote pid when REMOTE is set

if [ -n "$REMOTE" ]; then
  [ -n "$REMOTE_BIN" ] || { echo "set ERPL_REV_E2E_REMOTE_BIN too" >&2; exit 2; }
  [ "$GWHOST" = localhost ] && { echo "set ERPL_REV_E2E_GWHOST: 'localhost' is this box, not $REMOTE" >&2; exit 2; }
fi

# Run a command where the server is. The command goes over ssh on STDIN rather
# than in argv, so SAP_PASSWORD -- which the CLI stages need on the server's host
# -- never appears in the remote machine's process list.
on_server() {
  if [ -n "$REMOTE" ]; then
    printf '%s%s\n' "$REMOTE_ENV" "$(printf '%q ' "$@")" | ssh "$REMOTE" bash -s
  else
    "$@"
  fi
}
# What the CLI needs to reach SAP from the server's host. Locally it is already
# in this shell's environment; remotely it has to travel.
REMOTE_ENV=""
[ -n "$REMOTE" ] && REMOTE_ENV="export SAP_HOST=$GWHOST SAP_PORT=50000 \
SAP_CLIENT=001 SAP_USER=DEVELOPER SAP_PASSWORD=$(printf '%q' "$SAP_PASSWORD"); "
# The same connection details for a LOCAL run. They used to be set only in the
# remote case, so the CLI stages silently depended on the operator happening to
# have a saved erpl-rev config -- and failed with an empty --user on any machine
# that did not. The ABAP stages never noticed, because they go through erpl-adt.
if [ -z "$REMOTE" ]; then
  export SAP_HOST="${SAP_HOST:-localhost}" SAP_PORT="${SAP_PORT:-50000}" \
         SAP_CLIENT="${SAP_CLIENT:-001}" SAP_USER="${SAP_USER:-DEVELOPER}"
fi
# The erpl-rev CLI, wherever the server is. `sql`/`sync`/`replicate` reach the
# server through its loopback quack listener, so they must run on its host.
cli() {
  if [ -n "$REMOTE" ]; then on_server "$REMOTE_BIN" "$@"; else "$LOCAL_BIN" "$@"; fi
}
srv_kill() {
  [ -n "$SRV" ] || return 0
  if [ -n "$REMOTE" ]; then ssh "$REMOTE" "kill $SRV" 2>/dev/null
  else kill "$SRV" 2>/dev/null; fi
}
fail() { echo "E2E FAIL: $*" >&2; srv_kill; exit 1; }

# Static gates first: they need no SAP, no build and no credentials, so a
# violation should cost a second rather than a full e2e run.
echo "== compliance (static) =="
"$HERE/scripts/compliance-scan.sh" || fail "compliance scan"

echo "== build =="
if [ -n "$REMOTE" ]; then
  # Nothing here can build or test the other platform's binary; that is CI's job
  # and the reason this mode exists. Check the one thing that is checkable from
  # here: the binary is present, runs, and its RFC backend and DuckDB load.
  on_server "$REMOTE_BIN" --smoke || fail "--smoke on $REMOTE"
  echo "   remote binary smoke OK on $REMOTE (build + unit tests are CI's, not ours)"
else
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
fi

echo "== start server =="
on_server rm -f /tmp/erpl_rev_sap_export.parquet
# The taxi fixture is read by DuckDB, so it has to exist on the SERVER's disk,
# not this one. Ship it there every run rather than assume: it is 654 bytes, and
# a stale copy is a confusing failure three stages later.
if [ -n "$REMOTE" ]; then
  scp -q "$HERE/data/taxi.parquet" "$REMOTE:/tmp/erpl_taxi.parquet" \
    || fail "copy the taxi fixture to $REMOTE"
else
  cp "$HERE/data/taxi.parquet" /tmp/erpl_taxi.parquet || fail "stage the taxi fixture"
fi
# Isolate the DB: the server is now file-backed by default, so give e2e a fresh
# throwaway file (removed around the run) — never the persistent default.
E2E_DB="/tmp/erpl_e2e_$$.duckdb"
on_server rm -f "$E2E_DB" "$E2E_DB".wal
if [ -n "$REMOTE" ]; then
  # Detached, and its pid comes back so srv_kill can reach it. `ssh -n` keeps the
  # remote process off this script's stdin.
  SRV=$(ssh -n "$REMOTE" "ERPL_REV_GWHOST=$GWHOST ERPL_REV_GWSERV=3300 \
        nohup $(printf '%q' "$REMOTE_BIN") --db $E2E_DB \
        > /tmp/erpl_e2e_srv.log 2>&1 & echo \$!") \
    || fail "start the server on $REMOTE"
  sleep 8
  ssh "$REMOTE" "kill -0 $SRV" 2>/dev/null \
    || { ssh "$REMOTE" "cat /tmp/erpl_e2e_srv.log"; SRV=""; fail "server died on $REMOTE"; }
  echo "   server up on $REMOTE (pid $SRV), registered at gateway $GWHOST:3300"
else
  LD_LIBRARY_PATH="$RFC_LIB_DIR" "$LOCAL_BIN" --db "$E2E_DB" >/tmp/erpl_e2e_srv.log 2>&1 &
  SRV=$!
  sleep 6
  ps -p "$SRV" >/dev/null || { cat /tmp/erpl_e2e_srv.log; fail "server died"; }
fi


# --- suite runner ------------------------------------------------------------
# Every ABAP suite is one line in the table below: NAME|CLASS|file|marker|tag.
# Selection is by environment, not by flags, because this script deliberately
# refuses unknown flags:
#
#   ERPL_REV_E2E_ONLY='delta|cdc'   run only suites whose NAME or tag matches
#   ERPL_REV_E2E_SKIP='soak|perf'   skip suites whose NAME or tag matches
#
# The marker convention is `<NAME> RESULT pass=<n> fail=0`. The assertion is
# ALWAYS fail=0 plus an optional minimum pass count -- never an exact count.
# Eight suites used to assert exact totals, so every added assertion inside a
# suite meant hand-bumping a number here; those are the most rubber-stamped
# lines in the file and the first to be "fixed" by making them match.
ONLY="${ERPL_REV_E2E_ONLY:-}"
# The default skip keeps the long lanes out of an ordinary run. But asking for a
# suite BY NAME and being told "skipped" is a gate that silently runs nothing --
# so an explicit ONLY beats the DEFAULT skip. An explicit SKIP still wins, which
# is how a soak lane excludes one suite from the set it asked for.
if [ -n "$ONLY" ]; then SKIP="${ERPL_REV_E2E_SKIP:-}"
else SKIP="${ERPL_REV_E2E_SKIP:-@soak|@perf}"; fi

suite() {  # NAME CLASS file marker-prefix min-pass tag description
  local name="$1" cls="$2" file="$3" marker="$4" minpass="${5:-1}" tag="${6:-}" desc="${7:-}"
  if [ -n "$ONLY" ] && ! printf '%s %s' "$name" "$tag" | grep -qiE "$ONLY"; then return 0; fi
  if [ -n "$SKIP" ] &&   printf '%s %s' "$name" "$tag" | grep -qiE "$SKIP";  then
    echo "== $name (skipped) =="; return 0
  fi
  echo "== $name =="
  [ -n "$desc" ] && echo "   $desc"
  run "$cls" "$file"
  echo "$OUT" | grep -qE "$marker RESULT pass=[0-9]+ fail=0" \
    || fail "$name ($OUT)"
  local got
  got=$(echo "$OUT" | grep -oE "$marker RESULT pass=[0-9]+" | grep -oE '[0-9]+$' | head -1)
  if [ -n "$got" ] && [ "$got" -lt "$minpass" ]; then
    fail "$name ran only $got assertions, expected at least $minpass"
  fi
  echo "   $name OK ($got assertions)"
}

run() {  # cls file -> $OUT (cleaned). Uses ABSOLUTE --file path.
  local cls="$1" file="$HERE/$2"
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
on_server test -f /tmp/erpl_rev_sap_export.parquet || fail "parquet not written"
echo "   ingest OK (parquet written, upsert verified)"

suite console ZCL_ERPL_REV_CONSOLETEST abap/zcl_erpl_rev_consoletest.abap CONSOLE 9 "" \
  "arbitrary SQL to arbitrary result column names, without a dump"


suite slt ZCL_ERPL_REV_SLTTEST abap/zcl_erpl_rev_slttest.abap SLT 37 "" \
  "projection, source filter, key retention, error safety"


suite diff ZCL_ERPL_REV_DIFFTEST abap/zcl_erpl_rev_difftest.abap DIFF 4 "" \
  "every replicated cell equals the SAP source; corruption is detected"


suite delta ZCL_ERPL_REV_DELTATEST abap/zcl_erpl_rev_deltatest.abap DELTA 1 "" \
  "watermark, snapshot delete, real change documents, orchestration"


# Trigger-CDC (opt-in physical-delete tier, ADR-0004): provisions REAL HANA triggers
# on the source via the server-generated DDL, physically deletes rows, and proves one
# CDC cycle reflects the deletes in the DuckDB target; idempotent re-run; teardown
# leaves no orphan objects. Needs ZCL_ERPL_REV_CDC[TEST] + the CDC FMs (mkfm).
suite cdc ZCL_ERPL_REV_CDCTEST abap/zcl_erpl_rev_cdctest.abap CDC 1 "" \
  "real HANA triggers capture physical deletes; teardown leaves nothing"


suite partition ZCL_ERPL_REV_PARTEST abap/zcl_erpl_rev_partest.abap PARTITION 9 "" \
  "disjoint workers into one heap, PK built once"


suite replrun ZCL_ERPL_REV_REPLRUN abap/zcl_erpl_rev_replrun.abap REPLRUN 6 "" \
  "the end-user report's parallel branch, with real background jobs"


suite publish ZCL_ERPL_REV_PUBTEST abap/zcl_erpl_rev_pubtest.abap PUBTEST 6 "" \
  "parquet file and dataset, attached catalog full and append"


suite watermark ZCL_ERPL_REV_WMTEST abap/zcl_erpl_rev_wmtest.abap WM 8 "" \
  "the corrections: a late commit below the observed max is delivered, DATE never reads today, a DATS+TIMS pair survives midnight, load types I and F"

suite daemon ZCL_ERPL_REV_DAEMONTEST abap/zcl_erpl_rev_daemontest.abap DAEMON 16 "@soak" \
  "the daemon as a real background job: it ticks, it replicates with nobody calling run(), a second one refuses to start, and the stop flag ends it"

suite stress ZCL_ERPL_REV_STREAMSTRESS abap/zcl_erpl_rev_streamstress.abap STRESS 14 "@soak" \
  "a real change workload, then the two anti-joins: nothing lost, nothing invented, nothing stale"

suite cds ZCL_ERPL_REV_CDSTEST abap/zcl_erpl_rev_cdstest.abap CDS 8 "" \
  "CDS view entity as a source: keys, parity, parameters"


suite bw ZCL_ERPL_REV_BWTEST abap/zcl_erpl_rev_bwtest.abap BW 5 "" \
  "replicate_native over ADBC: a HANA view stands in for a calc view"

# ---------------------------------------------------------------------------
echo "== CLI E2E (operate the server from the shell) =="
# The commands that replace the SAP GUI reports. The server started above is
# still running with quack on the default loopback listener, so the CLI should
# find it through the runtime state file with no flags at all.
adt() { uvx erpl-adt "${ADT[@]}" "$@"; }
# Run the CLI with every SAP credential removed, wherever the server is. Step 7
# turns on this, and unsetting them in THIS shell says nothing about the remote
# one -- the whole point is a caller who cannot log into SAP at all.
cli_nocreds() {
  if [ -n "$REMOTE" ]; then
    # Append rather than replace, so this strips exactly what `env -u` strips
    # below: the credentials, not SAP_HOST/PORT/CLIENT. Removing those too would
    # make the command fail for want of an address and look like it passed.
    REMOTE_ENV="${REMOTE_ENV}unset SAP_USER SAP_PASSWORD ERPL_REV_SAP_PASSWORD; " cli "$@"
  else
    env -u SAP_USER -u SAP_PASSWORD -u ERPL_REV_SAP_PASSWORD "$LOCAL_BIN" "$@"
  fi
}

# The state file lives next to the server, so it is checked next to the server.
on_server sh -c 'S="${XDG_RUNTIME_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}}/erpl-rev/server.json"
  [ -f "$S" ] || { echo "no server state file at $S" >&2; exit 1; }
  M=$(stat -c %a "$S" 2>/dev/null || stat -f %Lp "$S")
  [ "$M" = 600 ] || { echo "state file is $M, not 0600 -- it holds the quack token" >&2; exit 1; }' \
  || fail "server runtime state file"

# 1. Zero-config: no flags, finds the running server.
OUT="$(cli sql "SELECT 42 AS answer" 2>&1)" || fail "sql failed: $OUT"
grep -q "using the running server" <<<"$OUT" || fail "sql did not use quack: $OUT"
grep -q "42" <<<"$OUT" || fail "sql returned no answer: $OUT"

# 2. Formats.
[ "$(cli sql "SELECT 1 AS n" --format csv 2>/dev/null)" = "n
1" ] || fail "csv output wrong"
cli sql "SELECT 1 AS n" --format json 2>/dev/null | grep -q '"rows":\[{"n":1}\]' \
  || fail "json output wrong"

# 3. --print-abap contacts nothing and produces activatable ABAP. This is the
#    only compile check the generated skeletons ever get.
GEN="/tmp/erpl_cli_gen_$$.abap"
cli replicate --table T000 --target t000_cli --print-abap > "$GEN" 2>/dev/null \
  || fail "--print-abap failed"
grep -q "SUBMIT z_erpl_rev_replicate" "$GEN" || fail "generated ABAP has no SUBMIT"
# Deploy it under a stable name purely to prove it compiles.
adt object create --type CLAS/OC --name ZCL_ERPL_CLI_SYNTAX --package '$TMP' \
     --description 'erpl-rev CLI syntax probe' >/dev/null 2>&1 || true
sed -e 's/ZCL_ERPL_REV_CLI_R[0-9a-f]*/ZCL_ERPL_CLI_SYNTAX/g' "$GEN" > "$GEN.named"
adt source write ZCL_ERPL_CLI_SYNTAX --file "$GEN.named" --activate 2>&1 \
  | grep -qE "Activated|Nothing to activate" \
  || fail "the generated replicate ABAP does not compile"
adt object delete /sap/bc/adt/oo/classes/zcl_erpl_cli_syntax >/dev/null 2>&1 || true
rm -f "$GEN" "$GEN.named"
echo "   generated ABAP compiles"

# 4. Injection: each payload must become a literal, never a statement.
for BAD in "MANDT = '000' OR '1'='1'" "x' ). DELETE FROM t000. \"" 'a` ). zcl_evil=>go( ). `'; do
  P="$(cli replicate --table T000 --where "$BAD" --print-abap 2>&1)" \
    || fail "print-abap refused a legitimate payload: $BAD"
  grep -q "DELETE FROM" <<<"$(grep -v p_where <<<"$P")" \
    && fail "payload escaped its literal: $BAD"
done
# A newline cannot be escaped into an ABAP literal, so it must be refused.
cli replicate --table T000 --where "$(printf 'a\nENDMETHOD.')" --print-abap >/dev/null 2>&1
[ $? -eq 2 ] || fail "a newline in --where was not refused with exit 2"
echo "   injection payloads render as literals; a newline is refused"

# 5. A real load, submitted as a background job and verified from run stats.
cli replicate --table T000 --target t000_cli --yes --wait 180 --quiet >/dev/null 2>&1 \
  || fail "replicate did not report success"
CNT="$(cli sql "SELECT count(*) AS n FROM t000_cli" --format csv --quiet 2>/dev/null | tail -1)"
[ "${CNT:-0}" -gt 0 ] || fail "replicate produced no rows"
echo "   replicate -> $CNT rows via a background job"

# 6. sync: register through ABAP, list from DuckDB.
cli sync create t000_cli --method SNAPSHOT --source T000 --keys MANDT \
     --cadence nightly --yes >/dev/null 2>&1 || fail "sync create failed"
cli sync ls --quiet 2>/dev/null | grep -q t000_cli || fail "sync ls does not show the job"
# The granularity gate lives in ABAP; the CLI must not be able to bypass it.
if cli sync create t000_bad --method WATERMARK --source T000 --keys MANDT \
        --chg-col X --wm-kind DATE --cadence micro:30 --yes >/dev/null 2>&1; then
  fail "the ABAP granularity gate was bypassed"
fi
cli sync run t000_cli --yes >/dev/null 2>&1 || fail "sync run failed"
echo "   sync create/ls/run, and the granularity gate still applies"

# 7. The driver: parameters as data, no generated ABAP, no authorisation.
#    Deploy it first -- setup ships it, but this suite deploys via deploy-abap.sh.
adt object create --type CLAS/OC --name ZCL_ERPL_REV_CLIDRV --package '$TMP' \
    --description 'erpl-rev CLI command driver' >/dev/null 2>&1 || true
adt source write ZCL_ERPL_REV_CLIDRV --file "$HERE/abap/zcl_erpl_rev_clidrv.abap" --activate \
  2>&1 | grep -qE "Activated|Nothing to activate" || fail "clidrv did not activate"

# --queue-only must not touch SAP: run it with the credentials stripped.
QOUT="$(cli_nocreds replicate --table T000 --target t000_q --queue-only --yes 2>&1)"
grep -q "Queued as command" <<<"$QOUT" || fail "--queue-only did not queue: $QOUT"
CID="$(sed -n 's/.*Queued as command \([0-9]*\).*/\1/p' <<<"$QOUT" | head -1)"
[ -n "$CID" ] || fail "no command id in: $QOUT"

# The heartbeat drains it -- the path that needs no SAP rights from the CLI.
cat > /tmp/erpl_e2e_tick_$$.abap <<'ABAPTICK'
CLASS zcl_erpl_e2e_tick DEFINITION PUBLIC FINAL CREATE PUBLIC.
  PUBLIC SECTION.
    INTERFACES if_oo_adt_classrun.
ENDCLASS.
CLASS zcl_erpl_e2e_tick IMPLEMENTATION.
  METHOD if_oo_adt_classrun~main.
    " The report writes a list. A classrun has nowhere to put one, so it has to
    " be captured to memory or the SUBMIT dumps.
    SUBMIT z_erpl_rev_delta WITH p_once = abap_true
      EXPORTING LIST TO MEMORY AND RETURN.
    DATA lt TYPE STANDARD TABLE OF abaplist.
    CALL FUNCTION 'LIST_FROM_MEMORY' TABLES listobject = lt EXCEPTIONS OTHERS = 1.
    CALL FUNCTION 'LIST_FREE_MEMORY' EXCEPTIONS OTHERS = 0.
    out->write( 'tick' ).
  ENDMETHOD.
ENDCLASS.
ABAPTICK
adt object create --type CLAS/OC --name ZCL_ERPL_E2E_TICK --package '$TMP' \
    --description 'e2e heartbeat tick' >/dev/null 2>&1 || true
adt source write ZCL_ERPL_E2E_TICK --file "/tmp/erpl_e2e_tick_$$.abap" --activate >/dev/null 2>&1
adt object run ZCL_ERPL_E2E_TICK >/dev/null 2>&1 || fail "heartbeat tick failed"
rm -f "/tmp/erpl_e2e_tick_$$.abap"

ST="$(cli sql "SELECT status FROM _erpl_rev_cli_cmd WHERE cmd_id = $CID" \
      --format csv --quiet 2>/dev/null | tail -1)"
[ "$ST" = "DONE" ] || fail "queued command $CID is '$ST', not DONE"
QROWS="$(cli sql "SELECT count(*) AS n FROM t000_q" --format csv --quiet 2>/dev/null | tail -1)"
[ "${QROWS:-0}" -gt 0 ] || fail "the queued replicate loaded no rows"
adt object delete /sap/bc/adt/oo/classes/zcl_erpl_e2e_tick >/dev/null 2>&1 || true
echo "   driver: queued with no SAP credentials, heartbeat ran it, $QROWS rows"

# 8. Nothing may be left behind in the customer's system.
# The trailing underscore matters: ZCL_ERPL_REV_CLIDRV is the permanent driver,
# ZCL_ERPL_REV_CLI_<kind><nonce> are the throwaways that must never survive.
LEFT="$(adt search 'ZCL_ERPL_REV_CLI_*' 2>/dev/null | grep -c 'ZCL_ERPL_REV_CLI_' || true)"
[ "$LEFT" -eq 0 ] || fail "$LEFT temporary CLI class(es) leaked into SAP"
echo "   no temporary classes left behind"

srv_kill; sleep 1
on_server rm -f "$E2E_DB" "$E2E_DB".wal /tmp/erpl_taxi.parquet
# ldd and the SDK path are this box's; a remote binary is a different platform's
# and was linked by CI, which has its own check.
if [ "$RFC_BACKEND" = proto ] && [ -z "$REMOTE" ]; then
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

suite footprint ZCL_ERPL_REV_FOOTPRINT abap/zcl_erpl_rev_footprint.abap FOOTPRINT 14 "" \
  "the delivered package contains exactly the documented objects, and nothing else"

echo "== E2E PASSED =="
