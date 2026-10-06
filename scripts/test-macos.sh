#!/bin/bash
# Headless checks for the macOS frontend:
#  1. XCTest bundle (exporter, geometry, audio ring, input config) - no UI, no host app.
#  2. The H.264 smoke export written by the test is verified with ffprobe when available.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APPDIR="$ROOT/apps/macos"
SMOKE_DIR="$ROOT/build/smoke"
mkdir -p "$SMOKE_DIR"
SMOKE="$SMOKE_DIR/smoke-export.mp4"
rm -f "$SMOKE"

(cd "$APPDIR" && "$(command -v xcodegen || echo /opt/homebrew/bin/xcodegen)" generate --quiet)
# TEST_RUNNER_ prefixed variables are forwarded to the test process.
TEST_RUNNER_RN_SMOKE_MP4="$SMOKE" xcodebuild -project "$APPDIR/ReplayNES.xcodeproj" -scheme ReplayNES \
  -destination 'platform=macOS,arch=arm64' -derivedDataPath "$ROOT/build/DerivedData" test \
  | grep -E "Test Case .*(passed|failed)|error:|Executed|\*\* TEST" \
  || { echo "xcodebuild test failed"; exit 1; } # pipefail: xcodebuild's status (PIPESTATUS after "|| true" was always 0)

[ -f "$SMOKE" ] || { echo "smoke export missing: $SMOKE"; exit 1; }
if command -v ffprobe >/dev/null; then
  echo "==> ffprobe $SMOKE"
  ffprobe -v error -show_entries stream=codec_name,width,height,r_frame_rate,sample_rate,channels,nb_frames:format=duration \
    -of compact "$SMOKE"
  NB=$(ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$SMOKE")
  [ "$NB" = "180" ] || { echo "expected 180 video frames, got $NB"; exit 1; }
  echo "video frames: $NB (ok)"
else
  echo "ffprobe not found; AVAsset checks in XCTest already passed"
fi
