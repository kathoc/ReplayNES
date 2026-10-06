#!/bin/sh
# Renders the Steam library artwork + hicolor icons from src/*.svg into png/ (committed, so the
# Flatpak build needs no SVG renderer). Deterministic for a given rsvg-convert; the committed PNGs
# come from org.freedesktop.Sdk//25.08 (rsvg-convert 2.62.3) - on a Linux host / the Steam Deck:
#   flatpak run --filesystem="$PWD" --command=sh org.freedesktop.Sdk//25.08 apps/linux/steam/artwork/render.sh
# Regenerate the SVGs first (wordmark outlines need the font) when the design changes:
#   python3 apps/linux/steam/artwork/tools/generate_artwork.py --font /usr/share/fonts/noto/NotoSans-Black.ttf
set -eu
cd "$(dirname "$0")"
command -v rsvg-convert >/dev/null || { echo "rsvg-convert not found (run inside org.freedesktop.Sdk)" >&2; exit 1; }
mkdir -p png/hicolor
r() { rsvg-convert --format=png "$@"; }
r -w 600 -h 900 -o png/capsule.png src/capsule.svg      # library capsule (portrait)  -> <id>p.png
r -w 920 -h 430 -o png/wide.png src/wide.svg            # wide capsule / header       -> <id>.png
r -w 1920 -h 620 -o png/hero.png src/hero.svg           # hero background            -> <id>_hero.png
r -o png/logo.png src/logo.svg                          # logo, 1280 wide, transparent -> <id>_logo.png
r -w 256 -h 256 -o png/icon.png src/icon.svg            # Steam shortcut icon         -> <id>_icon.png
for s in 64 128 256 512; do r -w $s -h $s -o png/hicolor/$s.png src/icon.svg; done
ls -l png png/hicolor
