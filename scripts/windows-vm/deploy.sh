#!/bin/bash
# Copy the cross-built Windows binaries (scripts/build-windows.sh) into the VM:
#   build/windows-<arch>/{tests,tools/*,apps/windows}/*.exe  ->  %USERPROFILE%\ReplayNES-dev\<arch>\
# (apps/windows: the frontend ReplayNES.exe, statically linked - no DLLs)
# The guest pulls them from the Mac home share (\\Mac\Home, Parallels shared folders) with
# robocopy, so the repo must live under $HOME. ROMs are never copied.
#   scripts/windows-vm/deploy.sh [x86_64|aarch64|all]   (default: all built archs)
set -euo pipefail
. "$(dirname "$0")/common.sh"
need_running
ARCHS="${1:-all}"
[ "$ARCHS" = all ] && ARCHS="$(for a in x86_64 aarch64; do [ -d "$ROOT/build/windows-$a" ] && echo "$a"; done)"
[ -n "$ARCHS" ] || { echo "nothing built (scripts/build-windows.sh)" >&2; exit 1; }
for arch in $ARCHS; do
  B="$ROOT/build/windows-$arch"
  [ -f "$B/tests/test_core.exe" ] || { echo "$B not built (scripts/build-windows.sh $arch)" >&2; exit 1; }
  SRC="$(host_unc "$B")"
  DST="$GUEST_DIR\\$arch"
  # A running ReplayNES.exe would lock its file (robocopy then retries for a long time).
  cmds="(taskkill /im ReplayNES.exe /f >nul 2>&1) & (if not exist \"$DST\" mkdir \"$DST\")"
  for sub in tests tools\\replaynes-cli tools\\windows-probe apps\\windows; do
    # robocopy: exit codes < 8 are success; /NJH /NJS /NFL /NDL keep it quiet.
    cmds+=" & robocopy \"$SRC\\$sub\" \"$DST\" *.exe /R:3 /W:1 /NJH /NJS /NFL /NDL /NP >nul & (if errorlevel 8 exit /b 8)"
  done
  n="$(win_cmd "$cmds & dir /b \"$DST\\*.exe\" | find /c \".exe\"" | tr -d '\r ')"
  printf '%s: %s executables in %s\n' "$arch" "$n" "$DST"
done
