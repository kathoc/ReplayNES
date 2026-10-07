#!/bin/bash
# Shut the VM down (graceful, guest-side first; forced after TIMEOUT seconds). Leaves it stopped, not suspended.
#   scripts/macos-vm/stop.sh
set -euo pipefail
. "$(dirname "$0")/common.sh"
[ "$(vm_status)" = stopped ] && { echo "$VM is not running"; exit 0; }
if vm_running; then vm_ssh 'sudo -n shutdown -h now' >/dev/null 2>&1 || true; fi
for _ in $(seq 1 "${TIMEOUT:-60}"); do [ "$(vm_status)" = stopped ] && break; sleep 1; done
[ "$(vm_status)" = stopped ] || "$PRLCTL" stop "$VM" --kill >/dev/null 2>&1 || true
for _ in $(seq 1 30); do [ "$(vm_status)" = stopped ] && break; sleep 1; done
[ "$(vm_status)" = stopped ] || { echo "$VM did not stop ($(vm_status))" >&2; exit 1; }
echo "$VM stopped"
