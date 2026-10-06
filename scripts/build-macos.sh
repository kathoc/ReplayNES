#!/bin/bash
# Builds the macOS app (Release, arm64, ad-hoc signed, Sparkle embedded) and packages a zip.
#   scripts/build-macos.sh            -> build/ReplayNES.app, dist/ReplayNES-<version>-macOS-arm64.zip
# Requires: Xcode, XcodeGen (brew install xcodegen), CMake (+ Ninja recommended).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APPDIR="$ROOT/apps/macos"
VERSION="$(sed -n 's/^ *MARKETING_VERSION: *"\{0,1\}\([0-9.]*\)"\{0,1\}.*/\1/p' "$APPDIR/project.yml" | head -1)"
DERIVED="$ROOT/build/DerivedData-release"
OUT_APP="$ROOT/build/ReplayNES.app"
ZIP="$ROOT/dist/ReplayNES-$VERSION-macOS-arm64.zip"

XCODEGEN="$(command -v xcodegen || echo /opt/homebrew/bin/xcodegen)"
[ -x "$XCODEGEN" ] || { echo "xcodegen not found (brew install xcodegen)"; exit 1; }

echo "==> xcodegen ($VERSION)"
(cd "$APPDIR" && "$XCODEGEN" generate --quiet)

echo "==> xcodebuild Release"
xcodebuild -project "$APPDIR/ReplayNES.xcodeproj" -scheme ReplayNES -configuration Release \
  -destination 'platform=macOS,arch=arm64' -derivedDataPath "$DERIVED" \
  CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM= \
  clean build | grep -E "error:|warning: |BUILD (SUCCEEDED|FAILED)|\*\* " || true
test "${PIPESTATUS[0]}" -eq 0 || { echo "xcodebuild failed"; exit 1; }

BUILT="$DERIVED/Build/Products/Release/ReplayNES.app"
[ -d "$BUILT" ] || { echo "missing $BUILT"; exit 1; }
rm -rf "$OUT_APP"
ditto "$BUILT" "$OUT_APP"

echo "==> ad-hoc codesign (no Developer ID; see docs/RELEASE.md for notarization)"
# Inside-out, per component, as Sparkle documents (no --deep: it would drop the hardened-runtime
# flag / entitlements Sparkle's helpers are shipped with). SIGN_IDENTITY defaults to ad-hoc ("-").
# Sparkle accepts an update when its EdDSA signature matches SUPublicEDKey of the running app; an
# ad-hoc signature only has to be valid (old and new cdhash-based identities may differ).
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
sign() { codesign --force --sign "$SIGN_IDENTITY" --timestamp=none "$@"; }
SPK="$OUT_APP/Contents/Frameworks/Sparkle.framework"
if [ -d "$SPK" ]; then
  sign -o runtime "$SPK/Versions/B/XPCServices/Installer.xpc"
  sign -o runtime --preserve-metadata=entitlements "$SPK/Versions/B/XPCServices/Downloader.xpc"
  sign -o runtime "$SPK/Versions/B/Autoupdate"
  sign -o runtime "$SPK/Versions/B/Updater.app"
  sign -o runtime "$SPK"
fi
sign "$OUT_APP"
codesign --verify --deep --strict "$OUT_APP"

mkdir -p "$ROOT/dist"
rm -f "$ZIP"
STAGE="$ROOT/build/zip-stage/ReplayNES-$VERSION"
rm -rf "$ROOT/build/zip-stage" && mkdir -p "$STAGE"
ditto "$OUT_APP" "$STAGE/ReplayNES.app"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/README.md" "$STAGE/"
ditto -c -k --norsrc --keepParent "$STAGE" "$ZIP"
echo "==> $OUT_APP"
echo "==> $ZIP ($(du -h "$ZIP" | cut -f1))"
