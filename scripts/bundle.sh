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

case "$PLATFORM" in
  linux) SAP_LIBS=(libsapnwrfc.so libsapucum.so libicudata.so.50 libicui18n.so.50 libicuuc.so.50); DUCKDB_LIB=libduckdb.so ;;
  osx)   SAP_LIBS=(libsapnwrfc.dylib libsapucum.dylib libicudata.50.dylib libicui18n.50.dylib libicuuc.50.dylib); DUCKDB_LIB=libduckdb.dylib ;;
  *) echo "unknown platform: $PLATFORM" >&2; exit 2 ;;
esac

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
PAY="$STAGE/payload"; mkdir -p "$PAY"

cp "$SERVER" "$PAY/erpl_rev_server"
for l in "${SAP_LIBS[@]}"; do cp "$SDK_LIB/$l" "$PAY/$l"; done
cp "$DUCKDB_DIR/$DUCKDB_LIB" "$PAY/$DUCKDB_LIB"

echo "Bundling $(ls "$PAY" | wc -l) files:"; ls -la "$PAY" | awk 'NR>1{print "  "$5"  "$NF}'

( cd "$PAY" && tar -cf "$STAGE/payload.tar" * )
SIZE="$(wc -c < "$STAGE/payload.tar")"

mkdir -p "$(dirname "$OUT")"
cat "$LAUNCHER" "$STAGE/payload.tar" > "$OUT"
perl -e 'print "ERPLREV\x01"; print pack("Q<", $ARGV[0])' "$SIZE" >> "$OUT"
chmod +x "$OUT"

echo "-> $OUT ($(wc -c < "$OUT") bytes, payload $SIZE bytes)"
