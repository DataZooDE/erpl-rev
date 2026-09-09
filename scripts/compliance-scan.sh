#!/bin/bash
# Two static gates that run on every e2e, and can be run on their own.
#
# 1) NO NON-PUBLISHED SAP INTERFACE. The licence and API-policy position rests on
#    erpl-rev using customer-owned Open SQL, CDS and ADBC-from-ABAP only. A call
#    to an ODP replication FM, RFC_READ_TABLE, a BICS interface or a generated
#    /1DH/ object would break that quietly, in one commit, months before anyone
#    reads the code again.
#
#    Scanned as CALL FUNCTION / CALL METHOD operands, not as raw text: the names
#    legitimately appear in comments and documentation that explain why they are
#    NOT used. A grep that goes red on its own documentation teaches people to
#    widen the pattern until it proves nothing.
#
# 2) NO THIRD-PARTY PRODUCT OR VENDOR NAMES in shipped artefacts. Competitor
#    products are studied as blueprints; naming one in code, comments, tests,
#    docs or a commit message is a different thing entirely. SAP's own product
#    names are allowed -- erpl-rev cannot describe what it does without them.
#
# Usage: scripts/compliance-scan.sh [--staged]
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

fail=0
note() { echo "COMPLIANCE FAIL: $*" >&2; fail=1; }

# --- 1) non-published SAP interfaces ---------------------------------------
# Strip full-line ABAP comments (*) and trailing " comments before matching, so
# only real call sites are considered.
strip_abap() { sed -e 's/^\*.*$//' -e 's/"[^"]*$//' "$1"; }

FORBIDDEN_CALLS='RODPS_REPL_[A-Z_]*|RFC_READ_TABLE|BAPI_ODP_[A-Z_]*|/1DH/[A-Z0-9_]*'
while IFS= read -r f; do
  hits=$(strip_abap "$f" \
    | grep -nEi "(CALL[[:space:]]+FUNCTION|CALL[[:space:]]+METHOD)[[:space:]]*['\`\"]?($FORBIDDEN_CALLS)" \
    || true)
  [ -n "$hits" ] && note "non-published SAP interface called in $f:"$'\n'"$hits"
done < <(find abap -name '*.abap' -o -name '*.ddl' 2>/dev/null)

# The server must not reach the database directly either; everything goes
# through ABAP. A native SAP DB driver linked in would mean it does.
if grep -rqiE '#include[[:space:]]*[<"](hdbcli|sqlext|occi)' src/ 2>/dev/null; then
  note "src/ links a native database client; extraction must go through ABAP"
fi

# --- 2) third-party names in shipped artefacts ------------------------------
# Add a vendor or product here the moment it is studied, not after it leaks.
DENY='fivetran|theobald|qlik|matillion|informatica|talend|dvd|datavard|snp[[:space:]]|snpgroup|glue[[:space:]]+(table|queue|scheduler|process)'
# SAP's own product names are not third-party, and erpl-rev cannot describe
# itself without them.
ALLOW='sap|s/4|s4hana|hana|netweaver|abap|bw/4|ducklake|duckdb|iceberg|motherduck|postgres|bigquery|parquet'

while IFS= read -r f; do
  hits=$(grep -nEi "$DENY" "$f" 2>/dev/null | grep -viE "$ALLOW" || true)
  [ -n "$hits" ] && note "third-party product/vendor name in shipped artefact $f:"$'\n'"$hits"
done < <(find abap src docs -type f \( -name '*.abap' -o -name '*.ddl' -o -name '*.cpp' \
           -o -name '*.hpp' -o -name '*.md' \) 2>/dev/null)

if [ "${1:-}" = "--staged" ]; then
  msg=$(git log -1 --pretty=%B 2>/dev/null || true)
  if echo "$msg" | grep -qiE "$DENY"; then
    note "third-party product/vendor name in the last commit message"
  fi
fi

[ "$fail" = 0 ] && echo "COMPLIANCE OK (no non-published SAP interfaces, no third-party names)"
exit "$fail"
