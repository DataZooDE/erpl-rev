#!/usr/bin/env bash
# Stage the erpl-rev runtime payload — the inner server plus the shared libraries
# it needs at run time — into a plain directory.
#
# This is the SINGLE SOURCE OF TRUTH for the runtime lib set, shared by:
#   - scripts/bundle.sh        (self-extracting single-file distributable)
#   - the Docker image build    (.github/workflows/release.yml -> Dockerfile)
# so the two never drift when the ICU/DuckDB versions bump.
#
# Usage: scripts/stage_runtime.sh <linux|osx> <server-bin> <nwrfc-lib-dir> <duckdb-dir> <out-dir>
set -euo pipefail

PLATFORM="${1:?platform: linux|osx}"
SERVER="${2:?server binary}"
SDK_LIB="${3:?nwrfcsdk <plat>/lib dir}"
DUCKDB_DIR="${4:?duckdb dist dir}"
OUT="${5:?output dir}"

case "$PLATFORM" in
  linux) SAP_LIBS=(libsapnwrfc.so libsapucum.so libicudata.so.50 libicui18n.so.50 libicuuc.so.50); DUCKDB_LIB=libduckdb.so ;;
  osx)   SAP_LIBS=(libsapnwrfc.dylib libsapucum.dylib libicudata.50.dylib libicui18n.50.dylib libicuuc.50.dylib); DUCKDB_LIB=libduckdb.dylib ;;
  *) echo "unknown platform: $PLATFORM" >&2; exit 2 ;;
esac

mkdir -p "$OUT"
cp "$SERVER" "$OUT/erpl_rev_server"
for l in "${SAP_LIBS[@]}"; do cp "$SDK_LIB/$l" "$OUT/$l"; done
cp "$DUCKDB_DIR/$DUCKDB_LIB" "$OUT/$DUCKDB_LIB"

echo "Staged $(ls "$OUT" | wc -l) files into $OUT:"
ls -la "$OUT" | awk 'NR>1{print "  "$5"  "$NF}'
