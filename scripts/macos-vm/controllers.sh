#!/bin/bash
# Game controllers and the VM (Parallels USB passthrough).
#   scripts/macos-vm/controllers.sh                       host USB devices usable by the VM + connection state
#   scripts/macos-vm/controllers.sh connect 'Pro Controller'     hand a device to the VM (name as listed, '#N' suffix ok)
#   scripts/macos-vm/controllers.sh disconnect 'Pro Controller'  give it back to the host
#   scripts/macos-vm/controllers.sh guest [seconds]       what the guest sees (HID gamepads + GCController)
# `guest` builds tools/gc-list.swift on the host (the guest has no compiler) and runs it in the guest.
set -euo pipefail
. "$(dirname "$0")/common.sh"
cmd="${1:-list}"; [ $# -gt 0 ] && shift
case "$cmd" in
  list)
    "$PRLSRVCTL" usb list | awk '
      /^Device:/ { if (n) print line; n=$0; sub(/^Device: /,"",n); gsub(/\x27/,"",n); sub(/ *: *$/,"",n); line=n; next }
      /Present-On-Host/ { p=$2 } /Connected-To-Vm/ { c=$2 } /Compatible-with-macOS-VM/ { line=line "  [host:" p " connected:" c " vm-compatible:" $2 "]" }
      END { if (n) print line }'
    ;;
  connect|disconnect)
    [ $# -ge 1 ] || { echo "usage: controllers.sh $cmd '<device name>'" >&2; exit 2; }
    "$PRLCTL" set "$VM" "--device-$cmd" "$1" ;;
  guest)
    OUT="$STATE_DIR/gc-list"; SRC="$(dirname "$0")/tools/gc-list.swift"
    if [ ! -x "$OUT" ] || [ "$SRC" -nt "$OUT" ]; then xcrun swiftc -O -target arm64-apple-macos14 "$SRC" -o "$OUT"; fi
    vm_running || { echo "$VM is not running (scripts/macos-vm/start.sh)" >&2; exit 1; }
    vm_scp "$OUT" "ReplayNES-test/gc-list"
    vm_ssh "~/ReplayNES-test/gc-list ${1:-4}" ;;
  *) echo "usage: controllers.sh [list | connect <dev> | disconnect <dev> | guest [sec]]" >&2; exit 2 ;;
esac
