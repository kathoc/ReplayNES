#!/bin/bash
# Which game controllers does the guest see? Builds tools/gc-list.swift on the host (the guest has
# no compiler), copies it in and runs it: HID gamepads/joysticks (IOKit) + GameController.framework.
#   scripts/macos-vm/controllers.sh          (exit 0 = at least one GCController)
# Under tart the answer is always 0: Virtualization.framework passes no USB/Bluetooth HID devices.
set -euo pipefail
. "$(dirname "$0")/common.sh"
OUT="$STATE_DIR/gc-list"
SRC="$(dirname "$0")/tools/gc-list.swift"
if [ ! -x "$OUT" ] || [ "$SRC" -nt "$OUT" ]; then
  xcrun swiftc -O -target arm64-apple-macos14 "$SRC" -o "$OUT"
fi
vm_running "$VM" || { echo "$VM is not running (scripts/macos-vm/start.sh)" >&2; exit 1; }
vm_scp "$OUT" "ReplayNES-test/gc-list"
vm_ssh "~/ReplayNES-test/gc-list ${1:-4}"
