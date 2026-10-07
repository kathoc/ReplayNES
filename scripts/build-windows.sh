#!/bin/bash
# Cross-compiles ReplayNES for Windows with llvm-mingw (docs/WINDOWS.md): the frontend
# (ReplayNES.exe: apps/windows + apps/desktop, SDL3 from third_party/SDL linked statically), the
# engine, the frontend core, the tests, replaynes-cli and the Windows probe, then packages
#   dist/ReplayNES-<version>-windows-<x64|arm64>.zip   (ReplayNES.exe, WinSparkle.dll, README.txt, licenses)
#   scripts/build-windows.sh [x86_64|aarch64|all]   (default: all) -> build/windows-<arch>/
# The test executables are run in the Windows VM by scripts/windows-vm/run-tests.sh.
# Env: BUILD_TYPE (Release), NO_PACKAGE=1 (skip the zip), BUILD_BASE (build/), DIST (dist/),
#      VERSION_OVERRIDE=<v> (ReplayNES.exe reports <v>, the zip is named after it: update tests),
#      EXTRA_CMAKE_ARGS (e.g. -DRNW_UPDATE_PUBLIC_KEY=<test key> -DRNW_WINSPARKLE=OFF).
# Requires: llvm-mingw in $LLVM_MINGW or ~/.local/opt/llvm-mingw, CMake, Ninja, python3, and the
# SDL submodule (git submodule update --init third_party/SDL).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCHS="${1:-all}"
[ "$ARCHS" = all ] && ARCHS="x86_64 aarch64"
BUILD_TYPE="${BUILD_TYPE:-Release}"
VERSION="$(sed -n 's/^project(ReplayNES VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "${VERSION_OVERRIDE:-}" ] && VERSION="$VERSION_OVERRIDE"
BUILD_BASE="${BUILD_BASE:-$ROOT/build}"
DIST="${DIST:-$ROOT/dist}"
[ -f "$ROOT/third_party/SDL/CMakeLists.txt" ] || { echo "third_party/SDL missing: git submodule update --init third_party/SDL" >&2; exit 1; }

for arch in $ARCHS; do
  case "$arch" in x86_64) zarch=x64 ;; aarch64) zarch=arm64 ;; *) echo "unknown arch: $arch (x86_64|aarch64|all)"; exit 2 ;; esac
  B="$BUILD_BASE/windows-$arch"
  echo "==> configure $arch ($BUILD_TYPE)"
  cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/windows-$arch-llvm-mingw.cmake" \
    -DREPLAYNES_VERSION_OVERRIDE="${VERSION_OVERRIDE:-}" ${EXTRA_CMAKE_ARGS:-} >/dev/null
  echo "==> build $arch"
  cmake --build "$B"
  [ "${NO_PACKAGE:-0}" = 1 ] && continue
  NAME="ReplayNES-$VERSION-windows-$zarch"
  STAGE="$B/package/$NAME"
  rm -rf "$B/package"; mkdir -p "$STAGE" "$DIST"
  cp "$B/apps/windows/ReplayNES.exe" "$STAGE/"
  # In-app updates (WinSparkle, MIT: THIRD_PARTY_NOTICES.md); absent with -DRNW_WINSPARKLE=OFF.
  [ -f "$B/apps/windows/WinSparkle.dll" ] && cp "$B/apps/windows/WinSparkle.dll" "$STAGE/"
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
  rm -f "$DIST/$NAME.zip"
  (cd "$B/package" && zip -qr -X "$DIST/$NAME.zip" "$NAME")
  echo "    ${DIST#$ROOT/}/$NAME.zip ($(du -h "$DIST/$NAME.zip" | cut -f1))"
done
