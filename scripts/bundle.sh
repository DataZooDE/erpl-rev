#!/usr/bin/env bash
# Assemble the single-file distributable for POSIX platforms (linux | osx):
#   dist/erpl-rev = launcher  +  payload.tar(inner server + runtime libs)  +  footer
# footer = "ERPLREV\x01" + little-endian uint64 payload size (read by the launcher).
#
# Usage: scripts/bundle.sh <linux|osx> <server-bin> <launcher-bin> <nwrfc-lib-dir> <duckdb-dir> <out>
set -euo pipefail

PLATFORM="${1:?platform: linux|osx}"
SERVER="${2:?server binary}"
LAUNCHER="${3:?launcher binary}"
SDK_LIB="${4:?nwrfcsdk <plat>/lib dir}"
DUCKDB_DIR="${5:?duckdb dist dir}"
OUT="${6:?output path}"

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
PAY="$STAGE/payload"

# The runtime payload (inner server + SAP/ICU/DuckDB libs) is staged by the
# shared helper so the bundle and the Docker image use the identical lib set.
"$(dirname "$0")/stage_runtime.sh" "$PLATFORM" "$SERVER" "$SDK_LIB" "$DUCKDB_DIR" "$PAY"

( cd "$PAY" && tar -cf "$STAGE/payload.tar" * )
SIZE="$(wc -c < "$STAGE/payload.tar")"

mkdir -p "$(dirname "$OUT")"
cat "$LAUNCHER" "$STAGE/payload.tar" > "$OUT"
perl -e 'print "ERPLREV\x01"; print pack("Q<", $ARGV[0])' "$SIZE" >> "$OUT"
chmod +x "$OUT"

echo "-> $OUT ($(wc -c < "$OUT") bytes, payload $SIZE bytes)"
