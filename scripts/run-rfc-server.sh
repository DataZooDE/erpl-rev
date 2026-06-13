#!/usr/bin/env bash
# Start the erpl-rev RFC server, optionally with MotherDuck and/or BigQuery
# attached (both opt-in via the environment — see below).
#
# Both cloud catalogs are OPT-IN via the environment, so this script carries no
# secrets or site-specific values — put those in your shell profile (~/.bashrc):
#
#   # MotherDuck: attaches `md:` when this is set
#   export motherduck_token="<your-motherduck-token>"
#
#   # BigQuery: attaches when a project is set (auth = Google ADC)
#   export ERPL_REV_BQ_PROJECT="<your-gcp-project>"
#   export ERPL_REV_BQ_DATASET="<dataset>"      # optional; blank = whole project
#
# BigQuery uses Application Default Credentials — set them up once:
#   gcloud auth application-default login
#   gcloud auth application-default set-quota-project <your-gcp-project>
#
# Prerequisite: the server is built ->  cmake --build build
#
# Usage:
#   scripts/run-rfc-server.sh       # start (refuses if one is already running)
#   scripts/run-rfc-server.sh -r    # restart: stop any running server, then start
#
# Other overrides (env):
#   ERPL_REV_PROGRAM_ID   gateway PROGRAM_ID      (default ERPL_REVERSE)
#   ERPL_REV_GWHOST       SAP gateway host        (default localhost)
#   ERPL_REV_GWSERV       SAP gateway service     (default 3300)
#   ERPL_REV_DB_PATH      DuckDB file             (default /tmp/erpl-rev.duckdb)
#   ERPL_REV_BQ_READONLY  set to 1 to attach BigQuery read-only (default: read/write)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

# --- config (env overrides, else these defaults) ----------------------------
# PROGRAM_ID must match the ABAP registered destination (zcl_erpl_rev_setup
# registers dest ERPL_REV with program=ERPL_REV) and the server's own default
# (main.cpp / README). Anything else => CM_ALLOCATE_FAILURE_RETRY on CALL.
: "${ERPL_REV_PROGRAM_ID:=ERPL_REV}"
: "${ERPL_REV_GWHOST:=localhost}"
: "${ERPL_REV_GWSERV:=3300}"
: "${ERPL_REV_DB_PATH:=/tmp/erpl-rev.duckdb}"
: "${ERPL_REV_BQ_PROJECT:=}"     # blank => BigQuery not attached
: "${ERPL_REV_BQ_DATASET:=}"     # blank => attach the whole project

SERVER="$HERE/build/erpl_rev_server"
[[ -x "$SERVER" ]] || { echo "error: $SERVER not built — run 'cmake --build build'" >&2; exit 1; }

# --- restart handling -------------------------------------------------------
if pgrep -x erpl_rev_server >/dev/null; then
  if [[ "${1:-}" == "-r" || "${1:-}" == "--restart" ]]; then
    echo "stopping running erpl_rev_server ..."
    pkill -x erpl_rev_server || true
    for _ in 1 2 3 4 5 6; do pgrep -x erpl_rev_server >/dev/null || break; sleep 1; done
    pgrep -x erpl_rev_server >/dev/null && { pkill -9 -x erpl_rev_server || true; sleep 1; }
  else
    echo "erpl_rev_server is already running. Pass -r/--restart to replace it." >&2
    exit 1
  fi
fi

# --- build the boot init SQL (MotherDuck + BigQuery, both opt-in) ------------
init=""
if [[ -n "${motherduck_token:-}" ]]; then
  init+="INSTALL motherduck; LOAD motherduck; ATTACH 'md:'; "
else
  echo "note: motherduck_token not set -> MotherDuck not attached" >&2
fi
if [[ -n "$ERPL_REV_BQ_PROJECT" ]]; then
  ro=""; [[ -n "${ERPL_REV_BQ_READONLY:-}" ]] && ro=", READ_ONLY"
  conn="project=$ERPL_REV_BQ_PROJECT"
  [[ -n "$ERPL_REV_BQ_DATASET" ]] && conn+=" dataset=$ERPL_REV_BQ_DATASET"
  init+="INSTALL bigquery FROM community; LOAD bigquery; ATTACH '$conn' AS bq (TYPE bigquery$ro); "
else
  echo "note: ERPL_REV_BQ_PROJECT not set -> BigQuery not attached" >&2
fi

# --- run --------------------------------------------------------------------
export LD_LIBRARY_PATH="$HERE/nwrfcsdk/linux/lib:$HERE/vendor/duckdb-1.5.3${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export ERPL_REV_PROGRAM_ID ERPL_REV_GWHOST ERPL_REV_GWSERV

echo "erpl-rev: program_id=$ERPL_REV_PROGRAM_ID gw=$ERPL_REV_GWHOST:$ERPL_REV_GWSERV db=$ERPL_REV_DB_PATH"
echo "         motherduck=$([[ -n "${motherduck_token:-}" ]] && echo on || echo off) \
bigquery=${ERPL_REV_BQ_PROJECT:-off}${ERPL_REV_BQ_DATASET:+.$ERPL_REV_BQ_DATASET} \
$([[ -n "$ERPL_REV_BQ_PROJECT" ]] && { [[ -n "${ERPL_REV_BQ_READONLY:-}" ]] && echo '(read-only)' || echo '(read/write)'; })"
exec "$SERVER" --db "$ERPL_REV_DB_PATH" --init-sql "$init"
