#!/bin/bash
# Deploy erpl-rev's ABAP objects to the A4H trial.
#
# THIS IS NOW A SHIM. The objects themselves live in erpl
# (erpl/scripts/sap/assets/rev/abap) and are deployed by erpl's provisioner, so each one
# exists once rather than once per repo.
#
# The move happened because the *order* matters and could not be expressed from here:
# activating the BW Modeling services restarts the ABAP instance, and restarting the
# instance re-materialises every PSE from the database -- destroying the SNC and wsRFC
# certificate trust erpl-proto's live tests depend on. erpl's provision.d encodes that
# sequence. See DataZooDE/erpl#123.
#
#   scripts/deploy-abap.sh          # deploy erpl-rev's objects (delegates to erpl)
#
# To provision the whole trial -- BW services, every repo's fixtures, certificates, BW
# test data -- run erpl's provisioner instead:
#
#   erpl/scripts/sap/provision.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ERPL_DIR="${ERPL_DIR:-$HERE/../erpl}"
if [ ! -x "$ERPL_DIR/scripts/sap/provision.sh" ]; then
    # Loudly, not silently. These objects are what the live E2E tests call; without them
    # the suite fails with a read timeout that names nothing, or skips and reads green.
    cat >&2 <<MSG
erpl-rev's ABAP objects now live in erpl, and no erpl checkout was found.

  looked in: $ERPL_DIR

Set ERPL_DIR to an erpl checkout, or clone erpl beside this repository, then re-run.
Nothing was deployed.
MSG
    exit 1
fi

exec "$ERPL_DIR/scripts/sap/provision.sh" --only 25 "$@"
