#!/bin/bash
# UI check of the macOS app (needs a GUI login session): walks every Quick Menu page, the paused
# seek bar and the library in English and Japanese, at the default window size and at 1280x800,
# with the app's own snapshots (--menu-walk, TestHooks.swift; no screen capture). Fails when a
# menu page does not fit its window without scrolling (docs/design/UI_REDESIGN.md: no page ever
# scrolls). The PNGs are for looking at: build/ui-check/<lang>-<size>/NN-<page>.png
#   scripts/ui-check-macos.sh [app]          (default build/ReplayNES.app; generated test ROM)
#   EMPTY=1 ...                              also the empty library (no game open)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-$ROOT/build/ReplayNES.app}"
BIN="$APP/Contents/MacOS/ReplayNES"
CLI="$ROOT/build/tools/replaynes-cli/replaynes-cli"
OUT="${OUT:-$ROOT/build/ui-check}"
[ -x "$BIN" ] || { echo "app not found: $APP"; exit 1; }
[ -x "$CLI" ] || { echo "replaynes-cli not found: $CLI (cmake --build build)"; exit 1; }
rm -rf "$OUT" && mkdir -p "$OUT"
ROM="$OUT/test.nes"
"$CLI" make-test-rom "$ROM" >/dev/null

fail=0
run() {  # lang size(WxH) extra-args...
  local lang="$1" size="$2"; shift 2
  local dir="$OUT/$lang-$size${TAG:-}"
  mkdir -p "$dir/library/ROM" "$dir/session"
  "$BIN" "$@" --library-root "$dir/library" --session-root "$dir/session" --no-updater \
    --test-actions "0.5:screen:0,0.8:windowSize:$size" --menu-walk "$dir" --menu-walk-delay 4 --quit-after-snapshot \
    -AppleLanguages "($lang)" -ApplePersistenceIgnoreState YES -integerScale YES -crtEnabled NO >"$dir/app.log" 2>&1 || true
  python3 - "$dir" <<'PY' || fail=1
import json, sys
d = sys.argv[1]
pages = json.load(open(d + "/menu-walk.json"))["pages"]
bad = [p["name"] for p in pages if p.get("menuOpen") and not p.get("fits")]
print(f"{d}: {len(pages)} screens, {len(bad)} not fitting" + (": " + ", ".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
PY
}

for lang in en ja; do
  for size in 1100x820 1280x800; do
    run "$lang" "$size" --rom "$ROM" --autoplay
  done
  if [ "${EMPTY:-0}" = "1" ]; then TAG=-empty run "$lang" 1280x800; fi
done
[ "$fail" = 0 ] && echo "ui-check: all menu pages fit" || { echo "ui-check: FAILED"; exit 1; }
