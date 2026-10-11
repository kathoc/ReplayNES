# Photosensitive flash reduction (Flash Reduction)

ReplayNES can limit large-area flashing in the picture it **displays** (and, optionally, in
exported video), to reduce the risk for people sensitive to flashing light.

> **Disclaimer.** This is a mitigation, not a medical device. It cannot guarantee that no
> photosensitive seizure is triggered. Stop playing immediately if you feel unwell.

## Scope and guarantees

* Display-side only. The filter (`engine/src/video/FlashFilter.*`, C API `rn_flash_filter_*`)
  reads a copy of the emulated frame and writes another buffer. Emulation, recorded input,
  checkpoints, projects and every verification hash (`rn_state_hash`, `rn_video_hash`,
  `rn_renderer_hash`) are unaffected (tested in `tests/test_flash_filter.cpp` and
  `apps/macos/Tests/LibraryTests.swift`).
* Deterministic: a pure function of the frame sequence it is given plus the level. All arithmetic
  is integer (16-bit linear light), so the output is bit-identical on every platform.
* Portable: lives in the engine so Windows/Linux frontends reuse it (see PORTING.md).
* Normal content is passed through **bit-exactly**: only blocks that are being suppressed are
  changed.

## Reference: WCAG 2.x flash thresholds

* General flash: a pair of opposing changes in relative luminance of ≥ 10 % of the maximum, where
  the darker state is below 0.80, more than **3 times in any one-second period**, over a combined
  area larger than ~25 % of the field of view (for a game filling the screen we use 25 % of the
  screen).
* Red flash: a pair of opposing transitions involving saturated red: `R/(R+G+B) ≥ 0.8` in either
  state and a change of `(R−G−B)×320` (negatives as 0) greater than 20, linear R, G, B.

Relative luminance `L = 0.2126 R + 0.7152 G + 0.0722 B` with sRGB-linearised channels.

## Algorithm

1. **Measure.** The 256×240 frame is divided into 16×15 blocks of 16×16 px. Per block: mean
   relative luminance `L`, mean red metric `M = mean(max(0, R−G−B))` (×320 = WCAG scale) and
   whether the block-mean colour is saturated red.
2. **Count transitions on what is shown.** Per block and per metric, an extremum tracker follows
   the *displayed* values: a move of at least the threshold against the previous direction is a
   transition (a flash = two opposing transitions). Luminance and red seeing the same swing count
   once. Transition frames are kept for a sliding **61-frame** window (> 1 s at 60.0988 Hz).
3. **Large-area test.** If the blocks that would make a transition this frame (plus those that made
   one in the previous 2 frames, same direction) cover at least the level's area share — measured
   in pixels that really change, against the previous frame and against the frame at the block's
   last extremum (so gradual fades count too) — the frame is a large-area flash candidate. Smaller
   areas (sprite blinking, scrolling details, small explosions) are never altered.
4. **Pairing: what is a flash.** A flash is a pair of *opposing* transitions close together in
   time. The same extremum tracking also runs on the **input** of every block; a transition counts
   toward the budget only when it pairs up — the input made opposing transitions at most
   `pairFrames` apart (and the latest of them at most `pairFrames` ago), or the block's previous
   displayed transition is at most `pairFrames` ago — and then the transition it pairs with counts
   too. Everything else passes untouched and is never held:
   * an isolated scene cut, or a fade that comes to rest on its new level;
   * screen changes through black (fade out, black, fade in) when the black and the screens last
     longer than `pairFrames`;
   * alternation slower than the level's limit (with `pairFrames` = ⌊30.5 / limit⌋ frames, an
     alternation whose transitions are more than `pairFrames` apart cannot exceed the limit).

   Why not "gradual transitions do not count": a 4.5 Hz sinusoidal flash is gradual too (WCAG counts
   it, and so must we). What separates a fade from a flash is that a fade *stays* on its new level;
   that is exactly what the pairing window measures. Once the input stops alternating, a held block
   is released `pairFrames` later (plus the short sticky smoothing), not when the 61-frame window
   has emptied, so the picture no longer lags behind the game for up to a second after a flash.
5. **Budget.** In a large-area event, a block may make a paired transition only while its window
   still has budget. With budget ≥ 4 the last unit is reserved for a transition towards *darker*, so a
   suppressed screen rests on its darker state (this is the "dimming" behaviour). If only a few
   blocks (≤ 10 % of the screen, with ≥ 4× as much area allowed, and < 20 % of all blocks over
   budget) are out of budget, they follow the majority, avoiding stale patches after scene cuts.
6. **Suppress.** A block out of budget is blended in linear light between the previously displayed
   block and the new one, with the largest weight (found by bisection on the quantised result)
   that keeps it from making a transition: the opposing move from its extremum stays within the
   *hold limit* and the per-frame change within the *rate limit*. The block stays rate-limited for
   a few "sticky" frames so residual flicker remains tiny. Everything else is copied unchanged.
7. **Reset** clears all state; the next frame is shown as is and becomes the new reference.
8. **CRT codes.** With the CRT effect the picture is drawn from the PPU's palette codes through an
   RF / composite model. `rn_flash_filter_process_codes` filters the codes along with the picture:
   unchanged pixels keep their codes; a pixel the filter changed is encoded back into codes — the
   colour of every code is learned from the frames passed in, and the pixel takes, through a fixed
   16×16 ordered dither, the nearest learned colour at or below and at or above its luminance,
   weighted so that each block's mean light matches the reduced RGB picture. The CRT therefore
   stays on its RF path while flashes are reduced (before, every reduced frame switched the whole
   screen to the RGB input and back, so the CRT look itself flickered).

## Levels

| Level | Transition threshold (L / red ×320) | Darker < 0.8 / red saturation required | Budget per 61 frames | Pairing window | Large area | Hold / rate (L) |
|---|---|---|---|---|---|---|
| Off | – | – | – | – | – | output = input |
| Low | 0.10 / 20 (WCAG) | yes / yes (WCAG) | 6 transitions (3 flashes) | 10 frames | 25 % | 0.06 / 0.02 |
| **Standard (default)** | 0.08 / 16 | no / no | 4 (2 flashes) | 15 frames | 20 % | 0.04 / 0.01 |
| High | 0.06 / 12 | no / no | 4 (2 flashes) | 15 frames | 15 % | 0.02 / 0.005 |

The default is Standard (on): safety first. Low follows WCAG literally; Standard detects earlier
and allows fewer flashes than WCAG permits. High allows as many flashes as Standard but reacts to
smaller areas and smaller changes and smooths more strongly. (Until version 0.5.x High allowed one
flash per second; together with counting every transition that held ordinary screen changes —
a fade through black is already one flash — for up to a second, showing stale picture blocks.)

## Use in the macOS app

* Settings → Display & Audio → "Flash Reduction": Off / Low / Standard / High, with an explanation and the
  disclaimer; optional viewport indicator "Flash Reduction Active" while the picture is being altered.
* Live display: frames (and their PPU codes, for the CRT) are filtered on the emulation thread
  before they are handed to Metal.
  Only frames that are actually shown are processed (fast-forward: the last of each tick), so the
  1-second window is wall-clock based. While paused / in slow motion, a frame that is being held
  back is re-filtered every display tick so it settles on the real frame within the budget.
* Reset on: new session / project load, seek, scrub, bookmark jump, take switch, "Back to Previous Take",
  1-frame step back, start of a rewind (a rewind itself is a continuous, filtered sequence).
* Export sheet: "Apply Flash Reduction" (default = on when the setting is not Off) applies the
  current level (Standard if the setting is Off) to the exported video only, with a fresh filter.

## Verification

`tests/test_flash_filter.cpp` measures the output with an **independent** WCAG counter (double
precision, 8×8 px cells, own sRGB conversion; flashes = ⌈transitions/2⌉ shared by ≥ 25 % of the
screen in any 60-frame window):

* full-screen black/white at 30 Hz: Low ≤ 3, Standard ≤ 2, High ≤ 2 flashes/s; also 7.5 Hz,
  mid-grey pairs, half-screen and 4.5 Hz sinusoidal flashing ≤ 3 flashes/s;
* red flashes (grey↔red with equal luminance, black↔red, dark↔bright red) ≤ 3 red flashes/s;
* static, horizontally (1–5 px/frame) and vertically scrolling scenes, a 4-step fade and a single
  scene cut: output bit-identical to input at every level;
* sprite blink (16×16) at every level and 15 % area blinking (aligned or not, B/W or red) at
  Low/Standard: unaltered;
* determinism, reset, in-place processing, C API errors; session state/video/audio hashes are
  identical with and without the filter running;
* not held at any level (pairing): a Super Mario Bros. 3-like world-map entry (bright title, cut
  to the dark map, 4-frame fade steps whose hue goes orange then pale), menus with fades through
  black about 3 times per second, two screens cut every 20 frames, and the Test Cartridge's
  palette chart with greyscale / emphasis pressed every 20 frames (the previous rule held these
  for up to 161 frames at High);
* once the input stops alternating, the output equals the input again (bit-exact) after
  `pairFrames` (+ the sticky smoothing; + another `pairFrames` when the scene scrolls);
* PPU codes: codes of passed pixels unchanged; reduced blocks' code pictures match the reduced
  RGB block means (full-screen strobe: within 3/256; the legacy test ROM in the desktop
  controller: within 0.02); codes never change the RGB output; in place == separate buffers;
* with the developer's own ROMs in `roms/` (gitignored): Super Mario Bros. (title, play, pause)
  and Gradius (stage 1 with shooting) — Standard alters 12 of 3000 / 0 of 6000 frames (2 blocks
  for 12 frames: the title screen's one-frame change to sky blue before the black screen is a
  real flash on the blocks with the logo and the bush). `RN_FLASH_DUMP_DIR=<dir>` writes side-by-side PPM
  pictures of altered frames for inspection.

`tests/test_flash_filter_exact.cpp` checks that the optimised implementation (cached per-pixel
metrics, deduplicated blending) is **bit-exact**: output pixels and frame info, every frame, equal
those of a frozen copy of the original per-pixel implementation (`tests/support/FlashFilterRef.*`)
over long synthetic flashing sequences (all levels, resets, in-place) and the generated test ROM.
The reference was rebased once, on purpose, when the pairing rule changed what counts as a flash:
the same rule (input trackers, counted transitions, `pairFrames`, High's budget) was added to it in
its straightforward form; the per-pixel measuring and blending it exists to check are unchanged.

Cost: `replaynes-cli bench-flash <rom> [frames] [level]` times `rn_flash_filter_process` alone.
On the generated test ROM (large-area flashing on most frames) at Standard: about 0.17 ms/frame
on an M1 Max and 0.3 ms/frame on a Steam Deck (previously 1.6 / 3.3 ms). With the PPU codes
(`--codes`, CRT on) about 0.3 ms/frame on an M1 Max.

## Known limitations

* Block-based (16×16): a suppressed region has block-shaped edges while it is held; a flash
  covering just under the area threshold is not limited (by design, to keep small flicker intact).
* Patterns (stripes, checkerboards) and their motion, and rapid colour changes other than
  saturated red, are not analysed.
* A held picture shows the previous picture of the held blocks (block-shaped) while the game
  keeps flashing; it catches up `pairFrames` after the flashing stops (up to a second when the
  flashing goes on).
* Two flashes in quick succession can still be reduced when they are real (e.g. a boot sequence
  that blinks green, black and then the title within a second).
* Alternation slower than the pairing window is not reduced even when it goes on: at most the
  level's flash rate (Low 3/s, Standard and High 2/s).
* The thresholds assume the picture fills the view; on a very large or very close display the
  real visual angle of the flashing area is larger.
