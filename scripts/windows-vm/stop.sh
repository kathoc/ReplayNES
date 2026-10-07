#!/bin/bash
# Shut the Windows VM down gracefully (guest shutdown via Parallels Tools; forced only after
# TIMEOUT seconds). Leaves it stopped, not suspended.
#   scripts/windows-vm/stop.sh
set -euo pipefail
. "$(dirname "$0")/common.sh"
[ "$(vm_status)" = stopped ] && { echo "$VM is not running"; exit 0; }
"$PRLCTL" stop "$VM" >/dev/null 2>&1 &
for _ in $(seq 1 "${TIMEOUT:-120}"); do [ "$(vm_status)" = stopped ] && break; sleep 1; done
[ "$(vm_status)" = stopped ] || "$PRLCTL" stop "$VM" --kill >/dev/null 2>&1 || true
wait || true
[ "$(vm_status)" = stopped ] || { echo "$VM did not stop ($(vm_status))" >&2; exit 1; }
echo "$VM stopped"
