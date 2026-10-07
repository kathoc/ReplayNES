#!/bin/bash
# Copy a built ReplayNES.app (or a release zip) into the running VM's /Applications, plus any extra
# files (ROMs, projects) into ~/ReplayNES-test/ in the guest. Nothing is built here.
#   scripts/macos-vm/deploy.sh                                   build/ReplayNES.app
#   scripts/macos-vm/deploy.sh dist/ReplayNES-0.3.0-macOS-arm64.zip
#   scripts/macos-vm/deploy.sh build/ReplayNES.app /path/test.nes
#   QUARANTINE=1 scripts/macos-vm/deploy.sh dist/...zip     mark it as downloaded by Safari, so the
#                                                           first launch goes through Gatekeeper
#                                                           exactly like a user's download
set -euo pipefail
. "$(dirname "$0")/common.sh"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${1:-$ROOT/build/ReplayNES.app}"; shift || true
[ -e "$SRC" ] || { echo "not found: $SRC" >&2; exit 1; }
vm_running "$VM" || { echo "$VM is not running (scripts/macos-vm/start.sh)" >&2; exit 1; }
IP="$(vm_ip)"
SSHC=(ssh "${SSH_OPTS[@]}" -o BatchMode=yes "$VM_USER@$IP")

"${SSHC[@]}" 'pkill -x ReplayNES 2>/dev/null; rm -rf /Applications/ReplayNES.app ~/ReplayNES-test/incoming; mkdir -p ~/ReplayNES-test/incoming'
case "$SRC" in
  *.zip)
    vm_scp "$SRC" "ReplayNES-test/incoming/app.zip"
    if [ "${QUARANTINE:-0}" = 1 ]; then
      # What Safari puts on a download; ditto carries it onto the extracted app.
      "${SSHC[@]}" 'xattr -w com.apple.quarantine "0081;$(printf %x $(date +%s));Safari;$(uuidgen)" ~/ReplayNES-test/incoming/app.zip'
    fi
    "${SSHC[@]}" 'ditto -x -k ~/ReplayNES-test/incoming/app.zip ~/ReplayNES-test/incoming/x &&
      app=$(find ~/ReplayNES-test/incoming/x -maxdepth 3 -name ReplayNES.app -type d | head -1) &&
      [ -n "$app" ] && mv "$app" /Applications/'
    ;;
  *.app|*.app/)
    # tar stream keeps the bundle's symlinks, modes and code signature intact.
    COPYFILE_DISABLE=1 tar -C "$(dirname "$SRC")" -cf - "$(basename "${SRC%/}")" | "${SSHC[@]}" 'tar -xf - -C /Applications'
    if [ "${QUARANTINE:-0}" = 1 ]; then
      "${SSHC[@]}" 'xattr -w com.apple.quarantine "0081;$(printf %x $(date +%s));Safari;$(uuidgen)" /Applications/ReplayNES.app'
    fi
    ;;
  *) echo "expected a .app or .zip: $SRC" >&2; exit 1 ;;
esac
for f in "$@"; do vm_scp "$f" "ReplayNES-test/"; done
"${SSHC[@]}" 'rm -rf ~/ReplayNES-test/incoming
  v=$(defaults read /Applications/ReplayNES.app/Contents/Info CFBundleShortVersionString)
  q=$(xattr -p com.apple.quarantine /Applications/ReplayNES.app 2>/dev/null || echo none)
  echo "deployed ReplayNES $v to /Applications (quarantine: $q)"; ls ~/ReplayNES-test'
