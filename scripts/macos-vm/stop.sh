#!/bin/bash
# Shut the test VM down (graceful; tart forces it off after the timeout).
#   scripts/macos-vm/stop.sh             VM=replaynes-test-clean scripts/macos-vm/stop.sh
set -euo pipefail
. "$(dirname "$0")/common.sh"
if ! vm_running "$VM"; then echo "$VM is not running"; exit 0; fi
# A guest-side shutdown first keeps the disk consistent; tart stop then waits for it.
vm_ssh 'sudo -n shutdown -h now' >/dev/null 2>&1 || true
"$TART" stop --timeout "${TIMEOUT:-60}" "$VM" >/dev/null 2>&1 || true
for _ in $(seq 1 60); do vm_running "$VM" || break; sleep 1; done
vm_running "$VM" && { echo "$VM did not stop" >&2; exit 1; }
echo "$VM stopped"
