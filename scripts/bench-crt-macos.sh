#!/bin/bash
# Headless CRT GPU benchmark / quality harness on the host GPU (no app window).
#   scripts/bench-crt-macos.sh [--ppu seq.ppu] [--sizes 1144x858,...] [--pace 60] [--profile]
#                              [--compare reference:fast --png DIR --png-frames 99,199]
# Frame sequences: build/tools/replaynes-cli/replaynes-cli dump-ppu <rom> --out seq.ppu ...
# (raw PPU output only; ROMs are never committed). Without --ppu a synthetic scroll is used.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/crt-bench"
mkdir -p "$OUT"
CRT="$ROOT/apps/macos/Sources/Core/CRT"
SRC=("$CRT/CRTModel.swift" "$CRT/CRTShaders.swift" "$CRT/CRTRenderer.swift" "$ROOT/apps/macos/Tools/CRTBench/main.swift")
BIN="$OUT/crt-bench"
if [ ! -x "$BIN" ] || [ -n "$(find "${SRC[@]}" -newer "$BIN" 2>/dev/null)" ]; then
  swiftc -O -o "$BIN" "${SRC[@]}"
fi
exec "$BIN" "$@"
