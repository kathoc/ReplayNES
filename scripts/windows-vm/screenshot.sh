#!/bin/bash
# PNG of the Windows VM's screen via `prlctl capture` (Parallels' framebuffer grab; nothing is
# captured from the host screen).
#   scripts/windows-vm/screenshot.sh                 build/vm-shots/windows-<time>.png
#   scripts/windows-vm/screenshot.sh /path/out.png
set -euo pipefail
. "$(dirname "$0")/common.sh"
need_running
OUT="${1:-$ROOT/build/vm-shots/windows-$(date +%Y%m%d-%H%M%S).png}"
mkdir -p "$(dirname "$OUT")"
"$PRLCTL" capture "$VM" --file "$OUT" >/dev/null
[ -s "$OUT" ] || { echo "capture failed" >&2; exit 1; }
echo "$OUT"
