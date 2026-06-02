#!/bin/bash
# Start the A4H ABAP trial like erpl/scripts/start-sap.sh, but disable the
# gateway registration ACL (gw/acl_mode = 0) so our external erpl-rev RFC
# server may register its PROGRAM_ID. Fine for a throwaway trial; not how you'd
# run production (there you'd use a reginfo allowlist instead).
#
# Leaves the erpl repo untouched: the instance profile is copied and augmented.
# Runs detached (-d); follow bootup with `docker logs -f a4h`.
set -euo pipefail

ERPL="${ERPL_ROOT:-$HOME/Projects/datazoo/erpl}"
SRC="$ERPL/scripts"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # the erpl-rev dir

IMAGE="sapse/abap-cloud-developer-trial:2023"
PROFILE_SRC="$SRC/A4H_D00_vhcala4hci.profile"
LICENSE_FILE="${1:-$SRC/A4H_Multiple.txt}"               # MAC below must match license
PROFILE_GEN="$HERE/sap/A4H_D00_vhcala4hci.profile.generated"

mkdir -p "$HERE/sap"
# Augmented profile = stock profile + open gateway ACL.
cp "$PROFILE_SRC" "$PROFILE_GEN"
cat >> "$PROFILE_GEN" <<'EOF'

# --- erpl-rev: allow external RFC server registration on this trial ---
gw/acl_mode = 0
EOF

# Replace any existing container.
docker rm -f a4h >/dev/null 2>&1 || true

docker run -d \
  --stop-timeout 3600 \
  --rm \
  --name a4h \
  -h vhcala4hci \
  --mac-address="02:42:ac:11:00:02" \
  -p 3200:3200 -p 3300:3300 -p 8443:8443 \
  -p 30213:30213 -p 50000:50000 -p 50001:50001 \
  --sysctl kernel.shmmax=42949672960 \
  --sysctl kernel.shmmni=32768 \
  --sysctl kernel.shmall=10485760 \
  --sysctl kernel.msgmni=1024 \
  --sysctl kernel.sem="1250 256000 100 8192" \
  --ulimit nofile=1048576:1048576 \
  -v "$PROFILE_GEN":/usr/sap/A4H/SYS/profile/A4H_D00_vhcala4hci \
  -v "$LICENSE_FILE":/opt/sap/ASABAP_license \
  "$IMAGE" -agree-to-sap-license -skip-limits-check

echo "a4h started detached. Follow boot with: docker logs -f a4h"
