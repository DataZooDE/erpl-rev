#!/usr/bin/env bash
# Everything that has to be true before the camera rolls.
#
# The rule this script exists to enforce: the tape may only run commands that
# complete on their own -- `erpl-rev sql`, `erpl-rev top`, `erpl-adt object run`.
# Every other CLI verb (sync create, sync run, daemon start/stop/status) is
# QUEUED into _erpl_rev_cli_cmd and only completes when ZCL_ERPL_REV_CLIDRV
# drains it. Put one of those in a tape and the recording hangs on an interval
# nobody controls. So they all happen here, each followed by an explicit drain.
#
#   bash demo/setup.sh        # idempotent; run it before every take
set -euo pipefail
cd "$(dirname "$0")/.."

DB=demo/demo.duckdb
BIN=./build/erpl_rev_server
TARGET=stock_moves
PORT=19494

# Credentials: .adt.creds is what erpl-adt reads, and it is gitignored. Nothing
# here echoes it, and SAP_PASSWORD never reaches a process list.
if [ -z "${SAP_PASSWORD:-}" ] && [ -f .adt.creds ]; then
  SAP_PASSWORD=$(python3 -c "import json;print(json.load(open('.adt.creds'))['password'])")
  export SAP_PASSWORD
fi
: "${SAP_PASSWORD:?set SAP_PASSWORD, or run erpl-adt login to write .adt.creds}"

ADT=(--host localhost --port 50000 --client 001 --user DEVELOPER --password-env SAP_PASSWORD)
adt()   { uvx erpl-adt "${ADT[@]}" "$@"; }
drain() { adt object run ZCL_ERPL_REV_CLIDRV >/dev/null 2>&1 || true; }

say() { printf '  %s\n' "$*"; }

# --- 0. the binary, under the name an operator would type -------------------
# There is no `erpl-rev` binary in a source checkout -- it is
# build/erpl_rev_server. A recording full of ./build/... reads as a dev hack
# rather than as the product, so the demo puts the real binary on PATH under
# its real name. It is a symlink, not a copy: it cannot go stale.
[ -x "$BIN" ] || { echo "build it first: make build"; exit 1; }
mkdir -p demo/bin
ln -sf "$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")" demo/bin/erpl-rev
export PATH="$PWD/demo/bin:$PATH"
export LD_LIBRARY_PATH="${ERPL_RFC_LIB_DIR:-$PWD/nwrfcsdk/linux/lib}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export ERPL_REV_GWHOST=localhost ERPL_REV_GWSERV=3300
export SAP_HOST=localhost SAP_PORT=50000 SAP_CLIENT=001 SAP_USER=DEVELOPER

# --- 1. retire a daemon still ticking against the OLD database --------------
# A background ERPL_REV_DAEMON job outlives the server process. Pointed at a
# fresh file it reads status=STOPPED, stop=false and a SECOND daemon starts
# beside it -- two daemons racing on one singleton row, which is exactly the
# failure the singleton exists to prevent.
say "stopping any running daemon"
if erpl-rev sql --quiet "SELECT 1" >/dev/null 2>&1; then
  erpl-rev daemon stop --yes >/dev/null 2>&1 || true
  drain
  for _ in $(seq 20); do
    st=$(erpl-rev sql --quiet --format csv \
         "SELECT status FROM _erpl_rev_daemon WHERE id=1" 2>/dev/null | tail -1 || true)
    [ "$st" = "STOPPED" ] && break
    sleep 1
  done
fi
pkill -f "[e]rpl_rev_server --db $DB" 2>/dev/null || true
sleep 2

# --- 2. a FRESH database ----------------------------------------------------
# Deleted, not emptied. The demo's whole point is a physical delete, and a
# replicated row only leaves the target because the engine put a delete
# through -- so reusing a file would let a previous take's rows masquerade as
# this take's, and the opening "nothing there yet" beat would be a lie.
say "starting the server on a fresh database"
rm -f "$DB" "$DB".wal
"$BIN" --db "$DB" --metrics-port "$PORT" >demo/server.log 2>&1 &
sleep 6
erpl-rev sql --quiet "SELECT 1" >/dev/null 2>&1 \
  || { echo "server did not come up:"; tail -20 demo/server.log; exit 1; }

# --- 2a. retire daemons that outlived the OLD database ----------------------
# Step 1 only reaches a daemon while a server is up. If the server was already
# gone -- killed by hand, crashed, or stopped so the test suite could have the
# port -- the ABAP background jobs are still running, and a FRESH database
# looks to each of them like a daemon nobody has asked to stop. Two or three
# then tick against the same singleton row and drive the same cycle at once,
# which surfaces as "TransactionContext Error: Conflict on update" and a target
# left in ERROR: a recording of a product that does not work.
#
# Cost of getting this wrong is a whole take, and it is not visible until the
# closing query comes up empty, so the stop is asserted here rather than hoped
# for: set the flag, wait until the heartbeat stops moving, then clear it.
say "retiring any daemon left over from a previous take"
erpl-rev sql --quiet "UPDATE _erpl_rev_daemon SET stop=true, status='STOPPED' WHERE id=1" \
  >/dev/null 2>&1 || true

# HELD for several ticks before the first check. The previous version compared
# consecutive readings and stopped as soon as two matched -- which they did
# immediately, because a failed read returns an EMPTY string and two empty
# strings are equal. It cleared the flag inside four seconds, before any daemon
# had polled, and three of them sailed through into the recording. A reading
# that is not a number is now not a reading.
sleep 8
prev=""; still=0
for _ in $(seq 40); do
  cur=$(erpl-rev sql --quiet --format csv \
        "SELECT coalesce(ticks,0) FROM _erpl_rev_daemon WHERE id=1" 2>/dev/null | tail -1)
  case "$cur" in
    ''|*[!0-9]*) still=0 ;;                                  # no answer is not agreement
    *) if [ "$cur" = "$prev" ]; then still=$((still + 1)); else still=0; fi ;;
  esac
  [ "$still" -ge 3 ] && break
  prev="$cur"
  sleep 1
done
[ "$still" -ge 3 ] || { echo "a daemon is still ticking after 48s; stop it before recording"; exit 1; }
erpl-rev sql --quiet "UPDATE _erpl_rev_daemon SET stop=false, ticks=0 WHERE id=1" \
  >/dev/null 2>&1 || true

# --- 3. the demo ABAP, into $TMP -------------------------------------------
# $TMP keeps them out of the delivered package, so E-FOOTPRINT is unaffected,
# and demo/abap/ is outside the tree compliance-scan.sh walks.
say "deploying the demo classes"
for c in goods_movement stock_workload demosetup demoteardown; do
  # Named for the business action, not for the framework.
  case "$c" in
    goods_movement) N=ZCL_GOODS_MOVEMENT; F=demo/abap/zcl_goods_movement.abap ;;
    stock_workload) N=ZCL_STOCK_WORKLOAD; F=demo/abap/zcl_stock_workload.abap ;;
    *)              N="ZCL_ERPL_REV_$(echo "$c" | tr '[:lower:]' '[:upper:]')"
                    F="demo/abap/zcl_erpl_rev_$c.abap" ;;
  esac
  adt object create --type CLAS/OC --name "$N" --package '$TMP' \
      --description "erpl-rev demo" >/dev/null 2>&1 || true
  adt source write "$N" --file "$F" --activate 2>&1 \
    | grep -qiE "activated|nothing to activate" \
    || { echo "could not activate $N"; exit 1; }
done

# --- 4. purge, register, provision the triggers -----------------------------
say "registering $TARGET on the trigger tier (source keeps its million movements)"
OUT=$(adt object run ZCL_ERPL_REV_DEMOSETUP 2>&1)
grep -q "DEMOSETUP OK" <<<"$OUT" || { echo "$OUT"; exit 1; }

# --- 4a. the clock gate -----------------------------------------------------
# The engine reads a source timestamp AT TIME ZONE 'UTC' with no skew
# correction. If SAP's clock and this one disagree, every latency in the
# closing shot is wrong by exactly that offset -- and looks entirely plausible.
# Refuse to record rather than publish a fabricated number.
SAP_TS=$(sed -n 's/.*sap_utc=\([0-9]\{14\}\).*/\1/p' <<<"$OUT")
if [ -n "$SAP_TS" ]; then
  sap_epoch=$(date -u -d "${SAP_TS:0:8} ${SAP_TS:8:2}:${SAP_TS:10:2}:${SAP_TS:12:2}" +%s)
  skew=$(( sap_epoch - $(date -u +%s) ))
  [ "${skew#-}" -le 3 ] \
    || { echo "SAP clock is ${skew}s from this one; the latency shot would be fiction"; exit 1; }
  say "clock skew ${skew}s"
fi

# --- 5. the daemon ----------------------------------------------------------
# Queued, drained, and then POLLED: CLIDRV only SUBMITs the job, and a work
# process still has to pick it up.
say "starting the daemon at a 2s tick"
# Where the log stands BEFORE ours starts, so the count below sees only
# heartbeats written after it -- reading the whole tail counted the daemons
# that had already been stopped and failed a setup that had worked.
LOG_MARK=$(wc -l < demo/server.log)
erpl-rev daemon start --tick 2 --workers 2 --yes >/dev/null 2>&1 || true
drain
for _ in $(seq 40); do
  st=$(erpl-rev sql --quiet --format csv \
       "SELECT status FROM _erpl_rev_daemon WHERE id=1" 2>/dev/null | tail -1 || true)
  [ "$st" = "RUNNING" ] && break
  sleep 1
done
[ "${st:-}" = "RUNNING" ] || { echo "the daemon never came up"; exit 1; }

# And exactly ONE of them. Two daemons driving the same target run the same
# cycle concurrently, DuckDB rejects the second with "TransactionContext Error:
# Conflict on update", and the target lands in ERROR -- which the monitor shows
# as a target that has simply never run. That cost a complete take: the first
# sign of it was the closing latency query returning no rows, two minutes in.
#
# The singleton row cannot answer this, because each daemon overwrites the same
# instance_id. The server's own log can: every heartbeat carries the id that
# wrote it.
sleep 6
DAEMONS=$(tail -n +$((LOG_MARK + 1)) demo/server.log \
          | grep -o "instance_id='[^']*'" | sort -u | wc -l)
[ "$DAEMONS" = "1" ] || { echo "$DAEMONS daemons are ticking, expected 1 -- a previous one survived"; exit 1; }

# --- 6. assert the opening frame -------------------------------------------
# The tape's first beat says "nothing there yet". If that is not true, stop.
rows=$(erpl-rev sql --quiet --format csv "SELECT count(*) FROM $TARGET" 2>/dev/null | tail -1 || echo x)
[ "$rows" = "0" ] || { echo "$TARGET holds '$rows' rows, expected 0"; exit 1; }

echo "SETUP OK -- $TARGET on the trigger tier, daemon RUNNING, target empty"
