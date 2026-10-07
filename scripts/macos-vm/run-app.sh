#!/bin/bash
# (Re)launch ReplayNES in the VM's GUI session with the given app arguments (guest paths).
# Host files given with ROM=... are copied to ~/ReplayNES-test/ first and opened with --rom --autoplay.
#   scripts/macos-vm/run-app.sh
#   ROM=/tmp/test.nes scripts/macos-vm/run-app.sh
#   UI_LANG=ja ROM=/tmp/test.nes scripts/macos-vm/run-app.sh      Japanese UI for this run only
#   scripts/macos-vm/run-app.sh --rom /Users/admin/ReplayNES-test/test.nes --autoplay
# The app runs under `open`, i.e. through LaunchServices and Gatekeeper, like a Finder launch.
# FRESH=1 deletes the app's preferences / Application Support / ~/Documents/ReplayNES first.
set -euo pipefail
. "$(dirname "$0")/common.sh"
vm_running "$VM" || { echo "$VM is not running (scripts/macos-vm/start.sh)" >&2; exit 1; }
IP="$(vm_ip)"
SSHC=(ssh "${SSH_OPTS[@]}" -o BatchMode=yes "$VM_USER@$IP")
ARGS=("$@")
if [ -n "${ROM:-}" ]; then
  vm_scp "$ROM" "ReplayNES-test/"
  ARGS=(--rom "/Users/$VM_USER/ReplayNES-test/$(basename "$ROM")" --autoplay ${ARGS[@]+"${ARGS[@]}"})
fi
[ -n "${UI_LANG:-}" ] && ARGS+=(-AppleLanguages "($UI_LANG)")
Q=""; for a in ${ARGS[@]+"${ARGS[@]}"}; do Q+=" $(printf %q "$a")"; done
"${SSHC[@]}" "pkill -x ReplayNES 2>/dev/null; for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -x ReplayNES >/dev/null || break; sleep 0.5; done
  if [ '${FRESH:-0}' = 1 ]; then
    defaults delete io.github.replaynes.ReplayNES 2>/dev/null
    rm -rf ~/Library/Application\ Support/ReplayNES ~/Library/Caches/io.github.replaynes.ReplayNES ~/Documents/ReplayNES
  fi
  open -n -a /Applications/ReplayNES.app ${Q:+--args$Q}
  sleep 2; pgrep -x ReplayNES >/dev/null && echo 'ReplayNES is running (pid '\$(pgrep -x ReplayNES)')' || echo 'ReplayNES is NOT running (Gatekeeper dialog? take a screenshot)'"
