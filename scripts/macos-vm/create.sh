#!/bin/bash
# One-time setup: pull the macOS 27 image, make the clean snapshot VM "replaynes-test-clean"
# (4 CPU / 8 GB / 1920x1200 / 80 GB, provisioned: your SSH key, no sleep / screen saver / lock,
# no Spotlight indexing or update checks), then clone the working VM "replaynes-test" from it.
#   scripts/macos-vm/create.sh           FORCE=1 scripts/macos-vm/create.sh   (rebuild the clean VM)
# Nothing on the host is changed besides tart's VM storage (~/.tart) and ~/.local/state/replaynes-vm.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/common.sh"
[ -n "$SSH_KEY" ] || { echo "no ~/.ssh/id_*.pub key found (ssh-keygen -t ed25519)" >&2; exit 1; }

if vm_exists "$CLEAN_VM" && [ "${FORCE:-0}" != 1 ]; then
  echo "$CLEAN_VM exists (FORCE=1 rebuilds it)"
else
  if vm_exists "$CLEAN_VM"; then
    VM="$CLEAN_VM" "$HERE/stop.sh" >/dev/null 2>&1 || true
    "$TART" delete "$CLEAN_VM"
  fi
  echo "== pulling/cloning $IMAGE -> $CLEAN_VM (tens of GB the first time)"
  "$TART" clone "$IMAGE" "$CLEAN_VM"
  "$TART" set "$CLEAN_VM" --cpu "$VM_CPU" --memory "$VM_MEM_MB" --display "$VM_DISPLAY" --disk-size "$VM_DISK_GB"

  echo "== booting $CLEAN_VM for provisioning"
  VM="$CLEAN_VM" SSH_MODE=pw "$HERE/start.sh"
  PUBKEY="$(cat "$SSH_KEY.pub")"
  VM="$CLEAN_VM" vm_ssh_pw "PW=$(printf %q "$VM_PASS") PUBKEY=$(printf %q "$PUBKEY") bash -s" <<'GUEST'
set -u
# 1. passwordless SSH with the host user's key
mkdir -p ~/.ssh && chmod 700 ~/.ssh
grep -qxF "$PUBKEY" ~/.ssh/authorized_keys 2>/dev/null || echo "$PUBKEY" >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
# passwordless sudo for the scripts (local test VM only)
printf '%s\n' "$PW" | sudo -S -p '' sh -c 'echo "admin ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/admin-nopasswd && chmod 440 /etc/sudoers.d/admin-nopasswd'
s() { sudo -n "$@"; }
# 2. never sleep, no screen saver, no lock screen
s pmset -a displaysleep 0 sleep 0 disksleep 0 powernap 0 >/dev/null
defaults -currentHost write com.apple.screensaver idleTime -int 0
defaults write com.apple.screensaver idleTime -int 0
defaults write com.apple.screensaver askForPassword -int 0
sysadminctl -screenLock off -password "$PW" 2>/dev/null || true
# 3. fewer interruptions: no update checks / banners, no Spotlight indexing (CPU noise)
s softwareupdate --schedule off >/dev/null 2>&1 || true
s defaults write /Library/Preferences/com.apple.SoftwareUpdate AutomaticCheckEnabled -bool false
s defaults write /Library/Preferences/com.apple.SoftwareUpdate AutomaticDownload -bool false
s defaults write /Library/Preferences/com.apple.commerce AutoUpdate -bool false
s mdutil -a -i off >/dev/null 2>&1 || true
defaults write com.apple.notificationcenterui bannerTime -int 1      # banners vanish fast (best effort)
defaults write com.apple.CrashReporter DialogType -string none       # no "quit unexpectedly" dialog
defaults write com.apple.dock autohide -bool true                    # more room, fewer occlusions
killall Dock 2>/dev/null || true
# 4. grow the APFS container to the new disk size (tart set --disk-size)
yes | sudo -n diskutil repairDisk disk0 >/dev/null 2>&1 || true
CONTAINER="$(diskutil list physical disk0 | awk '/Apple_APFS/ {print $NF; exit}')"
[ -n "$CONTAINER" ] && s diskutil apfs resizeContainer "$CONTAINER" 0 >/dev/null 2>&1 || true
mkdir -p ~/ReplayNES-test
echo "provisioned: $(sw_vers -productVersion) ($(sw_vers -buildVersion)), disk $(df -h / | awk 'NR==2 {print $2}')"
GUEST
  VM="$CLEAN_VM" wait_ssh       # key auth must work now
  VM="$CLEAN_VM" "$HERE/stop.sh"
fi

if vm_exists "$VM" && [ "${FORCE:-0}" != 1 ]; then
  echo "$VM exists (scripts/macos-vm/reset.sh re-clones it from $CLEAN_VM)"
else
  "$HERE/reset.sh"
fi
"$TART" list | grep -E "NAME|$VM"
