#!/bin/bash
# End-to-end check of the Syphon streaming output (needs a GUI login session, no OBS):
# launches ReplayNES with the generated test ROM and --syphon, then a headless Syphon client
# (scripts/syphon-check.swift) must find the "ReplayNES" server and receive frames of the
# configured size. Two configurations are checked: 4x (1024x960) and 1280x960 with 8:7 (bars).
#   scripts/check-syphon-macos.sh [path/to/ReplayNES.app]   (default: build/ReplayNES.app)
# Requires a built app (scripts/build-macos.sh) and build/tools/replaynes-cli/replaynes-cli.
# The settings are passed through the volatile argument domain (-syphonSize ...) and the library
# root points to a temp dir, so the user's preferences and ~/Documents are not touched.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-$ROOT/build/ReplayNES.app}"
BIN="$APP/Contents/MacOS/ReplayNES"
CLI="$ROOT/build/tools/replaynes-cli/replaynes-cli"
WORK="$ROOT/build/syphon-check"
[ -x "$BIN" ] || { echo "app not found: $APP"; exit 1; }
[ -x "$CLI" ] || { echo "replaynes-cli not found: $CLI (cmake --build build)"; exit 1; }
rm -rf "$WORK" && mkdir -p "$WORK/library"

"$CLI" make-test-rom "$WORK/test.nes" >/dev/null
# The embedded copy has no headers/module map: compile against the build product, run against
# the framework inside the app.
FW="$APP/Contents/Frameworks"
HDR=""
for d in "$(dirname "$APP")" "$ROOT/build/DerivedData-release/Build/Products/Release" "$ROOT/build/DerivedData/Build/Products/Debug"; do
  if [ -d "$d/Syphon.framework/Modules" ]; then HDR="$d"; break; fi
done
[ -n "$HDR" ] || { echo "Syphon.framework with module map not found (build the app first)"; exit 1; }
xcrun swiftc -O -F "$HDR" -framework Syphon -Xlinker -rpath -Xlinker "$FW" \
  "$ROOT/scripts/syphon-check.swift" -o "$WORK/syphon-check"

run_case() { # <syphonSize> <par87 YES|NO> <w> <h> <pillar>
  local size="$1" par="$2" w="$3" h="$4" pillar="$5"
  echo "==> case $size par87=$par (expect ${w}x${h})"
  "$BIN" --rom "$WORK/test.nes" --autoplay --syphon --library-root "$WORK/library" \
    --snapshot "$WORK/snapshot-$size.png" --snapshot-delay 25 --quit-after-snapshot \
    -syphonSize "$size" -syphonPAR87 "$par" >"$WORK/app-$size.log" 2>&1 &
  local pid=$!
  local rc=0
  "$WORK/syphon-check" "$w" "$h" "$pillar" || rc=$?
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  return $rc
}

run_case x4 NO 1024 960 0
# 8:7 inside 1280x960: picture 1170x960 at x=55 -> 55 px black bars on both sides.
run_case w1280 YES 1280 960 55
echo "==> Syphon output OK"
