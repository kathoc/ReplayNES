#!/usr/bin/env python3
"""Renders the Windows app icon (replaynes.ico, committed) from the macOS icon source
apps/macos/Resources/IconSource/appicon-1024.svg: ImageMagick renders the SVG at 1024 px, the
rounded square is cropped with a small margin (the macOS grid leaves 100 px around it), Pillow
writes the sizes Explorer and the taskbar use (16-256 px; 256 stored as PNG).

    python3 apps/windows/resources/make_icon.py      (needs `magick` and Pillow)
"""
import os
import subprocess
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "..", "..", "macos", "Resources", "IconSource", "appicon-1024.svg")
OUT = os.path.join(HERE, "replaynes.ico")
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, "icon.png")
        subprocess.run(["magick", "-background", "none", "-density", "96", SVG, "-resize", "1024x1024", png], check=True)
        im = Image.open(png).convert("RGBA")
        margin = 84  # the rounded square spans 100...924
        im = im.crop((margin, margin, 1024 - margin, 1024 - margin))
        im.save(OUT, format="ICO", sizes=[(s, s) for s in SIZES])
    print(OUT)


if __name__ == "__main__":
    main()
