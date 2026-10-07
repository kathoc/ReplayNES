#!/bin/bash
# Cross-compiles ReplayNES for Windows with llvm-mingw (docs/WINDOWS.md): the frontend
# (ReplayNES.exe: apps/windows + apps/desktop, SDL3 from third_party/SDL linked statically), the
# engine, the frontend core, the tests, replaynes-cli and the Windows probe, then packages
#   dist/ReplayNES-<version>-windows-<x64|arm64>.zip   (ReplayNES.exe, README.txt, licenses)
#   scripts/build-windows.sh [x86_64|aarch64|all]   (default: all) -> build/windows-<arch>/
# The test executables are run in the Windows VM by scripts/windows-vm/run-tests.sh.
# Env: BUILD_TYPE (Release), NO_PACKAGE=1 (skip the zip).
# Requires: llvm-mingw in $LLVM_MINGW or ~/.local/opt/llvm-mingw, CMake, Ninja, python3, and the
# SDL submodule (git submodule update --init third_party/SDL).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCHS="${1:-all}"
[ "$ARCHS" = all ] && ARCHS="x86_64 aarch64"
BUILD_TYPE="${BUILD_TYPE:-Release}"
VERSION="$(sed -n 's/^project(ReplayNES VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -f "$ROOT/third_party/SDL/CMakeLists.txt" ] || { echo "third_party/SDL missing: git submodule update --init third_party/SDL" >&2; exit 1; }

for arch in $ARCHS; do
  case "$arch" in x86_64) zarch=x64 ;; aarch64) zarch=arm64 ;; *) echo "unknown arch: $arch (x86_64|aarch64|all)"; exit 2 ;; esac
  B="$ROOT/build/windows-$arch"
  echo "==> configure $arch ($BUILD_TYPE)"
  cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/windows-$arch-llvm-mingw.cmake" >/dev/null
  echo "==> build $arch"
  cmake --build "$B"
  [ "${NO_PACKAGE:-0}" = 1 ] && continue
  NAME="ReplayNES-$VERSION-windows-$zarch"
  STAGE="$B/package/$NAME"
  rm -rf "$B/package"; mkdir -p "$STAGE" "$ROOT/dist"
  cp "$B/apps/windows/ReplayNES.exe" "$STAGE/"
  cp "$ROOT/apps/windows/README.txt" "$STAGE/README.txt"
  cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
  cp "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
  # CRLF for the text files (Notepad shows LF fine since Windows 10 1809; older viewers do not).
  for f in README.txt LICENSE.txt; do python3 - "$STAGE/$f" <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read().replace("\r\n", "\n").replace("\n", "\r\n")
open(p, "w", encoding="utf-8", newline="").write(t)
PY
  done
  rm -f "$ROOT/dist/$NAME.zip"
  (cd "$B/package" && zip -qr -X "$ROOT/dist/$NAME.zip" "$NAME")
  echo "    dist/$NAME.zip ($(du -h "$ROOT/dist/$NAME.zip" | cut -f1))"
done
