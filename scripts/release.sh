#!/bin/bash
# Builds a release and prepares the Sparkle update feed. Does NOT tag, push or upload.
#   scripts/release.sh <version>
#     -> build/ReplayNES.app, dist/ReplayNES-<version>-macOS-arm64.zip (scripts/build-macos.sh)
#     -> dist/appcast.xml  (one item, EdDSA-signed enclosure)
#     -> prints the `gh release create` command to run
#
# <version> must equal MARKETING_VERSION in apps/macos/project.yml (bump it, and
# CURRENT_PROJECT_VERSION, CMakeLists.txt project VERSION, by hand first; see docs/RELEASE.md).
#
# The EdDSA private key lives only in the login keychain (generic password "https://sparkle-project.org",
# account "$SPARKLE_ACCOUNT"). Its public half is SUPublicEDKey in project.yml; this script refuses
# to continue if they differ.
#
# Environment (all optional; for local update testing):
#   SKIP_BUILD=1          reuse the existing build/ReplayNES.app and zip
#   ZIP=<path>            archive to sign (default dist/ReplayNES-<version>-macOS-arm64.zip)
#   APP=<path>            app whose Info.plist provides version/build (default build/ReplayNES.app)
#   DOWNLOAD_URL=<url>    enclosure URL (default: the GitHub release asset URL)
#   APPCAST=<path>        output feed (default dist/appcast.xml)
#   SPARKLE_ACCOUNT=<a>   keychain account of the signing key (default ReplayNES)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="kathoc/ReplayNES"
VERSION="${1:?usage: scripts/release.sh <version>}"
SPARKLE_ACCOUNT="${SPARKLE_ACCOUNT:-ReplayNES}"
PROJECT_VERSION="$(sed -n 's/^ *MARKETING_VERSION: *"\{0,1\}\([0-9.]*\)"\{0,1\}.*/\1/p' "$ROOT/apps/macos/project.yml" | head -1)"
APP="${APP:-$ROOT/build/ReplayNES.app}"
ZIP="${ZIP:-$ROOT/dist/ReplayNES-$VERSION-macOS-arm64.zip}"
APPCAST="${APPCAST:-$ROOT/dist/appcast.xml}"
TAG="v$VERSION"
DOWNLOAD_URL="${DOWNLOAD_URL:-https://github.com/$REPO/releases/download/$TAG/$(basename "$ZIP")}"
NOTES_URL="https://github.com/$REPO/releases/tag/$TAG"

if [ -z "${SKIP_BUILD:-}" ]; then
  [ "$VERSION" = "$PROJECT_VERSION" ] || { echo "version $VERSION != MARKETING_VERSION $PROJECT_VERSION in project.yml"; exit 1; }
  "$ROOT/scripts/build-macos.sh"
fi
[ -f "$ZIP" ] || { echo "missing $ZIP"; exit 1; }
[ -d "$APP" ] || { echo "missing $APP"; exit 1; }

# Sparkle CLI tools come with the resolved SPM package (same version as the embedded framework).
BIN="$ROOT/build/DerivedData-release/SourcePackages/artifacts/sparkle/Sparkle/bin"
[ -x "$BIN/sign_update" ] || { echo "Sparkle tools not found in $BIN (run scripts/build-macos.sh first)"; exit 1; }

plist() { /usr/libexec/PlistBuddy -c "Print :$1" "$APP/Contents/Info.plist"; }
SHORT="$(plist CFBundleShortVersionString)"
BUILD="$(plist CFBundleVersion)"
MINOS="$(plist LSMinimumSystemVersion)"
APP_KEY="$(plist SUPublicEDKey)"
[ "$SHORT" = "$VERSION" ] || { echo "app version $SHORT != $VERSION"; exit 1; }

KEY="$("$BIN/generate_keys" --account "$SPARKLE_ACCOUNT" -p)" || { echo "no Sparkle key for account $SPARKLE_ACCOUNT in the keychain"; exit 1; }
[ "$KEY" = "$APP_KEY" ] || { echo "SUPublicEDKey in the app ($APP_KEY) != keychain key ($KEY)"; exit 1; }
codesign --verify --deep --strict "$APP"

echo "==> sign_update $(basename "$ZIP")"
SIG="$("$BIN/sign_update" --account "$SPARKLE_ACCOUNT" -p "$ZIP")"
LEN="$(stat -f %z "$ZIP")"
"$BIN/sign_update" --account "$SPARKLE_ACCOUNT" --verify "$ZIP" "$SIG" >/dev/null || { echo "signature verification failed"; exit 1; }

mkdir -p "$(dirname "$APPCAST")"
PUBDATE="$(LC_ALL=C date -u '+%a, %d %b %Y %H:%M:%S +0000')"
cat > "$APPCAST" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>ReplayNES</title>
    <link>https://github.com/$REPO</link>
    <description>ReplayNES updates</description>
    <language>ja</language>
    <item>
      <title>ReplayNES $SHORT</title>
      <pubDate>$PUBDATE</pubDate>
      <sparkle:version>$BUILD</sparkle:version>
      <sparkle:shortVersionString>$SHORT</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>$MINOS</sparkle:minimumSystemVersion>
      <sparkle:releaseNotesLink>$NOTES_URL</sparkle:releaseNotesLink>
      <enclosure url="$DOWNLOAD_URL" length="$LEN" type="application/octet-stream" sparkle:edSignature="$SIG"/>
    </item>
  </channel>
</rss>
EOF
xmllint --noout "$APPCAST"
echo "==> $APPCAST (version $SHORT build $BUILD, min macOS $MINOS)"

cat <<EOF

Next steps (not run by this script):
  git tag $TAG && git push origin $TAG
  gh release create $TAG "$ZIP" "$APPCAST" --repo $REPO --title "ReplayNES $VERSION" --notes-file <notes.md>
The release must be marked "latest" (the default) so that
https://github.com/$REPO/releases/latest/download/appcast.xml points at this appcast.
EOF
