#!/bin/bash
# PNG of the Windows VM's screen (nothing is captured from the host screen):
#   scripts/windows-vm/screenshot.sh [out.png]           `prlctl capture` (Parallels' framebuffer grab)
#   scripts/windows-vm/screenshot.sh --guest [out.png]   inside the guest (guest-capture.ps1, GDI
#       CopyFromScreen of the composed desktop): needed while a Direct3D flip-model window (the
#       ReplayNES frontend) is on screen - `prlctl capture` returns a black screen then.
# Default output: build/vm-shots/windows-<time>.png
set -euo pipefail
[ -n "${TRACE:-}" ] && set -x
. "$(dirname "$0")/common.sh"
need_running
GUEST=0
if [ "${1:-}" = --guest ]; then GUEST=1; shift; fi
OUT="${1:-$ROOT/build/vm-shots/windows-$(date +%Y%m%d-%H%M%S).png}"
mkdir -p "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
if [ "$GUEST" = 1 ]; then
  # Run hidden through wscript (run-hidden.vbs, copied to the guest: WSH does not run it from the
  # share): a console window would pop up over the desktop being captured.
  SRC="$(host_unc "$ROOT/scripts/windows-vm")"
  T='%TEMP%\replaynes-capture'
  win_cmd "(if not exist \"$T\" mkdir \"$T\") & (if exist \"$T\\shot.png\" del /q \"$T\\shot.png\") & copy /y \"$SRC\\run-hidden.vbs\" \"$T\" >nul & copy /y \"$SRC\\guest-capture.ps1\" \"$T\" >nul" >/dev/null
  TMPDIR_GUEST="$(win_cmd "echo %TEMP%" | tr -d '\r' | tail -1)\\replaynes-capture"
  prl_exec --current-user wscript.exe //Nologo "$TMPDIR_GUEST\\run-hidden.vbs" powershell.exe -NoProfile \
    -ExecutionPolicy Bypass -File "$TMPDIR_GUEST\\guest-capture.ps1" -Out "$TMPDIR_GUEST\\shot.png" >/dev/null || true
  win_cmd "copy /y \"$T\\shot.png\" \"$(host_unc "$OUT")\" >nul & del \"$T\\shot.png\"" >/dev/null || true
else
  "$PRLCTL" capture "$VM" --file "$OUT" >/dev/null
fi
[ -s "$OUT" ] || { echo "capture failed" >&2; exit 1; }
echo "$OUT"
