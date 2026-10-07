#!/bin/bash
# Builds the Windows release zips and the WinSparkle update feed. Does NOT tag, push or upload.
#   scripts/release-windows.sh <version>
#     -> dist/ReplayNES-<version>-windows-{x64,arm64}.zip  (scripts/build-windows.sh: ReplayNES.exe,
#        WinSparkle.dll, README.txt, licenses)
#     -> dist/appcast-windows.xml  (one item, one EdDSA-signed enclosure per architecture:
#        sparkle:os="windows-x64" / "windows-arm64"; WinSparkle picks its own)
# The feed has the macOS appcast's format (docs/RELEASE.md) and is signed with the same EdDSA key
# (Sparkle's sign_update; the public half is SUPublicEDKey in apps/macos/project.yml, which the
# Windows build embeds). The app reads https://github.com/<repo>/releases/latest/download/appcast-windows.xml,
# so every release (also a macOS-only one) must carry it.
#
# <version> must equal the project VERSION in CMakeLists.txt (unless VERSION_OVERRIDE builds a test).
#
# Environment (all optional; for local update testing):
#   SKIP_BUILD=1           reuse the zips in $DIST
#   DIST=<dir>             where the zips are / the feed goes (default dist/)
#   VERSION_OVERRIDE=1     build <version> as given (e.g. 0.3.2-test) instead of checking CMakeLists.txt
#   DOWNLOAD_BASE=<url>    enclosure URL prefix (default: the GitHub release's download URL)
#   APPCAST=<path>         output feed (default $DIST/appcast-windows.xml)
#   SPARKLE_ACCOUNT=<a>    keychain account of the signing key (default ReplayNES)
#   ED_KEY_FILE=<file>     sign with this private key file (base64 ed25519 seed) instead of the keychain
#   EXTRA_CMAKE_ARGS       passed to scripts/build-windows.sh (e.g. -DRNW_UPDATE_PUBLIC_KEY=<test key>)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="kathoc/ReplayNES"
VERSION="${1:?usage: scripts/release-windows.sh <version>}"
SPARKLE_ACCOUNT="${SPARKLE_ACCOUNT:-ReplayNES}"
DIST="${DIST:-$ROOT/dist}"
APPCAST="${APPCAST:-$DIST/appcast-windows.xml}"
TAG="v$VERSION"
DOWNLOAD_BASE="${DOWNLOAD_BASE:-https://github.com/$REPO/releases/download/$TAG}"
NOTES_URL="https://github.com/$REPO/releases/tag/$TAG"
PROJECT_VERSION="$(sed -n 's/^project(ReplayNES VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"

if [ -z "${SKIP_BUILD:-}" ]; then
  if [ -n "${VERSION_OVERRIDE:-}" ]; then
    VERSION_OVERRIDE="$VERSION" DIST="$DIST" "$ROOT/scripts/build-windows.sh" all
  else
    [ "$VERSION" = "$PROJECT_VERSION" ] || { echo "version $VERSION != project VERSION $PROJECT_VERSION in CMakeLists.txt"; exit 1; }
    DIST="$DIST" "$ROOT/scripts/build-windows.sh" all
  fi
fi

# Sparkle's CLI tools (the macOS release build resolves them: scripts/build-macos.sh).
BIN="$ROOT/build/DerivedData-release/SourcePackages/artifacts/sparkle/Sparkle/bin"
[ -x "$BIN/sign_update" ] || { echo "Sparkle tools not found in $BIN (run scripts/build-macos.sh once)"; exit 1; }
if [ -n "${ED_KEY_FILE:-}" ]; then
  SIGN=("$BIN/sign_update" --ed-key-file "$ED_KEY_FILE")
else
  SIGN=("$BIN/sign_update" --account "$SPARKLE_ACCOUNT")
  KEY="$("$BIN/generate_keys" --account "$SPARKLE_ACCOUNT" -p)" || { echo "no Sparkle key for account $SPARKLE_ACCOUNT in the keychain"; exit 1; }
  APP_KEY="$(sed -n 's/^ *SUPublicEDKey: *"\{0,1\}\([A-Za-z0-9+\/=]*\)"\{0,1\}.*/\1/p' "$ROOT/apps/macos/project.yml" | head -1)"
  [ "$KEY" = "$APP_KEY" ] || { echo "SUPublicEDKey in project.yml ($APP_KEY) != keychain key ($KEY)"; exit 1; }
fi

ENCLOSURES=""
for zarch in x64 arm64; do
  ZIP="$DIST/ReplayNES-$VERSION-windows-$zarch.zip"
  [ -f "$ZIP" ] || { echo "missing $ZIP"; exit 1; }
  LIST="$(unzip -l "$ZIP")"
  [[ "$LIST" == *"ReplayNES-$VERSION-windows-$zarch/WinSparkle.dll"* ]] || { echo "$ZIP has no WinSparkle.dll"; exit 1; }
  echo "==> sign_update $(basename "$ZIP")"
  SIG="$("${SIGN[@]}" -p "$ZIP")"
  "${SIGN[@]}" --verify "$ZIP" "$SIG" >/dev/null || { echo "signature verification failed"; exit 1; }
  LEN="$(stat -f %z "$ZIP")"
  ENCLOSURES+="      <enclosure url=\"$DOWNLOAD_BASE/$(basename "$ZIP")\" sparkle:os=\"windows-$zarch\" length=\"$LEN\" type=\"application/octet-stream\" sparkle:edSignature=\"$SIG\"/>
"
done

mkdir -p "$(dirname "$APPCAST")"
PUBDATE="$(LC_ALL=C date -u '+%a, %d %b %Y %H:%M:%S +0000')"
cat > "$APPCAST" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>ReplayNES for Windows</title>
    <link>https://github.com/$REPO</link>
    <description>ReplayNES updates (Windows)</description>
    <language>ja</language>
    <item>
      <title>ReplayNES $VERSION</title>
      <pubDate>$PUBDATE</pubDate>
      <sparkle:version>$VERSION</sparkle:version>
      <sparkle:shortVersionString>$VERSION</sparkle:shortVersionString>
      <sparkle:releaseNotesLink>$NOTES_URL</sparkle:releaseNotesLink>
$ENCLOSURES    </item>
  </channel>
</rss>
EOF
xmllint --noout "$APPCAST"
echo "==> $APPCAST (version $VERSION, x64 + arm64)"

cat <<EOF

Next steps (not run by this script; with the macOS assets of scripts/release.sh):
  gh release create $TAG dist/ReplayNES-$VERSION-macOS-arm64.zip dist/appcast.xml \\
    "$DIST/ReplayNES-$VERSION-windows-x64.zip" "$DIST/ReplayNES-$VERSION-windows-arm64.zip" "$APPCAST" ...
The release must be "latest" so that .../releases/latest/download/appcast-windows.xml is this feed.
EOF
