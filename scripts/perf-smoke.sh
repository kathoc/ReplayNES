#!/bin/bash
# Frame pacing smoke test (needs a GUI login session): runs ReplayNES recording a ROM for a while
# (temporary session with autosave, filmstrip growing) and reports how steadily frames are
# emulated and presented, from the app's own counters (--stats-log, FramePacing.swift).
#   scripts/perf-smoke.sh [app] [seconds] [rom]     (defaults: build/ReplayNES.app, 60, generated test ROM)
#   FULLSCREEN=1 scripts/perf-smoke.sh ...          same in full screen (FILL)
#   STRICT=1 ...                                    exit 1 when a threshold is missed (see below)
#   ACTIVATE=1 ...                                  activate the app (default: it runs behind the front app)
#   WARMUP=150 ...                                  start measuring later (long take: big filmstrip)
# Everything goes to build/perf-smoke (library / session roots, logs); the user's preferences
# are only overridden through the volatile argument domain (-integerScale NO ...).
# Output: emulated fps, presented fps, judder (frames held an extra refresh) per minute, frames
# never shown, late renders, late emulation ticks, main-thread / process CPU.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-$ROOT/build/ReplayNES.app}"
DUR="${2:-60}"
ROM="${3:-}"
BIN="$APP/Contents/MacOS/ReplayNES"
CLI="$ROOT/build/tools/replaynes-cli/replaynes-cli"
WORK="$ROOT/build/perf-smoke"
WARMUP="${WARMUP:-8}"
[ -x "$BIN" ] || { echo "app not found: $APP"; exit 1; }
rm -rf "$WORK" && mkdir -p "$WORK/library" "$WORK/session"
if [ -z "$ROM" ]; then
  [ -x "$CLI" ] || { echo "replaynes-cli not found: $CLI (cmake --build build), or pass a ROM"; exit 1; }
  ROM="$WORK/test.nes"
  "$CLI" make-test-rom "$ROM" >/dev/null
fi
ACTS="1:fill"
[ "${FULLSCREEN:-0}" = "1" ] && ACTS="$ACTS,2:fullscreen"

"$BIN" --rom "$ROM" --autoplay --library-root "$WORK/library" --session-root "$WORK/session" --no-updater \
  --stats-log "$WORK/stats.jsonl" --test-actions "$ACTS" \
  -integerScale NO -sidebarVisible NO -showLatency NO -ApplePersistenceIgnoreState YES ${EXTRA_ARGS:-} >"$WORK/app.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
sleep 1
# ACTIVATE=1: bring the launched instance to the front (it then also receives controller input).
[ "${ACTIVATE:-0}" = "1" ] && { open "$APP" 2>/dev/null || true; }

# Thread CPU: `ps -M` lists the main thread first.
: >"$WORK/cpu.txt"
END=$((SECONDS + WARMUP + DUR))
sleep "$WARMUP"
while [ $SECONDS -lt $END ]; do
  sleep 5
  ps -M -p "$PID" | awk -v t=$SECONDS 'NR==2 {main=$4; tot+=$4} NR>2 {tot+=$2} END {print t, main, tot}' >>"$WORK/cpu.txt" || true
done
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
trap - EXIT

python3 - "$WORK" "$WARMUP" <<'PY'
import json, os, sys
work, warm = sys.argv[1], float(sys.argv[2])
rows = [json.loads(l) for l in open(os.path.join(work, "stats.jsonl")) if l.strip()]
rows = [r for r in rows if r["t"] >= warm and not r["paused"]]
if len(rows) < 3: print("not enough samples (app paused or not running?)"); sys.exit(1)
a, b = rows[0], rows[-1]
dt = b["t"] - a["t"]; mins = dt / 60
d = lambda k: b[k] - a[k]
emu = sum(r["emulatedFPS"] for r in rows[1:]) / (len(rows) - 1)  # engine-side 1 s windows
pres = d("presentCount") / dt
cpu = [l.split() for l in open(os.path.join(work, "cpu.txt")) if l.strip()]
main_cpu = sum(float(c[1]) for c in cpu) / max(1, len(cpu))
proc_cpu = sum(float(c[2]) for c in cpu) / max(1, len(cpu))
print(f"window {dt:.0f}s  fullscreen={b['fullScreen']}  frames {a['frame']}->{b['frame']}")
print(f"emulated fps        {emu:8.3f}   (target 60.099)")
print(f"presented fps       {pres:8.3f}")
print(f"judder / min        {d('presentHitches')/mins:8.1f}   (new frame shown > 1.5 periods after the previous)")
print(f"never shown / min   {d('skippedFrames')/mins:8.1f}")
print(f"late renders / min  {d('drawLate')/mins:8.1f}   (present thread > 1.5 frame periods apart)")
print(f"present lead        {b.get('presentLeadMs', 0):8.1f} ms (frames appear this long after their tick)")
print(f"emulated->display   {b['emulatedToPresentMs']:8.1f} ms")
print(f"late ticks / min    {d('tickWakeLate')/mins:8.1f}   (emulation tick > 4 ms late); stalls {d('lateTicks')}")
print(f"audio underruns     {d('audioUnderruns'):8d}")
print(f"main thread CPU     {main_cpu:8.1f} %   process {proc_cpu:.1f} %")
bad = []
if abs(emu - 60.0988) > 0.05: bad.append("emulated fps")
if abs(pres - emu) > 0.5: bad.append("presented fps")
if d("lateTicks") > 0: bad.append("stalled ticks")
if d("presentHitches") / mins > 30: bad.append("judder")
print("RESULT", "OK" if not bad else "WARN: " + ", ".join(bad))
if bad and os.environ.get("STRICT") == "1": sys.exit(1)
PY
