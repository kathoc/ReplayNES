#!/bin/bash
# Idempotent guest preparation for unattended tests: passwordless sudo, never sleep / screen saver /
# lock, auto-login, no update checks. Inside the guest only. Needs the key-based SSH from the docs
# (the first sudo uses the account password). Called by start.sh; a reboot happens only when
# auto-login had to be enabled and nobody is logged in at the console.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/common.sh"
vm_ssh "PW=$(printf %q "$VM_PASS") U=$(printf %q "$VM_USER") bash -s" <<'GUEST'
set -u
[ -f /etc/sudoers.d/admin-nopasswd ] && sudo -n true 2>/dev/null ||
  printf '%s\n' "$PW" | sudo -S -p '' sh -c "echo '$U ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/admin-nopasswd && chmod 440 /etc/sudoers.d/admin-nopasswd"
s() { sudo -n "$@"; }
s pmset -a displaysleep 0 sleep 0 disksleep 0 powernap 0 >/dev/null
defaults -currentHost write com.apple.screensaver idleTime -int 0
defaults write com.apple.screensaver idleTime -int 0
defaults write com.apple.screensaver askForPassword -int 0
sysadminctl -screenLock off -password "$PW" 2>/dev/null || true
s softwareupdate --schedule off >/dev/null 2>&1 || true
s defaults write /Library/Preferences/com.apple.SoftwareUpdate AutomaticCheckEnabled -bool false
s defaults write /Library/Preferences/com.apple.SoftwareUpdate AutomaticDownload -bool false
s mdutil -a -i off >/dev/null 2>&1 || true
defaults write com.apple.CrashReporter DialogType -string none
mkdir -p ~/ReplayNES-test
if ! sysadminctl -autologin status 2>&1 | grep -qi "login user:"; then
  s sysadminctl -autologin set -userName "$U" -password "$PW" >/dev/null 2>&1 || true
  if ! s test -f /etc/kcpassword; then
    # sysadminctl fails over SSH ("SACSetAutoLoginPassword error:22"): write /etc/kcpassword ourselves
    # (the password XORed with Apple's fixed key, padded to 12-byte blocks) and name the user.
    KC="/tmp/kcp.$$"
    PW="$PW" KC="$KC" perl -e '@k=(0x7d,0x89,0x52,0x23,0xd2,0xbc,0xdd,0xea,0xa3,0xb9,0x1f);
      $p=$ENV{PW}; $n=(int(length($p)/12)+1)*12; $p.="\x00" x ($n-length($p));
      open F,">",$ENV{KC}; binmode F; print F map { chr(ord(substr($p,$_,1))^$k[$_%11]) } 0..$n-1; close F'
    s install -m 600 -o root -g wheel "$KC" /etc/kcpassword; rm -f "$KC"
    s defaults write /Library/Preferences/com.apple.loginwindow autoLoginUser "$U"
  fi
  echo "autologin-enabled"
fi
echo "console-user=$(stat -f %Su /dev/console)"
GUEST
CONSOLE="$(vm_ssh 'stat -f %Su /dev/console')"
if [ "$CONSOLE" != "$VM_USER" ]; then
  echo "== no desktop session yet (console: $CONSOLE); rebooting the guest once so auto-login applies"
  vm_ssh 'sudo -n shutdown -r now' >/dev/null 2>&1 || true
  sleep 15
  wait_ssh
  for _ in $(seq 1 60); do [ "$(vm_ssh 'stat -f %Su /dev/console')" = "$VM_USER" ] && break; sleep 2; done
  [ "$(vm_ssh 'stat -f %Su /dev/console')" = "$VM_USER" ] || { echo "guest never logged in" >&2; exit 1; }
fi
# Belt and braces for the disabled sound device: mute the guest's output in the desktop session.
vm_ssh 'osascript -e "set volume output volume 0" -e "set volume output muted true"' >/dev/null 2>&1 || true
echo "provisioned: $(vm_ssh 'sw_vers -productVersion'), desktop session for $VM_USER"
