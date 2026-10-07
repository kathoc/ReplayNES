#!/bin/bash
# ctest equivalent inside the Windows VM: deploys build/windows-<arch> (deploy.sh) and runs every
# test executable there. aarch64 runs natively on the ARM VM; x86_64 runs under Windows' x64
# emulation (Prism). Scratch files go to %TEMP%\replaynes-tests-<arch> (RN_TEST_TMP).
#   scripts/windows-vm/run-tests.sh [x86_64|aarch64|all] [test-name-substring]
# Exit status: number of failed test executables (0 = all passed). Logs: build/windows-<arch>/vm-test-logs/.
set -euo pipefail
. "$(dirname "$0")/common.sh"
need_running
ARCHS="${1:-all}"
FILTER="${2:-}"
[ "$ARCHS" = all ] && ARCHS="$(for a in x86_64 aarch64; do [ -d "$ROOT/build/windows-$a" ] && echo "$a"; done)"
"$(dirname "$0")/deploy.sh" "${1:-all}"
failed=0
for arch in $ARCHS; do
  B="$ROOT/build/windows-$arch"
  LOGS="$B/vm-test-logs"
  rm -rf "$LOGS"; mkdir -p "$LOGS"
  TMP="%TEMP%\\replaynes-tests-$arch"
  win_cmd "(if exist \"$TMP\" rmdir /s /q \"$TMP\") & mkdir \"$TMP\"" >/dev/null
  tests=()
  for f in "$B"/tests/test_*.exe; do
    n="$(basename "$f" .exe)"
    [ -z "$FILTER" ] || [[ "$n" == *"$FILTER"* ]] && tests+=("$n")
  done
  echo "==> $arch: ${#tests[@]} test executables"
  i=0; nfail=0; t0=$SECONDS
  for n in "${tests[@]}"; do
    i=$((i + 1)); s=$SECONDS
    if win_cmd "set RN_TEST_TMP=$TMP& cd /d \"$GUEST_DIR\\$arch\" & $n.exe" >"$LOGS/$n.log" 2>&1; then r=Passed; else r="***Failed"; nfail=$((nfail + 1)); fi
    printf '%3d/%d %-28s %-10s %4ds\n' "$i" "${#tests[@]}" "$n" "$r" $((SECONDS - s))
    [ "$r" = Passed ] || { grep -E 'FAILED|\[FAIL\]|PrlJob' "$LOGS/$n.log" || true; } | head -5 | sed 's/^/      /'
  done
  echo "$arch: $(( ${#tests[@]} - nfail ))/${#tests[@]} passed in $((SECONDS - t0))s (logs: $LOGS)"
  failed=$((failed + nfail))
done
exit "$failed"
