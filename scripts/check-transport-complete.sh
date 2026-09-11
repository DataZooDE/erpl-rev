#!/usr/bin/env bash
# The transport must deliver exactly what we say we deliver.
#
# zcl_erpl_rev_footprint.abap carries a checked-in list of every object erpl-rev
# puts into a customer system, and the E-FOOTPRINT suite asserts a real system
# matches it. Nothing asserted that the TRANSPORT ships that same list -- and it
# did not. It carried seven of fourteen, and the omissions were the ones that
# matter most: ZCL_ERPL_REV_CLIDRV, without which every CLI verb silently falls
# back to the temporary-class path that needs S_DEVELOP -- the exact thing the
# docs promise the transport avoids -- and ZCL_ERPL_REV_DIAG, which INSTALL.md
# names as the smoke test.
#
# Two lists, one truth. This compares them and fails if they differ.
#
#   bash scripts/check-transport-complete.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# The delivered set, from the footprint suite's `expected` method.
expected=$(sed -n '/METHOD expected\./,/ENDMETHOD\./p' abap/zcl_erpl_rev_footprint.abap \
           | grep -oE '`Z[A-Z_0-9]+`' | tr -d '`' | sort -u)

# What the transport packages into ZERPL_CORE.
shipped=$(sed -n '/^CORE=(/,/^)/p' scripts/package-transport.sh \
          | grep -oE '"Z[A-Z_0-9]+ ' | tr -d '" ' | sort -u)

missing=$(comm -23 <(printf '%s\n' "$expected") <(printf '%s\n' "$shipped"))
extra=$(comm -13 <(printf '%s\n' "$expected") <(printf '%s\n' "$shipped"))

rc=0
if [ -n "$missing" ]; then
  echo "TRANSPORT INCOMPLETE -- delivered per footprint, absent from the transport:"
  printf '  %s\n' $missing
  echo "  A customer importing this transport does NOT get these objects."
  rc=1
fi
if [ -n "$extra" ]; then
  echo "TRANSPORT OVERREACHES -- in the transport, not in the delivered footprint:"
  printf '  %s\n' $extra
  echo "  Either add them to zcl_erpl_rev_footprint.abap deliberately, or drop them."
  rc=1
fi
[ "$rc" = 0 ] && echo "TRANSPORT OK -- ships exactly the $(printf '%s\n' "$expected" | wc -l) delivered objects"
exit $rc
