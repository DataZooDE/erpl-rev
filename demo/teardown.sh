#!/usr/bin/env bash
# Undo the demo: stop the daemon, drop the triggers, purge the document.
#
# The triggers are the part that matters. ZCDC_* object names derive from the
# SOURCE table, and ZDELTA_ALL is the busiest fixture in the test suite -- a
# leftover trigger set writing into a log table that no longer exists makes
# every insert on it dump, and that surfaces as an unrelated suite failing for
# no visible reason.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -z "${SAP_PASSWORD:-}" ] && [ -f .adt.creds ]; then
  SAP_PASSWORD=$(python3 -c "import json;print(json.load(open('.adt.creds'))['password'])")
  export SAP_PASSWORD
fi
ADT=(--host localhost --port 50000 --client 001 --user DEVELOPER --password-env SAP_PASSWORD)
export PATH="$PWD/demo/bin:$PATH"
erpl-rev daemon stop --yes >/dev/null 2>&1 || true
uvx erpl-adt "${ADT[@]}" object run ZCL_ERPL_REV_CLIDRV >/dev/null 2>&1 || true
uvx erpl-adt "${ADT[@]}" object run ZCL_ERPL_REV_DEMOTEARDOWN 2>&1 | grep DEMOTEARDOWN || true
pkill -f '[e]rpl_rev_server --db demo/demo.duckdb' 2>/dev/null || true
rm -f demo/demo.duckdb demo/demo.duckdb.wal demo/server.log
echo "TEARDOWN OK"
