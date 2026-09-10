#!/usr/bin/env bash
# Print the SAP password for the recording, and nothing else.
#
# A separate script because vhs's tape parser cannot nest quotes, and reading a
# JSON field inline needs several layers of them. It contains no secret itself:
# it reads .adt.creds, which is gitignored and 0600, and is written by
# `erpl-adt login`.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -n "${SAP_PASSWORD:-}" ]; then printf '%s' "$SAP_PASSWORD"; exit 0; fi
python3 -c 'import json;print(json.load(open(".adt.creds"))["password"],end="")'
