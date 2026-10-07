#!/bin/bash
# PNG of the VM's screen on the host via `prlctl capture` (Parallels' own framebuffer grab: nothing runs
# on the host screen, no Screen Recording permission on either side, works while the VM is headless).
#   scripts/macos-vm/screenshot.sh                      build/vm-shots/<time>.png
#   scripts/macos-vm/screenshot.sh /path/out.png
set -euo pipefail
. "$(dirname "$0")/common.sh"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
vm_running || { echo "$VM is not running (scripts/macos-vm/start.sh)" >&2; exit 1; }
OUT="${1:-$ROOT/build/vm-shots/$(date +%Y%m%d-%H%M%S).png}"
mkdir -p "$(dirname "$OUT")"
"$PRLCTL" capture "$VM" --file "$OUT" >/dev/null
[ -s "$OUT" ] || { echo "capture failed" >&2; exit 1; }
echo "$OUT"
