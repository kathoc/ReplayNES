#!/bin/bash
# Frame pacing / latency measurement (needs a GUI login session; keep the display awake, e.g. run
# under `caffeinate -d`): runs ReplayNES recording a ROM for a while (temporary session with
# autosave, filmstrip growing) and reports, from the app's own per-frame log (--frame-log):
#   * latency breakdown per frame: tick -> input sample (just-in-time wait), sample -> emulated,
#     emulated -> commit (render), commit -> on screen (Metal presentedTime: GPU, compositor /
#     display pipeline), input sample -> on screen (total), presentation vs the targeted refresh
#   * input event -> on screen (INPUT=1 injects key presses of Z = B button; NSEvent timestamps)
#   * judder: consecutive frames whose on-screen interval differs from the steady one by more than
#     half a refresh; frames never shown; missed refreshes
#   * audio underruns / dropped samples / DRC resampling ratio; CPU per thread (emulation, main)
#     and process
#   scripts/perf-smoke.sh [app] [seconds] [rom]     (defaults: build/ReplayNES.app, 60, generated test ROM)
#   FULLSCREEN=1 ...       full screen (FILL; the chrome auto-hides while playing)
#   STRICT=1 ...           exit 1 when a threshold is missed (see below)
#   ACTIVATE=1 ...         bring the app to the front (default: it runs behind the front app;
#                          an occluded window is not presented at all)
#   WARMUP=150 ...         start measuring later (long take: big filmstrip)
#   INPUT=1 ...            inject key presses (event -> display latency)
#   EXTRA_ARGS="-filmstripThumbnails NO" ...  any launch arguments (e.g. -frameWorkgroup NO,
#                          -fullScreenAutoHide NO, -flashReduction 0)
#   LABEL=name ...         label for the JSON summary line
# Everything goes to build/perf-smoke (library / session roots, logs); the user's preferences
# are only overridden through the volatile argument domain (-integerScale NO ...).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-$ROOT/build/ReplayNES.app}"
DUR="${2:-60}"
ROM="${3:-}"
BIN="$APP/Contents/MacOS/ReplayNES"
CLI="$ROOT/build/tools/replaynes-cli/replaynes-cli"
WORK="${WORK:-$ROOT/build/perf-smoke}"
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
KEYS=()
if [ "${INPUT:-0}" = "1" ]; then
  # Z (B button) down/up every 0.25 s for the whole run.
  KEYS=(--inject-keys "$(python3 -c "
import sys; end=float(sys.argv[1]); t=3.0; out=[]
while t < end: out.append('%.3f:6:%s' % (t, 'd' if len(out) % 2 == 0 else 'u')); t += 0.25
print(','.join(out))" $((WARMUP + DUR + 2)))")
fi

# shellcheck disable=SC2086
"$BIN" --rom "$ROM" --autoplay --library-root "$WORK/library" --session-root "$WORK/session" --no-updater \
  --stats-log "$WORK/stats.jsonl" --frame-log "$WORK/frames.csv" --test-actions "$ACTS" ${KEYS[@]+"${KEYS[@]}"} \
  -integerScale NO -sidebarVisible NO -showLatency NO -ApplePersistenceIgnoreState YES ${EXTRA_ARGS:-} >"$WORK/app.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
sleep 1
# ACTIVATE=1: bring the launched instance to the front (it then also receives controller input).
[ "${ACTIVATE:-0}" = "1" ] && { open "$APP" 2>/dev/null || true; }

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
sleep 1

python3 - "$WORK" "$WARMUP" "${LABEL:-run}" <<'PY'
import csv, json, os, statistics, sys
work, warm, label = sys.argv[1], float(sys.argv[2]), sys.argv[3]
rows = [json.loads(l) for l in open(os.path.join(work, "stats.jsonl")) if l.strip()]
rows = [r for r in rows if r["t"] >= warm and not r["paused"]]
if len(rows) < 3: print("not enough samples (app paused or not running?)"); sys.exit(1)
a, b = rows[0], rows[-1]
dt = b["t"] - a["t"]; mins = dt / 60
d = lambda k: b.get(k, 0) - a.get(k, 0)
allrows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(open(os.path.join(work, "frames.csv")))]
if not allrows: print("no presented frames (window occluded? try ACTIVATE=1)"); sys.exit(1)
t0 = min(r["presented"] for r in allrows)
# Same window as the stats rows (stats t is from launch; frames start ~1-2 s later).
allrows = [r for r in allrows if warm - 2 <= r["presented"] - t0 <= warm - 2 + dt]
fr = [r for r in allrows if r.get("kind", 0) == 0]
# When each picture first reached the screen: its own present, or a repeat when that was dropped.
first = {}
for r in allrows:
    f = int(r["frame"])
    if f not in first or r["presented"] < first[f]: first[f] = r["presented"]
ms = lambda x: x * 1000
def stat(name, vals):
    vals = sorted(vals)
    if not vals: return {"n": 0}
    q = lambda p: vals[min(len(vals) - 1, int(p * (len(vals) - 1)))]
    return {"n": len(vals), "mean": statistics.fmean(vals), "p50": q(.5), "p95": q(.95), "p99": q(.99), "max": vals[-1]}
live = [r for r in fr if r["sample"] > 0]
stages = {
    "tick->sample (JIT wait)": [ms(r["sample"] - r["tick"]) for r in live if r["tick"] > 0],
    "sample->emulated": [ms(r["emulated"] - r["sample"]) for r in live],
    "emulated->commit (render)": [ms(r["commit"] - r["emulated"]) for r in live],
    "commit->on screen": [ms(r["presented"] - r["commit"]) for r in live],
    "INPUT SAMPLE->ON SCREEN": [ms(first[int(r["frame"])] - r["sample"]) for r in live],
    "on screen - target refresh": [ms(r["presented"] - r["target"]) for r in live if r["target"] > 0],
    "input event->on screen": [ms(first[int(r["frame"])] - r["event"]) for r in fr if r["event"] > 0],
}
# Judder: how long each picture stays on screen (next picture's first appearance - its own),
# consecutive frames only; steady = median; refresh from the stats (default 120 Hz).
frames_sorted = sorted(first)
iv = [first[b2] - first[b1] for b1, b2 in zip(frames_sorted, frames_sorted[1:]) if b2 == b1 + 1 and 0 < first[b2] - first[b1] < 0.25]
skipped = sum(b2 - b1 - 1 for b1, b2 in zip(frames_sorted, frames_sorted[1:]) if 1 < b2 - b1 <= 8)
refresh = 1 / b["refreshHz"] if b.get("refreshHz") else 1 / 120
steady = statistics.median(iv) if iv else 0
off = sum(1 for x in iv if abs(x - steady) > refresh / 2)
own = {int(r["frame"]) for r in fr}
late_pictures = sum(1 for f in first if f not in own)   # own present dropped, shown by a repeat
cpu = lambda k: d(k) / dt * 100
summary = {
    "label": label, "fullscreen": b["fullScreen"], "chromeHidden": b.get("chromeHidden", False), "pacing": b.get("pacing", "?"),
    "seconds": round(dt), "emulatedFPS": statistics.fmean(r["emulatedFPS"] for r in rows[1:]),
    "presentedFPS": d("presentCount") / dt, "steadyIntervalMs": ms(steady),
    "judderPerMin": off / mins, "neverShownPerMin": skipped / mins, "shownByRepeatPerMin": late_pictures / mins, "missedRefreshes": d("missedRefreshes"), "droppedFrames": d("droppedFrames"), "foreignCallbacks": d("foreignCallbacks"),
    "repeatPerSec": d("repeatPresents") / dt, "backlogDrains": d("backlogDrains"),
    "audioUnderruns": d("audioUnderruns"), "audioDropped": d("audioDropped"), "audioRatio": b.get("audioRatio", 1),
    "audioFillMs": statistics.fmean(r.get("audioFillMs", 0) for r in rows),
    "cpuEmulation": cpu("emulationCPU"), "cpuMain": cpu("mainCPU"), "cpuProcess": cpu("processCPU"),
    "gpuMs": statistics.fmean(r.get("displayGPUMs", 0) for r in rows), "inputLeadMs": b.get("inputLeadMs", 0),
    "stages": {k: stat(k, v) for k, v in stages.items()},
}
print(f"{label}: {summary['pacing']}  fullscreen={summary['fullscreen']} chromeHidden={summary['chromeHidden']}  window {dt:.0f}s  frames {a['frame']}->{b['frame']}")
print(f"emulated fps        {summary['emulatedFPS']:8.3f}   presented fps {summary['presentedFPS']:.3f}   steady interval {ms(steady):.2f} ms")
print(f"{'stage (ms)':28s} {'mean':>7s} {'p50':>7s} {'p95':>7s} {'p99':>7s} {'max':>7s}  n")
for k, s in summary["stages"].items():
    if s["n"]: print(f"{k:28s} {s['mean']:7.2f} {s['p50']:7.2f} {s['p95']:7.2f} {s['p99']:7.2f} {s['max']:7.2f}  {s['n']}")
print(f"judder / min        {summary['judderPerMin']:8.1f}   (interval off the steady one by > 1/2 refresh)")
print(f"never shown / min   {summary['neverShownPerMin']:8.1f}   shown only by a repeat / min {summary['shownByRepeatPerMin']:.1f}   missed refreshes {summary['missedRefreshes']}  dropped {summary['droppedFrames']}  main-thread updates {summary['foreignCallbacks']}   repeats/s {summary['repeatPerSec']:.1f}   backlog drains {summary['backlogDrains']}")
print(f"audio               underruns {summary['audioUnderruns']}  dropped {summary['audioDropped']}  ratio {summary['audioRatio']:.5f}  fill {summary['audioFillMs']:.1f} ms")
print(f"CPU % of one core   emulation {summary['cpuEmulation']:.1f}  main {summary['cpuMain']:.1f}  process {summary['cpuProcess']:.1f}   GPU busy+wait {summary['gpuMs']:.1f} ms/frame   input lead {summary['inputLeadMs']:.2f} ms")
print("JSON " + json.dumps(summary))
bad = []
if abs(summary["presentedFPS"] - summary["emulatedFPS"]) > 0.5: bad.append("presented fps")
if d("lateTicks") > 0: bad.append("stalled ticks")
if summary["judderPerMin"] > 6: bad.append("judder")
if summary["audioUnderruns"] > 0: bad.append("audio underruns")
print("RESULT", "OK" if not bad else "WARN: " + ", ".join(bad))
if bad and os.environ.get("STRICT") == "1": sys.exit(1)
PY
