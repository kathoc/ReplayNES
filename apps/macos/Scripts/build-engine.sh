#!/bin/sh
# Builds the portable engine (same CMake source list as Linux/Windows) as static libs for the
# macOS app. Always Release: the emulator core is far too slow unoptimized for real-time play.
set -eu
REPO="$(cd "${SRCROOT:-$(dirname "$0")/..}/../.." && pwd)"
OUT="${RN_ENGINE_BUILD_DIR:-$REPO/build/xcode-engine}"

find_tool() {
  for c in "$(command -v "$1" 2>/dev/null || true)" "/opt/homebrew/bin/$1" "/usr/local/bin/$1" "$HOME/.local/bin/$1"; do
    if [ -n "$c" ] && [ -x "$c" ]; then echo "$c"; return 0; fi
  done
  return 1
}
CMAKE="$(find_tool cmake)" || { echo "error: cmake not found (brew install cmake)"; exit 1; }
GEN="Unix Makefiles"; MAKEPROG=""
if NINJA="$(find_tool ninja)"; then GEN="Ninja"; MAKEPROG="-DCMAKE_MAKE_PROGRAM=$NINJA"; fi

# Xcode exports variables that would leak into the CMake toolchain detection.
unset CC CXX LD LDPLUSPLUS CFLAGS CXXFLAGS LDFLAGS CPATH LIBRARY_PATH

if [ ! -f "$OUT/CMakeCache.txt" ]; then
  "$CMAKE" -S "$REPO" -B "$OUT" -G "$GEN" $MAKEPROG \
    -DCMAKE_BUILD_TYPE=Release \
    -DREPLAYNES_BUILD_TESTS=OFF -DREPLAYNES_BUILD_CLI=OFF \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
fi
"$CMAKE" --build "$OUT" --target replaynes_engine
test -f "$OUT/engine/libreplaynes_engine.a"
test -f "$OUT/libnestopia_core.a"
