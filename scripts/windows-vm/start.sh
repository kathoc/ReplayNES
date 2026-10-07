#!/bin/bash
# Start (or resume) the Windows VM and wait for the logged-in desktop session that
# `prlctl exec --current-user` needs. Does not change any VM setting or snapshot.
#   scripts/windows-vm/start.sh            WINDOWS_VM="Other VM" scripts/windows-vm/start.sh
set -euo pipefail
. "$(dirname "$0")/common.sh"
vm_exists || { echo "VM \"$VM\" does not exist (docs/WINDOWS.md)" >&2; exit 1; }
case "$(vm_status)" in
  running) ;;
  suspended|paused) "$PRLCTL" resume "$VM" >/dev/null ;;
  *) "$PRLCTL" start "$VM" >/dev/null ;;
esac
wait_session
echo "$VM is running; user session: $(win_cmd whoami | tr -d '\r')"
