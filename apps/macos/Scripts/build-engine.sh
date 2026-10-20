#!/bin/sh
# Builds the portable engine and the shared frontend core (same CMake source lists as
# Linux/Windows) as static libs for the macOS app. Always Release: the emulator core is far too
# slow unoptimized for real-time play. The frontend core's localization table is generated from
# Resources/Localizable.xcstrings by this build (python3).
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
# Optional development core (docs/CORE_SELECTION_PLAN.md): only when REPLAYNESCORE_DIR names a
# ReplayNEScore checkout; release builds leave it unset and stay Nestopia-only.
EXTRA_TARGETS=""
if [ -n "${REPLAYNESCORE_DIR:-}" ]; then
  [ -f "$REPLAYNESCORE_DIR/include/replaynes/nrom_cpu_ppu.hpp" ] || { echo "error: REPLAYNESCORE_DIR=$REPLAYNESCORE_DIR is not a ReplayNEScore checkout"; exit 1; }
  "$CMAKE" -S "$REPO" -B "$OUT" -DREPLAYNES_ENABLE_REPLAYNESCORE=ON -DREPLAYNESCORE_SOURCE_DIR="$REPLAYNESCORE_DIR" >/dev/null
  EXTRA_TARGETS="replaynes_experimental_adapter"
else
  "$CMAKE" -S "$REPO" -B "$OUT" -DREPLAYNES_ENABLE_REPLAYNESCORE=OFF >/dev/null
fi
"$CMAKE" --build "$OUT" --target replaynes_engine replaynes_frontend $EXTRA_TARGETS
test -f "$OUT/engine/libreplaynes_engine.a"
test -f "$OUT/frontend/libreplaynes_frontend.a"
test -f "$OUT/libnestopia_core.a"

# libreplaynes_extra.a: the adapter + every ReplayNEScore archive in one library (the app links one
# fixed name), or a one-symbol stub without the core. Rebuilt each time so a switch never leaves stale code.
rm -f "$OUT/libreplaynes_extra.a"
if [ -n "$EXTRA_TARGETS" ]; then
  # shellcheck disable=SC2046
  xcrun libtool -static -no_warning_for_no_symbols -o "$OUT/libreplaynes_extra.a" \
    "$OUT/engine/libreplaynes_experimental_adapter.a" $(find "$OUT/engine/experimental-core" -name 'lib*.a' | sort)
else
  echo 'void rn_extra_stub(void) {}' | xcrun clang -arch arm64 -mmacosx-version-min=14.0 -x c -c -o "$OUT/extra_stub.o" -
  xcrun libtool -static -o "$OUT/libreplaynes_extra.a" "$OUT/extra_stub.o"
fi
test -f "$OUT/libreplaynes_extra.a"
