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

# Input model through the real controller / keyboard paths (--inject-pad / --inject-keys):
# L+R chord opens the menu, D-pad + A navigate, B goes back, L+R closes (resumes), R alone pauses
# and resumes, L alone toggles slow, Esc opens / closes.
input_check() {
  local dir="$OUT/input"
  mkdir -p "$dir/library/ROM" "$dir/session"
  local pad="3:leftShoulder:d,3.04:rightShoulder:d,3.2:leftShoulder:u,3.2:rightShoulder:u"
  pad="$pad,4.5:dpad.right:d,4.6:dpad.right:u,5:face.south:d,5.1:face.south:u,6.5:face.east:d,6.6:face.east:u"
  pad="$pad,7.5:rightShoulder:d,7.53:leftShoulder:d,7.7:leftShoulder:u,7.7:rightShoulder:u"
  pad="$pad,9:rightShoulder:d,9.05:rightShoulder:u,11:rightShoulder:d,11.05:rightShoulder:u,12:leftShoulder:d,12.4:leftShoulder:u"
  local snaps="4:$dir/a.png,6:$dir/b.png,7:$dir/c.png,8.5:$dir/d.png,10:$dir/e.png,11.8:$dir/f.png,13:$dir/g.png,15:$dir/h.png,17:$dir/i.png"
  "$BIN" --rom "$ROM" --autoplay --library-root "$dir/library" --session-root "$dir/session" --no-updater \
    --inject-pad "$pad" --inject-keys "14:53:d,14.05:53:u,16:53:d,16.05:53:u" --snapshot-at "$snaps" \
    --test-actions "0.5:screen:0,19:quit" -AppleLanguages "(en)" -ApplePersistenceIgnoreState YES >"$dir/app.log" 2>&1 || true
  python3 - "$dir" <<'PY' || fail=1
import json, sys
d = sys.argv[1]
want = {  # snapshot: (menuOpen, page, paused, slow)
    "a": (True, "top", True, "Normal"), "b": (True, "retry", True, "Normal"), "c": (True, "top", True, "Normal"),
    "d": (False, None, False, "Normal"), "e": (False, None, True, "Normal"), "f": (False, None, False, "Normal"),
    "g": (False, None, False, "1/2"), "h": (True, "top", True, "1/2"), "i": (False, None, False, "1/2"),
}
bad = []
for k, (menu, page, paused, slow) in want.items():
    j = json.load(open(f"{d}/{k}.json"))
    got = (j["menuOpen"], j["menuPage"] if menu else None, j["paused"], j["slow"])
    if got != (menu, page, paused, slow) or j["pillMetalVisible"] != (not menu and not paused):
        bad.append(f"{k}: got {got} pill {j['pillMetalVisible']}, want {(menu, page, paused, slow)}")
print(f"{d}: input model " + ("ok" if not bad else "FAILED\n  " + "\n  ".join(bad)))
sys.exit(1 if bad else 0)
PY
}
input_check

for lang in en ja; do
  for size in 1100x820 1280x800; do
    run "$lang" "$size" --rom "$ROM" --autoplay
  done
  if [ "${EMPTY:-0}" = "1" ]; then TAG=-empty run "$lang" 1280x800; fi
done
[ "$fail" = 0 ] && echo "ui-check: all menu pages fit" || { echo "ui-check: FAILED"; exit 1; }
