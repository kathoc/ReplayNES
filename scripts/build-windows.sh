#!/bin/bash
# Cross-compiles the portable parts (engine, frontend core, tests, replaynes-cli, Windows probe)
# for Windows with llvm-mingw (docs/WINDOWS.md):
#   scripts/build-windows.sh [x86_64|aarch64|all]   (default: all) -> build/windows-<arch>/
# The test executables are run in the Windows VM by scripts/windows-vm/run-tests.sh.
# Requires: llvm-mingw in $LLVM_MINGW or ~/.local/opt/llvm-mingw, CMake, Ninja, python3.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCHS="${1:-all}"
[ "$ARCHS" = all ] && ARCHS="x86_64 aarch64"
BUILD_TYPE="${BUILD_TYPE:-Release}"

for arch in $ARCHS; do
  case "$arch" in x86_64|aarch64) ;; *) echo "unknown arch: $arch (x86_64|aarch64|all)"; exit 2 ;; esac
  B="$ROOT/build/windows-$arch"
  echo "==> configure $arch ($BUILD_TYPE)"
  cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/windows-$arch-llvm-mingw.cmake" >/dev/null
  echo "==> build $arch"
  cmake --build "$B"
done
