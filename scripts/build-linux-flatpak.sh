#!/bin/bash
# Builds the Linux Flatpak (io.github.replaynes.ReplayNES) on a Linux build host - by default the
# Steam Deck - installs it there (--user) and copies a single-file bundle to dist/.
#
#   scripts/build-linux-flatpak.sh                 # host deck@steamdeck.local
#   scripts/build-linux-flatpak.sh user@host       # any x86_64 Linux with flatpak
#   HOST=local scripts/build-linux-flatpak.sh      # build on this (Linux) machine
#
# The checkout is rsynced to ~/ReplayNES-dev/src on the host (never roms/, .git, build trees or
# ROM files); the Nestopia core is fetched by flatpak-builder at the pinned commit. The host needs
# flatpak + network; org.freedesktop.Sdk//25.08 and org.flatpak.Builder are installed --user if
# missing (nothing is installed into the read-only SteamOS root).
# Env: REMOTE_DIR (default ReplayNES-dev, relative to the host's home), NO_BUNDLE=1, NO_INSTALL=1.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${HOST:-${1:-deck@steamdeck.local}}"
RDIR="${REMOTE_DIR:-ReplayNES-dev}"
APP_ID=io.github.replaynes.ReplayNES
MANIFEST=apps/linux/flatpak/$APP_ID.yml
VERSION="$(sed -n 's/^project(ReplayNES VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUNDLE="$APP_ID-$VERSION-x86_64.flatpak"

run() {
  if [ "$HOST" = "local" ]; then bash -c "cd ~ && $1"; else ssh "$HOST" "$1"; fi
}

EXCLUDES=(
  --exclude=/.git --exclude=/.claude --exclude=/roms/ --exclude=/dist/ --exclude=/build/ --exclude='/build-*/'
  --exclude=/.flatpak-builder/ --exclude=/third_party/nestopia/ --exclude=/third_party/syphon/
  --exclude=/apps/macos/ReplayNES.xcodeproj/ --exclude=DerivedData/ --exclude=.DS_Store
  --exclude='*.[nN][eE][sS]' --exclude='*.[fF][dD][sS]' --exclude='*.[uU][nN][iI][fF]' --exclude='*.unf'
  --exclude='*.zip' --exclude='*.7z'
)
echo "==> sync $ROOT -> $HOST:~/$RDIR/src"
if [ "$HOST" = "local" ]; then
  mkdir -p ~/"$RDIR"/src
  rsync -a --delete "${EXCLUDES[@]}" "$ROOT/" ~/"$RDIR"/src/
else
  ssh "$HOST" "mkdir -p ~/$RDIR/src"
  rsync -a --delete -e ssh "${EXCLUDES[@]}" "$ROOT/" "$HOST:$RDIR/src/"
fi

if [ "${DEV:-0}" = "1" ]; then
  # Incremental developer build with the same SDK (no packaging): ~/ReplayNES-dev/build-dev.
  # Needs the pinned core in src/third_party/nestopia on the host (kept by rsync's excludes):
  #   git clone https://github.com/0ldsk00l/nestopia.git && git checkout <pin from cmake/NestopiaCore.cmake>
  # Run it inside the installed app's sandbox:
  #   flatpak run --filesystem=home --command=$HOME/ReplayNES-dev/build-dev/replaynes-linux io.github.replaynes.ReplayNES
  echo "==> dev build (SDK, incremental)"
  run "cd ~/$RDIR && flatpak run --user --filesystem=home --share=network --command=sh org.freedesktop.Sdk//25.08 -c \
    'cmake -S src/apps/linux -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C build-dev'"
  exit 0
fi

echo "==> build tools"
run "flatpak info --user org.flatpak.Builder >/dev/null 2>&1 || flatpak install --user -y --noninteractive flathub org.flatpak.Builder
flatpak info org.freedesktop.Sdk//25.08 >/dev/null 2>&1 || flatpak install --user -y --noninteractive flathub org.freedesktop.Sdk//25.08"

INSTALL="--install"
[ "${NO_INSTALL:-0}" = "1" ] && INSTALL=""
echo "==> flatpak-builder"
run "cd ~/$RDIR && flatpak run org.flatpak.Builder --user $INSTALL --force-clean --disable-updates \
  --state-dir=.flatpak-builder --repo=repo build-flatpak src/$MANIFEST"

if [ "${NO_BUNDLE:-0}" != "1" ]; then
  echo "==> bundle $BUNDLE"
  run "cd ~/$RDIR && flatpak build-bundle repo $BUNDLE $APP_ID"
  mkdir -p "$ROOT/dist"
  if [ "$HOST" = "local" ]; then cp ~/"$RDIR/$BUNDLE" "$ROOT/dist/"; else scp -q "$HOST:$RDIR/$BUNDLE" "$ROOT/dist/"; fi
  echo "    $ROOT/dist/$BUNDLE"
fi
echo "==> done. Run: flatpak run $APP_ID   (on $HOST)"
