#!/bin/bash
# MP4 export check of the Windows build (Media Foundation) in the VM: runs
# `replaynes-export --self-test` there (synthetic test ROM, recorded input; no game ROM), copies the
# files back and checks every one with ffprobe on the Mac against what the exporter reported
# (expect.jsonl): H.264 High + AAC-LC 48 kHz mono, size, BT.709 limited-range tags, video frame
# count (packets and decoded frames), timescale 39375000 with exactly 655171 per frame (every
# PTS = f * 655171), avg_frame_rate 39375000/655171, video duration, audio sample count.
# The self-test itself checks renderer-hash equality, cancel / invalid settings and ExportJob.
#   scripts/windows-vm/verify-export.sh [x86_64|aarch64] [--encoder mf_hardware|mf_software] [--frames N]
set -euo pipefail
. "$(dirname "$0")/common.sh"
need_running
ARCH="${1:-aarch64}"; shift || true
EXTRA="$*"
command -v ffprobe >/dev/null || { echo "ffprobe missing (brew install ffmpeg)"; exit 1; }
"$(dirname "$0")/deploy.sh" "$ARCH" >/dev/null
OUT="$ROOT/build/windows-$ARCH/export-selftest"
rm -rf "$OUT"; mkdir -p "$OUT"
GT="%TEMP%\\replaynes-export-selftest"
win_cmd "(if exist \"$GT\" rmdir /s /q \"$GT\") & cd /d \"$GUEST_DIR\\$ARCH\" & replaynes-export.exe --self-test \"$GT\" $EXTRA" | tee "$OUT/self-test.log"
win_cmd "robocopy \"$GT\" \"$(host_unc "$OUT")\" *.mp4 expect.jsonl /R:2 /W:1 /NJH /NJS /NFL /NDL /NP >nul & (if errorlevel 8 exit /b 8)" >/dev/null
python3 - "$OUT" <<'PY'
import json, subprocess, sys, os
out = sys.argv[1]
NUM, DEN, RATE = 39375000, 655171, 48000
fails = 0
for line in open(os.path.join(out, "expect.jsonl")):
    e = json.loads(line)
    path = os.path.join(out, e["file"])
    d = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-count_frames", "-count_packets", "-show_streams", "-of", "json", path]))
    v = next(s for s in d["streams"] if s["codec_type"] == "video")
    a = next(s for s in d["streams"] if s["codec_type"] == "audio")
    pts = [int(x) for x in subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "packet=pts",
                                                    "-of", "csv=p=0", path]).split()]
    bad = []
    if v["codec_name"] != "h264" or v.get("profile") != "High": bad.append(f"video {v['codec_name']} {v.get('profile')}")
    if (v["width"], v["height"]) != (e["width"], e["height"]): bad.append(f"size {v['width']}x{v['height']}")
    if (v.get("color_primaries"), v.get("color_space"), v.get("color_transfer"), v.get("color_range")) != ("bt709", "bt709", "bt709", "tv"):
        bad.append("colour tags")
    if int(v["nb_read_packets"]) != e["frames"] or int(v["nb_read_frames"]) != e["frames"]: bad.append(f"frames {v['nb_read_packets']}/{v['nb_read_frames']}")
    if v["time_base"] != f"1/{NUM}": bad.append(f"time_base {v['time_base']}")
    if sorted(pts) != [i * DEN for i in range(e["frames"])]: bad.append("PTS != f * 655171")
    if v["avg_frame_rate"] != f"{NUM}/{DEN}": bad.append(f"avg_frame_rate {v['avg_frame_rate']}")
    if int(v["duration_ts"]) != e["frames"] * DEN: bad.append(f"video duration_ts {v['duration_ts']}")
    if float(v.get("start_time", 0)) != 0: bad.append(f"video start {v['start_time']}")
    if a["codec_name"] != "aac" or int(a["sample_rate"]) != RATE or int(a["channels"]) != 1: bad.append("audio format")
    asamples = int(a["duration_ts"]) if a["time_base"] == f"1/{RATE}" else round(float(a["duration"]) * RATE)
    if abs(asamples - e["audio_samples"]) > 1024: bad.append(f"audio {asamples} samples")
    print(f"  {e['label']:10s} {'OK' if not bad else 'FAIL'}: {v['width']}x{v['height']} {v['nb_read_frames']} frames, "
          f"{v['avg_frame_rate']} fps, {float(v['duration']):.6f} s (expected {e['frames'] * DEN / NUM:.6f}), "
          f"audio {asamples} samples (expected {e['audio_samples']}), {int(v.get('bit_rate', 0)) / 1e6:.1f} Mbit/s, {e['encoder']}")
    for b in bad: print("             -", b)
    fails += bool(bad)
print(f"ffprobe: {fails} file(s) failed" if fails else "ffprobe: all files OK")
sys.exit(1 if fails else 0)
PY
