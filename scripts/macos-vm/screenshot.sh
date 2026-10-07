#!/bin/bash
# Grab the VM's screen into a PNG on the host, through tart's Virtualization.framework VNC server
# (started by start.sh). Nothing runs in the guest and nothing captures the host's screen, so no
# Screen Recording permission is involved on either side.
#   scripts/macos-vm/screenshot.sh                      build/vm-shots/<time>.png
#   scripts/macos-vm/screenshot.sh /path/out.png
#   scripts/macos-vm/screenshot.sh --do 'move 960 600 click 1'   other vncdo commands (mouse/keys)
# Needs vncdotool on the host: `uv tool install vncdotool` (or pipx / pip --user).
set -euo pipefail
. "$(dirname "$0")/common.sh"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VNCDO="$(command -v vncdo || true)"
[ -n "$VNCDO" ] || { echo "vncdo not found: uv tool install vncdotool" >&2; exit 1; }
vm_running "$VM" || { echo "$VM is not running (scripts/macos-vm/start.sh)" >&2; exit 1; }
URL="$(vnc_url)"
[ -n "$URL" ] || { echo "no VNC URL in $(run_log); restart the VM with scripts/macos-vm/start.sh" >&2; exit 1; }
PASS="$(sed -E 's#vnc://:([^@]*)@.*#\1#' <<<"$URL")"
HOSTPORT="$(sed -E 's#.*@([^:/]+):([0-9]+).*#\1::\2#' <<<"$URL")"
if [ "${1:-}" = "--do" ]; then
  shift
  # shellcheck disable=SC2086
  PYTHONWARNINGS=ignore exec "$VNCDO" -s "$HOSTPORT" -p "$PASS" $*
fi
OUT="${1:-$ROOT/build/vm-shots/$(date +%Y%m%d-%H%M%S).png}"
mkdir -p "$(dirname "$OUT")"
# The VZ VNC server sends the first frame once the guest has drawn; retry a couple of times.
for i in 1 2 3; do
  if PYTHONWARNINGS=ignore "$VNCDO" -t 30 -s "$HOSTPORT" -p "$PASS" capture "$OUT" 2>/dev/null && [ -s "$OUT" ]; then
    echo "$OUT"; exit 0
  fi
  sleep 2
done
echo "screenshot failed" >&2; exit 1
