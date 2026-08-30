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
# Pass "-" when the RFC backend is linked statically (RFC_BACKEND=proto
# RFC_LINK=static): the shim is inside the binary, so there is no SAP NW RFC
# SDK and no ICU to stage and the payload is DuckDB alone.
SDK_LIB="${3:?nwrfcsdk <plat>/lib dir, or - for a statically linked RFC backend}"
DUCKDB_DIR="${4:?duckdb dist dir}"
OUT="${5:?output dir}"

case "$PLATFORM" in
  linux) SAP_LIBS=(libsapnwrfc.so libsapucum.so libicudata.so.50 libicui18n.so.50 libicuuc.so.50); DUCKDB_LIB=libduckdb.so ;;
  osx)   SAP_LIBS=(libsapnwrfc.dylib libsapucum.dylib libicudata.50.dylib libicui18n.50.dylib libicuuc.50.dylib); DUCKDB_LIB=libduckdb.dylib ;;
  *) echo "unknown platform: $PLATFORM" >&2; exit 2 ;;
esac

if [ "$SDK_LIB" = "-" ]; then SAP_LIBS=(); fi

mkdir -p "$OUT"
cp "$SERVER" "$OUT/erpl_rev_server"
# Guard the expansion: macOS ships bash 3.2, where "${arr[@]}" on an EMPTY array
# counts as unbound under `set -u` and aborts. Linux's bash 5 expands it to
# nothing and carries on, so this only bites on the mac.
if [ "${#SAP_LIBS[@]}" -gt 0 ]; then
    for l in "${SAP_LIBS[@]}"; do cp "$SDK_LIB/$l" "$OUT/$l"; done
fi
cp "$DUCKDB_DIR/$DUCKDB_LIB" "$OUT/$DUCKDB_LIB"

echo "Staged $(ls "$OUT" | wc -l) files into $OUT:"
ls -la "$OUT" | awk 'NR>1{print "  "$5"  "$NF}'
