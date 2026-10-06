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
4. **Budget.** In a large-area event, a block may make its transition only while its window still
   has budget. With budget ≥ 4 the last unit is reserved for a transition towards *darker*, so a
   suppressed screen rests on its darker state (this is the "dimming" behaviour). If only a few
   blocks (≤ 10 % of the screen, with ≥ 4× as much area allowed, and < 20 % of all blocks over
   budget) are out of budget, they follow the majority, avoiding stale patches after scene cuts.
5. **Suppress.** A block out of budget is blended in linear light between the previously displayed
   block and the new one, with the largest weight (found by bisection on the quantised result)
   that keeps it from making a transition: the opposing move from its extremum stays within the
   *hold limit* and the per-frame change within the *rate limit*. The block stays rate-limited for
   a few "sticky" frames so residual flicker remains tiny. Everything else is copied unchanged.
6. **Reset** clears all state; the next frame is shown as is and becomes the new reference.

## Levels

| Level | Transition threshold (L / red ×320) | Darker < 0.8 / red saturation required | Budget per 61 frames | Large area | Hold / rate (L) |
|---|---|---|---|---|---|
| Off | – | – | – | – | output = input |
| Low | 0.10 / 20 (WCAG) | yes / yes (WCAG) | 6 transitions (3 flashes) | 25 % | 0.06 / 0.02 |
| **Standard (default)** | 0.08 / 16 | no / no | 4 (2 flashes) | 20 % | 0.04 / 0.01 |
| High | 0.06 / 12 | no / no | 2 (1 flash) | 15 % | 0.02 / 0.005 |

The default is Standard (on): safety first. Low follows WCAG literally; Standard and High detect
earlier and allow fewer flashes than WCAG permits.

## Use in the macOS app

* Settings → Display & Audio → "Flash Reduction": Off / Low / Standard / High, with an explanation and the
  disclaimer; optional viewport indicator "Flash Reduction Active" while the picture is being altered.
* Live display: frames are filtered on the emulation thread before they are handed to Metal.
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

* full-screen black/white at 30 Hz: Low ≤ 3, Standard ≤ 2, High ≤ 1 flashes/s; also 7.5 Hz,
  mid-grey pairs, half-screen and 4.5 Hz sinusoidal flashing ≤ 3 flashes/s;
* red flashes (grey↔red with equal luminance, black↔red, dark↔bright red) ≤ 3 red flashes/s;
* static, horizontally (1–5 px/frame) and vertically scrolling scenes, a 4-step fade and a single
  scene cut: output bit-identical to input at every level;
* sprite blink (16×16) at every level and 15 % area blinking (aligned or not, B/W or red) at
  Low/Standard: unaltered;
* determinism, reset, in-place processing, C API errors; session state/video/audio hashes are
  identical with and without the filter running;
* with the developer's own ROMs in `roms/` (gitignored): Super Mario Bros. (title, play, pause)
  and Gradius (stage 1 with shooting) — Standard alters ≤ 2 of 3000 / 0 of 6000 frames (a 2-frame,
  2-block hold during the boot sequence). `RN_FLASH_DUMP_DIR=<dir>` writes side-by-side PPM
  pictures of altered frames for inspection.

## Known limitations

* Block-based (16×16): a suppressed region has block-shaped edges while it is held; a flash
  covering just under the area threshold is not limited (by design, to keep small flicker intact).
* Patterns (stripes, checkerboards) and their motion, and rapid colour changes other than
  saturated red, are not analysed.
* A held picture can lag behind the game by up to about one second after a flash burst.
* At High, quick sequences of large scene changes (e.g. boot screens) may be shown with a delay.
* The thresholds assume the picture fills the view; on a very large or very close display the
  real visual angle of the flashing area is larger.
