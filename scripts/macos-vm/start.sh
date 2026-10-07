#!/bin/bash
# Boot the VM (headless where Parallels allows), provision it (idempotent) and wait for SSH and a
# logged-in desktop session. Resumes/starts only; it never changes the VM's snapshots.
#   scripts/macos-vm/start.sh            REPLAYNES_VM=Other scripts/macos-vm/start.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/common.sh"
vm_exists || { echo "VM $VM does not exist (docs/MACOS_VM.md, Setup)" >&2; exit 1; }
case "$(vm_status)" in
  running) ;;
  suspended|paused) "$PRLCTL" resume "$VM" >/dev/null ;;
  *)
    "$PRLCTL" set "$VM" --startup-view headless >/dev/null 2>&1 || true   # no Parallels window
    "$PRLCTL" start "$VM" >/dev/null ;;
esac
wait_ssh
"$HERE/provision.sh"
echo "$VM is running: ip $(vm_ip), ssh $VM_USER@$(vm_ip)"
