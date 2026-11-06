#!/bin/bash
# Builds the Linux Flatpak (io.github.replaynes.ReplayNES) on a Linux build host - by default the
# Steam Deck - installs it there (--user) and copies a single-file bundle to dist/.
#
#   scripts/build-linux-flatpak.sh                 # host deck@steamdeck.local
#   scripts/build-linux-flatpak.sh user@host       # any Linux with flatpak
#   HOST=local scripts/build-linux-flatpak.sh      # build on this (Linux) machine
#   ARCH=aarch64 HOST=lima-flatpak scripts/build-linux-flatpak.sh   # the ARM build (ARM SteamOS-likes)
#
# The checkout is rsynced to ~/ReplayNES-dev/src on the host (never roms/, .git, build trees or
# ROM files); the Nestopia core is fetched by flatpak-builder at the pinned commit. The host needs
# flatpak + network; org.freedesktop.Sdk//25.08 and org.flatpak.Builder are installed --user if
# missing (nothing is installed into the read-only SteamOS root).
# Env: ARCH (x86_64 or aarch64, default x86_64), REMOTE_DIR (default ReplayNES-dev, relative to the
# host's home), NO_BUNDLE=1, NO_INSTALL=1.
# ARCH: the SDK and flatpak-builder run at the app's architecture. A host builds its own one natively;
# macsrv's Lima VM (aarch64 + Rosetta) builds both, x86_64 through Rosetta (docs/RELEASE.md). The Deck
# builds x86_64 only. The build tree is per architecture (build-flatpak-<arch>); the repo is shared.
# The bundle made here has no repository (no --repo-url): fine for testing, but it never gets updates.
# Releases use scripts/publish-flatpak-repo.sh (signed repository on GitHub Pages + an updatable bundle).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${HOST:-${1:-deck@steamdeck.local}}"
RDIR="${REMOTE_DIR:-ReplayNES-dev}"
APP_ID=io.github.replaynes.ReplayNES
MANIFEST=apps/linux/flatpak/$APP_ID.yml
VERSION="$(sed -n 's/^project(ReplayNES VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
ARCH="${ARCH:-x86_64}"
case "$ARCH" in x86_64|aarch64) ;; *) echo "ARCH must be x86_64 or aarch64 (got $ARCH)" >&2; exit 2 ;; esac
BUNDLE="$APP_ID-$VERSION-$ARCH.flatpak"

run() {
  if [ "$HOST" = "local" ]; then bash -c "cd ~ && $1"; else ssh "$HOST" "$1"; fi
}

EXCLUDES=(
  --exclude=/.git --exclude=/.claude --exclude=/roms/ --exclude=/dist/ --exclude=/build/ --exclude='/build-*/'
  --exclude=/.flatpak-builder/ --exclude=/third_party/nestopia/ --exclude=/third_party/syphon/ --exclude=/third_party/SDL/
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
  # Incremental developer build with the same SDK (no packaging): ~/ReplayNES-dev/build-dev-<arch>.
  # Needs the pinned core in src/third_party/nestopia on the host (kept by rsync's excludes):
  #   git clone https://github.com/0ldsk00l/nestopia.git && git checkout <pin from cmake/NestopiaCore.cmake>
  # Run it inside the installed app's sandbox:
  #   flatpak run --filesystem=home --command=$HOME/ReplayNES-dev/build-dev-x86_64/replaynes-linux io.github.replaynes.ReplayNES
  echo "==> dev build (SDK, incremental)"
  run "cd ~/$RDIR && flatpak run --user --arch=$ARCH --filesystem=home --share=network --command=sh org.freedesktop.Sdk//25.08 -c \
    'cmake -S src/apps/linux -B build-dev-$ARCH -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C build-dev-$ARCH'"
  exit 0
fi

echo "==> build tools ($ARCH)"
# --arch=$ARCH everywhere: the builder and the SDK at the app's architecture, whatever the host's is.
run "flatpak info --user --arch=$ARCH org.flatpak.Builder >/dev/null 2>&1 || flatpak install --user -y --noninteractive --arch=$ARCH flathub org.flatpak.Builder
flatpak info --arch=$ARCH org.freedesktop.Sdk//25.08 >/dev/null 2>&1 || flatpak install --user -y --noninteractive --arch=$ARCH flathub org.freedesktop.Sdk//25.08"

INSTALL="--install"
[ "${NO_INSTALL:-0}" = "1" ] && INSTALL=""
echo "==> flatpak-builder ($ARCH)"
run "cd ~/$RDIR && flatpak run --arch=$ARCH org.flatpak.Builder --arch=$ARCH --user $INSTALL --force-clean --disable-updates \
  --state-dir=.flatpak-builder --repo=repo build-flatpak-$ARCH src/$MANIFEST"

if [ "${NO_BUNDLE:-0}" != "1" ]; then
  echo "==> bundle $BUNDLE"
  run "cd ~/$RDIR && flatpak build-bundle --arch=$ARCH --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo repo $BUNDLE $APP_ID"
  mkdir -p "$ROOT/dist"
  if [ "$HOST" = "local" ]; then cp ~/"$RDIR/$BUNDLE" "$ROOT/dist/"; else scp -q "$HOST:$RDIR/$BUNDLE" "$ROOT/dist/"; fi
  echo "    $ROOT/dist/$BUNDLE"
fi
echo "==> done. Run: flatpak run --arch=$ARCH $APP_ID   (on $HOST)"
