#!/bin/sh
# Builds ReplayNES for a real iPhone (signed with your team; a free "Personal Team" works) and,
# with --install, installs and starts it on the connected iPhone.
#
#   apps/ios/scripts/build-device.sh --team ABCDE12345 --bundle-id io.github.<you>.replaynes.dev
#   apps/ios/scripts/build-device.sh --install          # later runs (values are remembered)
#
# The team id and bundle id are written to apps/ios/Config/Signing.local.xcconfig (not committed);
# Xcode uses the same file. Requirements: Xcode with your Apple ID added (Xcode › Settings ›
# Accounts), XcodeGen, CMake (+ Ninja), the iPhone connected by cable (or paired over Wi-Fi) with
# Developer Mode on. See apps/ios/README.md.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
LOCAL="$HERE/Config/Signing.local.xcconfig"
TEAM="${TEAM_ID:-}"
BUNDLE="${BUNDLE_ID:-}"
INSTALL=0
CONFIG=Release

while [ $# -gt 0 ]; do
  case "$1" in
    --team) TEAM="$2"; shift 2 ;;
    --bundle-id) BUNDLE="$2"; shift 2 ;;
    --install) INSTALL=1; shift ;;
    --debug) CONFIG=Debug; shift ;;
    -h|--help) sed -n 2,13p "$0"; exit 0 ;;
    *) echo "unknown option: $1"; exit 2 ;;
  esac
done

value() { [ -f "$LOCAL" ] && sed -n "s/^$1 *= *//p" "$LOCAL" | tail -1 || true; }
[ -n "$TEAM" ] || TEAM="$(value DEVELOPMENT_TEAM)"
[ -n "$BUNDLE" ] || BUNDLE="$(value PRODUCT_BUNDLE_IDENTIFIER)"

if [ -z "$TEAM" ]; then
  cat <<'MSG'
error: no team id yet.
  1. Xcode › Settings › Accounts: add your Apple ID (a free account gives a "Personal Team").
  2. Find the team id (10 characters):
       security find-certificate -a -c "Apple Development" -p | openssl x509 -noout -subject
     (the OU= value), or open apps/ios/ReplayNES.xcodeproj › ReplayNES target › Signing &
     Capabilities, choose your team, and read it from the build settings (DEVELOPMENT_TEAM).
  3. Run again with  --team <TEAMID> --bundle-id io.github.<you>.replaynes.dev
MSG
  exit 1
fi
if [ -z "$BUNDLE" ] || [ "$BUNDLE" = "io.github.replaynes.dev" ]; then
  echo "error: choose a bundle id that is unique to you, e.g. --bundle-id io.github.<you>.replaynes.dev"
  exit 1
fi

cat > "$LOCAL" <<EOF
// Written by scripts/build-device.sh (not committed).
DEVELOPMENT_TEAM = $TEAM
PRODUCT_BUNDLE_IDENTIFIER = $BUNDLE
EOF
echo "==> signing: team $TEAM, bundle id $BUNDLE"

XCODEGEN="$(command -v xcodegen || echo /opt/homebrew/bin/xcodegen)"
[ -x "$XCODEGEN" ] || { echo "error: xcodegen not found (brew install xcodegen)"; exit 1; }
(cd "$HERE" && "$XCODEGEN" generate --quiet)

# The connected iPhone (devicectl, Xcode 15+): a free team can only sign for registered devices,
# and Xcode registers the one it builds for.
DEVICE=""
DEVJSON="$(mktemp)"
if xcrun devicectl list devices --json-output "$DEVJSON" >/dev/null 2>&1; then
  DEVICE="$(python3 -I - "$DEVJSON" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for dev in d.get("result", {}).get("devices", []):
    hw = dev.get("hardwareProperties", {})
    # Xcode 27 devicectl: no "reality" key for a real device (simulators say "simulated").
    if hw.get("platform") == "iOS" and hw.get("reality", "physical") in ("physical", None) \
            and dev.get("connectionProperties", {}).get("pairingState") == "paired":
        print(hw.get("udid", "")); break
PY
)"
fi
rm -f "$DEVJSON"
DEST="generic/platform=iOS"   # build generically (the device may be locked/not prepared for xcodebuild); install with devicectl
if [ -n "$DEVICE" ]; then echo "==> iPhone: $DEVICE"; else echo "==> no paired iPhone found: build only"; fi

# Build outputs: <repo>/build, or outside iCloud Drive when the checkout is in it (build-root.sh:
# codesign refuses files with File Provider attributes). The engine follows (RN_ENGINE_BUILD_DIR).
BUILD_ROOT="$("$HERE/scripts/build-root.sh" "$REPO")"
echo "==> build outputs: $BUILD_ROOT"
DERIVED="$BUILD_ROOT/ios-device"
# Build keychain (scripts/setup-build-keychain.sh): unlocked here, codesign signs from it, so the
# build also works over ssh (the login keychain is locked there). Absent: the login keychain.
SIGNKC=""
BKC="$HOME/Library/Keychains/replaynes-build.keychain-db"
BPASS="$HOME/.config/replaynes/build-keychain.pass"
if [ -f "$BKC" ] && [ -f "$BPASS" ]; then
  security unlock-keychain -p "$(cat "$BPASS")" "$BKC"
  SIGNKC="$BKC"
  echo "==> signing keychain: $BKC"
fi
# The version is ReplayNES's (CMakeLists.txt project VERSION), the build number the commit count:
# project.yml's MARKETING_VERSION had stayed 0.6.4 (2026-11-17: 0.6.7's code would have installed as 0.6.4),
# as ReplayArcade's scripts/build-ios.sh found for its own (0.3.16 installed as 0.3.4).
VERSION="$(sed -n 's/^project(ReplayNES VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")"
BUILD="$(git -C "$REPO" rev-list --count HEAD)"
echo "==> version $VERSION ($BUILD)"
xcodebuild -project "$HERE/ReplayNES.xcodeproj" -scheme ReplayNES -configuration "$CONFIG" \
  -destination "$DEST" -derivedDataPath "$DERIVED" -allowProvisioningUpdates \
  MARKETING_VERSION="$VERSION" CURRENT_PROJECT_VERSION="$BUILD" \
  RN_ENGINE_BUILD_DIR="$BUILD_ROOT/ios-engine" ${SIGNKC:+"OTHER_CODE_SIGN_FLAGS=--keychain $SIGNKC"} build
APP="$DERIVED/Build/Products/$CONFIG-iphoneos/ReplayNES.app"
echo "==> built $APP"

if [ "$INSTALL" = 1 ]; then
  [ -n "$DEVICE" ] || { echo "error: connect and unlock the iPhone (Developer Mode on), then run again"; exit 1; }
  # devicectl's service can't read bundles inside hidden directories (e.g. .claude/worktrees):
  # install from a copy in a plain temporary folder.
  STAGE="$(mktemp -d /tmp/replaynes-ios.XXXXXX)"
  ditto "$APP" "$STAGE/ReplayNES.app"
  xcrun devicectl device install app --device "$DEVICE" "$STAGE/ReplayNES.app"
  xcrun devicectl device process launch --device "$DEVICE" "$BUNDLE" || \
    echo "note: if iOS says the developer is not trusted: Settings › General › VPN & Device Management › your Apple ID › Trust"
fi
