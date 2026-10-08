#!/bin/bash
# UI check of the macOS app: walks every Quick Menu page, the paused seek bar and the library in
# English and Japanese, at the default window size and at 1280x800, with the app's own snapshots
# (--menu-walk, TestHooks.swift; no screen capture), and drives the input model through the real
# controller / keyboard paths (--inject-pad / --inject-keys). Fails when a menu page does not fit
# its window without scrolling (docs/design/UI_REDESIGN.md: no page ever scrolls) or an input step
# ends in the wrong state. The PNGs are for looking at: build/ui-check/<run>/NN-<page>.png
#
# By default everything runs inside the macOS VM (scripts/macos-vm/, Parallels; docs/MACOS_VM.md):
# the app is deployed there, launched in the guest's desktop session over ssh, and the snapshots /
# logs are copied back. Nothing is launched on this Mac. The VM must be running
# (scripts/macos-vm/reset.sh && scripts/macos-vm/start.sh); stop it afterwards (stop.sh).
#   scripts/ui-check-macos.sh [app]          (default build/ReplayNES.app; generated test ROM)
#   EMPTY=1 ...                              also the empty library (no game open)
#   ONLY="input flows" ...                   a subset: input, flows (pad flows), walk
#   LOCAL=1 ...                              run the app on this Mac instead (needs a GUI login session;
#                                            no flows by default: they change the bindings / settings)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-$ROOT/build/ReplayNES.app}"
CLI="$ROOT/build/tools/replaynes-cli/replaynes-cli"
OUT="${OUT:-$ROOT/build/ui-check}"
if [ "${LOCAL:-0}" = 1 ]; then ONLY="${ONLY:-input walk}"; else ONLY="${ONLY:-input flows walk}"; fi
[ -d "$APP" ] || { echo "app not found: $APP"; exit 1; }
[ -x "$CLI" ] || { echo "replaynes-cli not found: $CLI (cmake --build build)"; exit 1; }
rm -rf "$OUT" && mkdir -p "$OUT"
"$CLI" make-test-rom "$OUT/test.nes" >/dev/null

if [ "${LOCAL:-0}" = 1 ]; then
  BIN="$APP/Contents/MacOS/ReplayNES"
  G="$OUT"                       # where the app writes (same tree here)
  ROM="$OUT/test.nes"
  # app <run name> <args...>: runs the app to completion; its output in <run>/app.log.
  app() { local d="$G/$1"; shift; mkdir -p "$d/library/ROM" "$d/session"; "$BIN" "$@" >"$d/app.log" 2>&1 || true; }
  fetch() { :; }
else
  . "$ROOT/scripts/macos-vm/common.sh"
  vm_running || { echo "$VM is not running (scripts/macos-vm/reset.sh && scripts/macos-vm/start.sh)" >&2; exit 1; }
  "$ROOT/scripts/macos-vm/deploy.sh" "$APP" "$OUT/test.nes" >/dev/null
  G="/Users/$VM_USER/ReplayNES-test/ui-check"
  ROM="/Users/$VM_USER/ReplayNES-test/test.nes"
  vm_ssh "rm -rf $G && mkdir -p $G"
  # The app runs in the guest's desktop session (LaunchServices); `open -W` waits until it quits.
  app() {
    local d="$G/$1"; shift
    local q=""; for a in "$@"; do q+=" $(printf %q "$a")"; done
    # Every run starts from the default bindings and settings (the flows change them).
    vm_ssh "mkdir -p '$d/library/ROM' '$d/session'; pkill -x ReplayNES 2>/dev/null; sleep 0.5
      rm -f ~/Library/Application\\ Support/ReplayNES/bindings.json; defaults delete io.github.replaynes.ReplayNES >/dev/null 2>&1
      ( sleep ${TIMEOUT:-120}; pkill -f -- '$d/session' ) >/dev/null 2>&1 &
      open -W -n --stdout '$d/app.log' --stderr '$d/app.log' -a /Applications/ReplayNES.app --args$q; true"
  }
  fetch() { mkdir -p "$OUT/$1"; vm_scp_from "$G/$1/" "$OUT/$1/"; }
  vm_scp_from() { local ip; ip="$(vm_ip)"; scp -q -r "${SSH_OPTS[@]}" -o BatchMode=yes "$VM_USER@$ip:$1." "$2"; }
fi
want() { case " $ONLY " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

fail=0
COMMON=(--no-updater -ApplePersistenceIgnoreState YES)

walk() {  # lang size(WxH) extra-args...
  local lang="$1" size="$2"; shift 2
  local name="$lang-$size${TAG:-}"
  app "$name" "$@" --library-root "$G/$name/library" --session-root "$G/$name/session" "${COMMON[@]}" \
    --test-actions "0.5:screen:0,0.8:windowSize:$size" --menu-walk "$G/$name" --menu-walk-delay 4 --quit-after-snapshot \
    -AppleLanguages "($lang)" -integerScale YES -crtEnabled NO
  fetch "$name"
  python3 - "$OUT/$name" <<'PY' || fail=1
import json, sys
d = sys.argv[1]
pages = json.load(open(d + "/menu-walk.json"))["pages"]
bad = [p["name"] for p in pages if p.get("menuOpen") and not p.get("fits")]
print(f"{d}: {len(pages)} screens, {len(bad)} not fitting" + (": " + ", ".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
PY
}

# Input model through the real controller / keyboard paths (--inject-pad / --inject-keys), menus
# confirming with east and going back with south (the defaults): L+R opens the menu, D-pad + east
# open a page, south goes back, L+R closes (resumes); R alone (released) pauses, R while paused
# steps a frame (stays paused), south tapped resumes, L alone toggles slow; Esc opens / closes;
# while paused, south held through a D-pad frame step does not resume, a tap does.
input_check() {
  local name="input"
  local pad="3:leftShoulder:d,3.04:rightShoulder:d,3.2:leftShoulder:u,3.2:rightShoulder:u"
  pad="$pad,4.5:dpad.right:d,4.6:dpad.right:u,5:face.east:d,5.1:face.east:u,6.5:face.south:d,6.6:face.south:u"
  pad="$pad,7.5:rightShoulder:d,7.53:leftShoulder:d,7.7:leftShoulder:u,7.7:rightShoulder:u"
  pad="$pad,9:rightShoulder:d,9.05:rightShoulder:u,11:rightShoulder:d,11.05:rightShoulder:u,11.8:face.south:d,11.85:face.south:u"
  pad="$pad,12:leftShoulder:d,12.4:leftShoulder:u"
  pad="$pad,17.5:rightShoulder:d,17.55:rightShoulder:u,18.5:face.south:d,18.6:dpad.right:d,18.7:dpad.right:u,18.9:face.south:u"
  pad="$pad,20:face.south:d,20.1:face.south:u"
  local d="$G/$name"
  local snaps="4:$d/a.png,6:$d/b.png,7:$d/c.png,8.5:$d/d.png,10:$d/e.png,11.5:$d/f.png,13:$d/g.png,15:$d/h.png,17:$d/i.png,19.5:$d/j.png,21:$d/k.png"
  app "$name" --rom "$ROM" --autoplay --library-root "$d/library" --session-root "$d/session" "${COMMON[@]}" \
    --inject-pad "$pad" --inject-keys "14:53:d,14.05:53:u,16:53:d,16.05:53:u" --snapshot-at "$snaps" \
    --test-actions "0.5:screen:0,2:dumpMenus,22:quit" -AppleLanguages "(en)"
  fetch "$name"
  # Close (⌘W) lives in the Window menu (the File menu is gone).
  grep -q "menu: Window | Close | ⌘w" "$OUT/$name/app.log" && echo "$OUT/$name: ⌘W close ok" || { echo "$OUT/$name: ⌘W close missing"; fail=1; }
  python3 - "$OUT/$name" <<'PY' || fail=1
import json, sys
d = sys.argv[1]
want = {  # snapshot: (menuOpen, page, paused, slow)
    "a": (True, "top", True, "Normal"), "b": (True, "retry", True, "Normal"), "c": (True, "top", True, "Normal"),
    "d": (False, None, False, "Normal"), "e": (False, None, True, "Normal"), "f": (False, None, True, "Normal"),
    "g": (False, None, False, "1/2"), "h": (True, "top", True, "1/2"), "i": (False, None, False, "1/2"),
    "j": (False, None, True, "1/2"), "k": (False, None, False, "1/2"),
}
bad, skipped = [], []
J = {k: json.load(open(f"{d}/{k}.json")) for k in want}
for k, (menu, page, paused, slow) in want.items():
    j = J[k]
    # h / i need the Esc key: injected keys only reach the app while it is frontmost (macOS does not
    # let an app launched from a background shell activate itself).
    if k in ("h", "i") and not j.get("keyboardEnabled", True):
        skipped.append(k)
        continue
    got = (j["menuOpen"], j["menuPage"] if menu else None, j["paused"], j["slow"])
    if got != (menu, page, paused, slow) or j["pillMetalVisible"] != (not menu and not paused):
        bad.append(f"{k}: got {got} pill {j['pillMetalVisible']}, want {(menu, page, paused, slow)}")
# R released while paused: one frame forward (the cursor moved by exactly one).
if J["f"]["statusFrame"] != J["e"]["statusFrame"] + 1:
    bad.append(f"R step: frame {J['e']['statusFrame']} -> {J['f']['statusFrame']}, want +1")
print(f"{d}: input model " + ("ok" if not bad else "FAILED\n  " + "\n  ".join(bad))
      + (f" (Esc steps {', '.join(skipped)} skipped: the app was not frontmost)" if skipped else ""))
sys.exit(1 if bad else 0)
PY
}

# Controller-only flows (--inject-pad, a listed test pad: --fake-controller):
#  controller: Settings › Controls › Controller, the D-pad moves the focus between the buttons on
#   the picture, east opens the action picker (focus on the current action), R = next sheet, east
#   assigns and returns, X = next pad, south goes back; the diagram of each family.
#  markers: pause, R / L step; drop two markers (east, hold R, east), ↑ focuses one, east edits,
#   D-pad / L held move it, east commits, X deletes one, south resumes.
flows() {
  local lang
  for lang in en ja; do
    local name="controller-$lang" d="$G/controller-$lang"
    # Generous gaps: the VM is slow and every snapshot holds the main thread for a moment.
    local pad="5:dpad.down:d,5.1:dpad.down:u,8:face.east:d,8.1:face.east:u,11:rightShoulder:d,11.05:rightShoulder:u"
    pad="$pad,14:dpad.down:d,14.1:dpad.down:u,14.6:face.east:d,14.7:face.east:u,17:face.west:d,17.1:face.west:u"
    pad="$pad,20:face.west:d,20.1:face.west:u,20.6:face.south:d,20.7:face.south:u"
    local snaps="4:$d/c0.png,7:$d/c1.png,10:$d/c2.png,13:$d/c3.png,16:$d/c4.png,19:$d/c5.png,22:$d/c6.png"
    snaps="$snaps,25:$d/fam-nintendo.png,28:$d/fam-xbox.png,31:$d/fam-playStation.png,34:$d/fam-steamDeck.png,37:$d/fam-generic.png"
    app "$name" --rom "$ROM" --autoplay --library-root "$d/library" --session-root "$d/session" "${COMMON[@]}" \
      --fake-controller xbox --inject-pad "$pad" --snapshot-at "$snaps" -AppleLanguages "($lang)" \
      --test-actions "0.5:screen:0,0.8:windowSize:1280x800,2:menu:controller,23:menu:controller,23.3:diagramFamily:nintendo,26.3:diagramFamily:xbox,29.3:diagramFamily:playStation,32.3:diagramFamily:steamDeck,35.3:diagramFamily:generic,38.5:diagramFamily:auto,39:quit"
    fetch "$name"
    python3 - "$OUT/$name" <<'PY' || fail=1
import json, sys
d = sys.argv[1]
J = {k: json.load(open(f"{d}/{k}.json")) for k in ["c0", "c1", "c2", "c3", "c4", "c5", "c6"]}
bad = []
def check(cond, msg):
    if not cond: bad.append(msg)
check(J["c0"]["menuPage"] == "controller", f"c0 page {J['c0']['menuPage']}")
check(J["c1"]["controllerFocus"] != J["c0"]["controllerFocus"], f"focus did not move: {J['c1']['controllerFocus']}")
check(J["c2"]["menuPage"] == "assign" and J["c2"]["assignElement"] == "gc0:" + J["c1"]["controllerFocus"],
      f"c2 picker: {J['c2']['menuPage']} {J['c2']['assignElement']}")
check(J["c3"]["menuPage"] == "assign" and J["c3"]["menuListPage"] != J["c2"]["menuListPage"],
      f"R did not switch the sheet: {J['c2']['menuListPage']} -> {J['c3']['menuListPage']}")
check(J["c4"]["menuPage"] == "controller", f"c4 back on the diagram: {J['c4']['menuPage']}")
check(J["c4"]["controllerFocusActions"] != J["c1"]["controllerFocusActions"], f"assignment unchanged: {J['c4']['controllerFocusActions']}")
check(J["c5"]["menuListPage"] == 1, f"X: next pad {J['c5']['menuListPage']}")
check(J["c6"]["menuPage"] == "controls", f"south: back to Controls ({J['c6']['menuPage']})")
print(f"{d}: controller flow " + ("ok" if not bad else "FAILED\n  " + "\n  ".join(bad)))
sys.exit(1 if bad else 0)
PY

    name="markers-$lang"; d="$G/markers-$lang"
    pad="3:rightShoulder:d,3.05:rightShoulder:u,5.5:rightShoulder:d,5.55:rightShoulder:u,8:leftShoulder:d,8.05:leftShoulder:u"
    pad="$pad,10.5:face.east:d,10.55:face.east:u,13:rightShoulder:d,14:rightShoulder:u,15:face.east:d,15.05:face.east:u"
    pad="$pad,17.5:dpad.up:d,17.55:dpad.up:u,20:face.east:d,20.05:face.east:u,22.5:dpad.left:d,22.55:dpad.left:u"
    pad="$pad,25:leftShoulder:d,26.1:leftShoulder:u,28:face.east:d,28.05:face.east:u,30.5:dpad.up:d,30.55:dpad.up:u"
    pad="$pad,33:face.west:d,33.05:face.west:u,35.5:face.south:d,35.55:face.south:u,38:face.south:d,38.05:face.south:u"
    snaps="4.3:$d/m0.png,6.8:$d/m1.png,9.3:$d/m2.png,11.8:$d/m3.png,16.3:$d/m4.png,18.8:$d/m5.png,21.3:$d/m6.png,23.8:$d/m7.png"
    snaps="$snaps,26.8:$d/m8.png,29.3:$d/m9.png,31.8:$d/m10.png,34.3:$d/m11.png,36.8:$d/m12.png,39.3:$d/m13.png"
    app "$name" --rom "$ROM" --autoplay --library-root "$d/library" --session-root "$d/session" "${COMMON[@]}" \
      --fake-controller xbox --inject-pad "$pad" --snapshot-at "$snaps" -AppleLanguages "($lang)" \
      --test-actions "0.5:screen:0,0.8:windowSize:1280x800,40.5:quit"
    fetch "$name"
    python3 - "$OUT/$name" <<'PY' || fail=1
import json, sys
d = sys.argv[1]
J = {k: json.load(open(f"{d}/m{k}.json")) for k in range(14)}
bad = []
def check(cond, msg):
    if not cond: bad.append(msg)
f = lambda k: J[k]["statusFrame"]
slot0 = lambda k: next((s for s in J[k]["practiceSlots"] if s["slot"] == 0), None)
check(J[0]["paused"], "R did not pause")
check(J[1]["paused"] and f(1) == f(0) + 1, f"R step: {f(0)} -> {f(1)}")
check(J[2]["paused"] and f(2) == f(1) - 1, f"L step: {f(1)} -> {f(2)}")
check(J[3]["markers"] == [f(2)] and slot0(3) and not slot0(3)["hasB"], f"first marker: {J[3]['markers']} slot {slot0(3)}")
check(f(4) > f(3) + 5, f"R held: repeated steps {f(3)} -> {f(4)}")
check(len(J[4]["markers"]) == 2 and slot0(4) and slot0(4)["hasB"], f"second marker: {J[4]['markers']} slot {slot0(4)}")
check(J[5]["markerFocus"] >= 0 and not J[5]["markerEditing"], f"up: focus {J[5]['markerFocus']}")
check(J[6]["markerEditing"], "east: editing")
check(J[7]["markers"] != J[6]["markers"], f"D-pad left moved nothing: {J[6]['markers']} {J[7]['markers']}")
check(J[8]["markerEditing"] and J[8]["markers"] != J[7]["markers"], f"L held moved nothing: {J[7]['markers']} {J[8]['markers']}")
check(not J[9]["markerEditing"] and J[9]["markerFocus"] == -1 and slot0(9) and slot0(9)["hasB"], f"commit: {J[9]['markers']} {slot0(9)}")
check(J[10]["markerFocus"] >= 0, "up again: focus")
check(len(J[11]["markers"]) == 1 and slot0(11) and not slot0(11)["hasB"], f"X delete: {J[11]['markers']} {slot0(11)}")
check(J[12]["paused"], "south on a marker goes back to the bar (still paused)")
check(not J[13]["paused"], "south tapped on the bar resumes")
print(f"{d}: markers flow " + ("ok" if not bad else "FAILED\n  " + "\n  ".join(bad)))
sys.exit(1 if bad else 0)
PY
  done
}

want input && input_check
want flows && flows
if want walk; then
  for lang in en ja; do
    for size in 1100x820 1280x800; do
      walk "$lang" "$size" --rom "$ROM" --autoplay
    done
    if [ "${EMPTY:-0}" = "1" ]; then TAG=-empty walk "$lang" 1280x800; fi
  done
fi
[ "$fail" = 0 ] && echo "ui-check: all checks passed" || { echo "ui-check: FAILED"; exit 1; }
