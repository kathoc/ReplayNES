#!/bin/sh
# Build the ReplayArcade macOS app and its Release engine (arm64, macOS 14+).
# Usage: build-macos.sh [--run]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPLAYNES_DIR="$(cd "${REPLAYNES_DIR:-$ROOT/../..}" && pwd)"
export REPLAYNES_MACOS_DIR="$REPLAYNES_DIR/apps/macos"
HERE="$ROOT/packaging/macos"
OUT="$ROOT/build-macos"
ENGINE="$OUT/engine"
DERIVED="$OUT/DerivedData"
case "${1:-}" in '') RUN=0 ;; --run) RUN=1 ;; *) echo "usage: $0 [--run]" >&2; exit 2 ;; esac
[ "$#" -le 1 ] || { echo "usage: $0 [--run]" >&2; exit 2; }
find_tool() {
  for c in "$(command -v "$1" 2>/dev/null || true)" "/opt/homebrew/bin/$1" "/usr/local/bin/$1" "$HOME/.local/bin/$1"; do
    if [ -n "$c" ] && [ -x "$c" ]; then echo "$c"; return 0; fi
  done
  return 1
}
CMAKE="$(find_tool cmake)" || { echo 'error: cmake not found' >&2; exit 1; }
XCODEGEN="$(find_tool xcodegen)" || { echo 'error: xcodegen not found' >&2; exit 1; }
GEN='Unix Makefiles'
if NINJA="$(find_tool ninja)"; then GEN=Ninja; fi
# Do not inherit an Xcode toolchain or deployment target from a calling build.
unset CC CXX LD LDPLUSPLUS CFLAGS CXXFLAGS LDFLAGS CPATH LIBRARY_PATH SDKROOT
unset IPHONEOS_DEPLOYMENT_TARGET MACOSX_DEPLOYMENT_TARGET
if [ -f "$ENGINE/CMakeCache.txt" ] && ! grep -qx "REPLAYNES_DIR:PATH=$REPLAYNES_DIR" "$ENGINE/CMakeCache.txt"; then
  echo 'error: engine cache uses a different ReplayNES tree; remove build-macos/engine and retry' >&2
  exit 1
fi
if [ -f "$ENGINE/CMakeCache.txt" ]; then
  set --
else
  set -- -G "$GEN"
  if [ "$GEN" = Ninja ]; then set -- "$@" "-DCMAKE_MAKE_PROGRAM=$NINJA"; fi
fi
"$CMAKE" -S "$ROOT" -B "$ENGINE" "$@" \
    -DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_BUILD_TYPE=Release \
    -DREPLAYNES_DIR="$REPLAYNES_DIR" -DREPLAYARCADE_BUILD_TESTS=OFF \
    -DREPLAYNES_BUILD_CLI=OFF -DREPLAYNES_BUILD_DESKTOP=OFF \
    -DREPLAYNES_BUILD_WINDOWS_PROBE=OFF -DREPLAYNES_BUILD_WINDOWS_APP=OFF
"$CMAKE" --build "$ENGINE" --target replaynes_engine replaynes_frontend replayarcade_core replayarcade_startup fbneo_build fbneo_libretro_common
set -- "$ENGINE/replaynes/libreplayarcade_startup.a" "$ENGINE/replaynes/libreplayarcade_core.a" \
  "$ENGINE/replaynes/frontend/libreplaynes_frontend.a" "$ENGINE/replaynes/engine/libreplaynes_engine.a" \
  "$ENGINE/fbneo-src/src/burner/libretro/fbneo_libretro_static.a" "$ENGINE/replaynes/libfbneo_libretro_common.a"
for a in "$@"; do [ -f "$a" ] || { echo "error: missing $a" >&2; exit 1; }; done
libtool -static -no_warning_for_no_symbols -o "$ENGINE/libreplaynes_all.a" "$@"
test -f "$ENGINE/generated/replaynes/product.h"
VERSION="$(sed -n 's/^set(RN_PRODUCT_VERSION "\(.*\)")/\1/p' "$ROOT/cmake/product.cmake")"
BUILD="$(git -C "$ROOT" rev-list --count HEAD)"
[ -n "$VERSION" ] || { echo 'error: product version not found' >&2; exit 1; }
(cd "$HERE" && "$XCODEGEN" generate --quiet)
xcodebuild -project "$HERE/ReplayArcade.xcodeproj" -scheme ReplayNES -configuration Release \
  -destination 'platform=macOS,arch=arm64' -derivedDataPath "$DERIVED" \
  CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM= \
  "RN_ENGINE_BUILD_DIR=$ENGINE" "MARKETING_VERSION=$VERSION" "CURRENT_PROJECT_VERSION=$BUILD" build
BUILT="$DERIVED/Build/Products/Release/ReplayArcade.app"
[ -d "$BUILT" ] || { echo "error: missing $BUILT" >&2; exit 1; }
rm -rf "$OUT/ReplayArcade.app"
ditto "$BUILT" "$OUT/ReplayArcade.app"
codesign --verify --deep --strict "$OUT/ReplayArcade.app"
echo "==> $OUT/ReplayArcade.app ($VERSION, build $BUILD)"
if [ "$RUN" = 1 ]; then open "$OUT/ReplayArcade.app"; fi
