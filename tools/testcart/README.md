# ReplayNES Test Cartridge

A test / demo program for the NES written for ReplayNES: 6502 assembly, tiles and font made for
this project (no third-party code, graphics or fonts). NROM-128 (mapper 0), 16 KiB PRG + 8 KiB
CHR, vertical mirroring. It runs on any NES emulator and on real hardware.

Licence: **CC0 1.0** (public domain dedication) for everything in this directory and the
generated `engine/src/testrom/TestCartridge*.inc`: to the extent possible under law, the authors
waive all copyright and related rights to this work
(<https://creativecommons.org/publicdomain/zero/1.0/>). The rest of ReplayNES stays
GPL-2.0-or-later; CC0 material can be used in it (and anywhere else) freely.

## Screens

Title: Up/Down to choose, A to open. In a test, Select = next test, Start = menu (controllers:
Select+Start together).

| Screen | What it shows | Controls |
|---|---|---|
| Palette chart | all 64 colours, 16 hue rows x 4 luminances; the palette is rewritten between rows | A emphasis bits, B greyscale |
| Color bars | colour bars, grey ramp ($0F $2D $00 $10 $3D $20), 1 px stripes / checker | A emphasis, B greyscale |
| Sprites | 8 bouncing sprites, sprite-0 hit and overflow flag readouts, a line of 8 or 9 sprites | D-pad moves sprite 0, A 8/9, B+A flicker |
| Scrolling | 512x240 world (A1..H5 labels): H, V, diagonal, D-pad, split (status bar fixed) | A mode, B speed |
| Controllers | both pads, 8 buttons each, raw byte | press Select+Start together to leave |
| Rapid-fire meter | 10 s test and turbo check (below) | Left/Right mode, Down reset |
| Sound test | pulse 1/2 (duty), triangle, noise (mode, rate), C major scale | Up/Down, A on/off, B duty/mode, Left/Right pitch |

### Rapid-fire meter

Input is polled 4 times per frame (once in the NMI, three times from the main loop; strobe + 8
reads of $4016/$4017 each, bits 0-1, no DPCM ever playing). Every A/B change seen by any poll is
an edge, stamped with the frame number. On hardware this counts taps shorter than a frame; in an
emulator the pads change once per frame, so the resolution there is 1/60 s (at most 30 presses
per second).

- 10-second test, started by the first press: the window is 600 frame samples (the one before
  the first press and 599 after), so it holds at most 599 edges and 300 presses; any 60 samples
  hold at most 59 edges / 30 presses ("Best 1s P/E").
- Presses, presses/s (presses / 10), edges, press-to-press and release-to-release interval mean
  and deviation (population, in frames), mean hold (press -> release) and gap (release -> press),
  duty = hold / (hold + gap), the last hold / gap, fastest interval, interval histogram, best
  10 s (presses and edges; kept across soft reset, guarded by a magic number). Intervals, holds
  and gaps longer than 30 frames are pauses: counted, but not in the means / deviations.
- Turbo check: per button the last period (press to press), presses/s, hold / gap, duty, the
  min-max of the last 8 periods, and a 30-frame trace.

Results live at fixed RAM addresses (`TestCartridgeSyms.inc`); `tests/test_testcart.cpp`
checks them with scripted input (e.g. alternate every frame for 60 frames -> 59 edges, 30
presses; press every 2 frames -> 30.0/s, 599 edges; intervals 3, 5, 3, 5 -> mean 4.00, deviation
1.00; ReplayNES turbo period / duty settings -> the same period / duty).

## Raster effects

- Palette chart: a sprite-0 hit at x=216 of line 22 starts a cycle-counted kernel. Each chart
  row is 12 lines = 1364 CPU cycles: 7 lines of colour, then rendering off; one palette entry
  (colour 3 of BG palettes 0-3) is written per horizontal blank, leaving the PPU address on a
  black entry; then the address is set to the next row (fine Y 0) and rendering comes back on in
  the horizontal blank. Timing (KD0 / KD5) is centred in the measured working range (±8 cycles).
- Split scroll: the frame starts at Y=208 (status bar rows 26-29), a sprite-0 hit on the bar's
  bottom line (x=200, line 31) is followed by $2006/$2005/$2005/$2006 so the last write lands in
  the horizontal blank.
- Kernels poll $2002 only when the sync sprite is surely in OAM, so they never spin into vblank
  (a $2002 read at the start of vblank can swallow the NMI).

## Building

```
python3 tools/testcart/build.py              # regenerate engine/src/testrom/TestCartridge*.inc
python3 tools/testcart/build.py --check      # CI / ctest: committed image == source
python3 tools/testcart/build.py --nes out.nes  # also write the ROM image
python3 tools/testcart/build.py -D KD0=100 --nes t.nes   # timing experiments
```

No external tools: `asm6502.py` is a small two-pass assembler (ca65-like syntax subset, local
`@labels`, `.byte/.word/.res/.align/.assert/.include/.macro`, and `.delay n` for cycle-exact
waits that refuses page-crossing loops). `chr.py` builds the CHR-ROM from `font.txt` (a 7x7
font with 2-pixel strokes) and generated tiles. The engine embeds the generated image
(`rn::buildTestCartridge`, `rn_write_test_cartridge`, `replaynes-cli make-test-rom --cartridge`).
