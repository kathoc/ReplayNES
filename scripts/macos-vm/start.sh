#!/bin/bash
# Boot the test VM headless (no window on the host desktop) with tart's built-in VNC server
# (Virtualization.framework, no guest involvement: screenshot.sh grabs frames through it).
# Waits until SSH answers. Audio is off unless AUDIO=1 (it would play on the host's speakers).
#   scripts/macos-vm/start.sh            VM=replaynes-test-clean scripts/macos-vm/start.sh
set -euo pipefail
. "$(dirname "$0")/common.sh"
vm_exists "$VM" || { echo "VM $VM does not exist (scripts/macos-vm/create.sh)" >&2; exit 1; }
if vm_running "$VM"; then
  echo "$VM is already running ($(vm_ip))"; exit 0
fi
ARGS=(--no-graphics --vnc-experimental)
[ "${AUDIO:-0}" = 1 ] || ARGS+=(--no-audio)
nohup "$TART" run "${ARGS[@]}" "$VM" > "$(run_log)" 2>&1 &
disown || true
for _ in $(seq 1 100); do [ -n "$(vnc_url)" ] && break; sleep 0.2; done
IP="$(vm_ip)"
if [ "${WAIT_SSH:-1}" = 1 ]; then wait_ssh "${SSH_MODE:-}"; fi
echo "$VM is running: ip $IP, ssh $VM_USER@$IP, VNC $(vnc_url | sed 's#//:[^@]*@#//:***@#')"
