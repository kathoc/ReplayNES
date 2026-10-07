#!/bin/bash
# Roll the VM back to its snapshot ("clean" by default; SNAPSHOT=name). The VM is stopped first.
# Everything done since the snapshot (installed apps, preferences, files) is discarded.
#   scripts/macos-vm/reset.sh            START=1 scripts/macos-vm/reset.sh   (and boot it)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/common.sh"
vm_exists || { echo "VM $VM does not exist" >&2; exit 1; }
# snapshot-list -j: {"{id}": {"name": "...", ...}, ...}
ID="$("$PRLCTL" snapshot-list "$VM" -j | SNAP="$SNAPSHOT" python3 -c '
import json,os,sys
d=json.load(sys.stdin)
for k,v in d.items():
    if v.get("name")==os.environ["SNAP"]: print(k); break' 2>/dev/null || true)"
[ -n "$ID" ] || { echo "snapshot \"$SNAPSHOT\" not found on $VM (prlctl snapshot-list $VM)" >&2; exit 1; }
[ "$(vm_status)" = stopped ] || "$HERE/stop.sh"
"$PRLCTL" snapshot-switch "$VM" --id "$ID" >/dev/null
echo "$VM reset to snapshot \"$SNAPSHOT\" $ID ($(vm_status))"
[ "${START:-0}" = 1 ] && exec "$HERE/start.sh"
exit 0
