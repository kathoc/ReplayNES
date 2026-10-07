# Shared settings for scripts/windows-vm/*.sh (sourced, not run). See docs/WINDOWS.md.
# Parallels Desktop backend for the developer's existing Windows 11 (on ARM) VM. Host -> guest
# access is `prlctl exec` (Parallels Tools): no SSH server, no extra software in the guest. The
# VM is the user's everyday/gaming VM: these scripts never change its settings or snapshots.

VM="${WINDOWS_VM:-Windows 11}"
PRLCTL="${PRLCTL:-$(command -v prlctl || echo /usr/local/bin/prlctl)}"
[ -x "$PRLCTL" ] || { echo "prlctl not found (Parallels Desktop required, see docs/WINDOWS.md)" >&2; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Guest working directory (inside the logged-in user's profile; delete it to undo everything).
GUEST_DIR='%USERPROFILE%\ReplayNES-dev'

vm_exists() { "$PRLCTL" list -a -o name 2>/dev/null | awk -v n="$VM" 'NR>1 && $0==n {f=1} END{exit !f}'; }
vm_status() { "$PRLCTL" list -a -o status,name 2>/dev/null | awk -v n="$VM" 'NR>1 { s=$1; $1=""; sub(/^ +/,""); if ($0==n) print s }'; }
vm_running() { [ "$(vm_status)" = running ]; }

# `prlctl exec` with the guest's exit code. It occasionally fails with "PrlJob_GetResult: /
# PrlJob_GetRetCode: Invalid argument" (exit 255, the guest result is lost); that case is
# re-run, so commands must be idempotent. Output is buffered.
prl_exec() {
  local out rc i
  for i in 1 2 3 4 5; do
    out="$("$PRLCTL" exec "$VM" "$@" 2>&1)" && rc=0 || rc=$?
    [ "$rc" = 255 ] && [[ "$out" == *PrlJob_Get* ]] && { sleep 2; continue; }
    break
  done
  [ -n "$out" ] && printf '%s\n' "$out"
  return "$rc"
}
# Run a cmd.exe command line in the logged-in user's desktop session (exit code is forwarded).
# UTF-8 console output (the guest may use a non-UTF-8 OEM code page, e.g. 932).
win_cmd() { prl_exec --current-user cmd /d /s /c "chcp 65001 >nul & $*"; }
# Same, as NT AUTHORITY\SYSTEM (works before anyone logs in; no access to the Mac shares).
win_cmd_system() { prl_exec cmd /d /s /c "chcp 65001 >nul & $*"; }
# PowerShell script text, passed as -EncodedCommand (UTF-16LE base64) so quoting survives intact.
win_ps() {
  local enc
  enc="$(printf '%s' "[Console]::OutputEncoding=[Text.Encoding]::UTF8; $*" | iconv -f UTF-8 -t UTF-16LE | base64 | tr -d '\n')"
  prl_exec --current-user powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand "$enc"
}

need_running() { vm_running || { echo "$VM is not running (scripts/windows-vm/start.sh)" >&2; exit 1; }; }

# Waits until a user is logged in (prlctl exec --current-user needs a desktop session).
wait_session() {
  local i
  for i in $(seq 1 "${TIMEOUT:-180}"); do
    "$PRLCTL" exec "$VM" --current-user cmd /c exit 0 >/dev/null 2>&1 && return 0
    sleep 1
  done
  echo "no logged-in user session in $VM (log in once in the VM window)" >&2; return 1
}

# The host repo as seen from the guest: Parallels shares the Mac home folder as \\Mac\Home.
host_unc() {   # host_unc <absolute host path under $HOME>
  local p="$1"
  case "$p" in "$HOME"/*) ;; *) echo "$p is not under \$HOME (not visible as \\\\Mac\\Home)" >&2; return 1 ;; esac
  p="${p#"$HOME"/}"
  printf '%s\n' "\\\\Mac\\Home\\${p//\//\\}"
}
