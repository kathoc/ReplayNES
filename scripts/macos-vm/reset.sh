#!/bin/bash
# Throw the working VM away and re-clone it from the clean snapshot (APFS copy-on-write: seconds,
# almost no extra disk). Every test run can start from a freshly provisioned macOS this way.
#   scripts/macos-vm/reset.sh            START=1 scripts/macos-vm/reset.sh   (and boot it)
set -euo pipefail
. "$(dirname "$0")/common.sh"
vm_exists "$CLEAN_VM" || { echo "clean snapshot $CLEAN_VM is missing (scripts/macos-vm/create.sh)" >&2; exit 1; }
vm_running "$CLEAN_VM" && { echo "$CLEAN_VM is running; stop it first (VM=$CLEAN_VM scripts/macos-vm/stop.sh)" >&2; exit 1; }
if vm_exists "$VM"; then
  vm_running "$VM" && "$(dirname "$0")/stop.sh"
  "$TART" delete "$VM"
fi
"$TART" clone "$CLEAN_VM" "$VM"
echo "$VM re-cloned from $CLEAN_VM"
[ "${START:-0}" = 1 ] && exec "$(dirname "$0")/start.sh"
exit 0
