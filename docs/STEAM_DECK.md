# ReplayNES on Steam Deck / Linux

Status 2026-10-07: **skeleton** (plan `docs/plans/2026-10-07-steam-deck-plan.md`, Step 2). It records,
replays, rewinds, fast-forwards, pauses and steps with the same controller hotkeys as the macOS
app, with display-locked low-latency pacing. The CRT display (Vulkan port of the nesterm model),
MP4 export, the faster flash filter and full-speed emulation on displays slower than 60 Hz are in;
their menu UI (and library, timeline, practice, takes, settings, Japanese UI) follows in Step 3 on
the shared frontend core in `frontend/`. Until then: `--crt` / F4 toggles the CRT display, F6
exports the take to `~/Documents/ReplayNES/export-<time>.mp4` (F6 again cancels).

## Install

ReplayNES is a Flatpak (`io.github.replaynes.ReplayNES`, runtime `org.freedesktop.Platform` 25.08), so
nothing touches the read-only SteamOS system.

From a bundle (`dist/io.github.replaynes.ReplayNES-<version>-x86_64.flatpak`), in Desktop Mode
(Konsole):

```sh
flatpak install --user io.github.replaynes.ReplayNES-0.2.0-x86_64.flatpak   # fetches the runtime from Flathub
flatpak run io.github.replaynes.ReplayNES
```

From source (builds on the Deck itself; run on a Mac or Linux checkout with ssh access to the Deck):

```sh
scripts/build-linux-flatpak.sh                    # deck@steamdeck.local; or user@host, or HOST=local on Linux
```

It rsyncs the checkout to `~/ReplayNES-dev/src` on the Deck (never `roms/`, `.git`, build trees or ROM
files), installs `org.freedesktop.Sdk//25.08` and `org.flatpak.Builder` with `--user` if missing,
builds with flatpak-builder (the Nestopia core is fetched at the pinned commit), installs the app
`--user` and copies the bundle to `dist/`. `DEV=1` instead does an incremental SDK build in
`~/ReplayNES-dev/build-dev` (needs the pinned core cloned into `src/third_party/nestopia` on the Deck).

ROMs go into `~/Documents/ReplayNES/ROM` (created on first start; `*.nes`, also one folder level
down). The app only has access to `~/Documents/ReplayNES` (`--filesystem=xdg-documents/ReplayNES`).

## Gaming Mode (add to Steam)

1. Desktop Mode: Steam -> Games -> "Add a Non-Steam Game to My Library..." -> tick **ReplayNES**
   (listed from the Flatpak's desktop entry) -> Add Selected Programs. Steam stores
   `flatpak run ... io.github.replaynes.ReplayNES` as the target.
2. Back in Gaming Mode, start ReplayNES from the library. It opens full screen (it detects gamescope).
3. Controller: Steam Input's default gamepad layout works as is (Steam Input presents an Xbox-style
   pad; SDL reads it by button position).
4. Recommended: Quick Access -> Performance -> **Refresh rate 60 Hz** for this game on the OLED
   model (see "Pacing" below; the LCD model is 60 Hz anyway).
5. Artwork is optional (Steam shows the icon from the desktop entry).

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | NES D-pad |
| B (east) / A (south) | NES A / NES B (by position, as on macOS: Nintendo's A/B) |
| Y (north) / X (west) | turbo A / turbo B |
| Menu (≡) / View (⧉) | START / SELECT |
| R2 hold | rewind |
| L2 hold | fast-forward (recorded part only) |
| L | slow motion 1/2 on/off |
| R | pause / play |
| D-pad left/right while paused | step back / forward one frame (repeats when held) |
| R3 (right stick click) | menu (open ROM, record/play back, save, display options, quit) |

Keyboard: arrows, X = A, Z = B, S/A = turbo, Enter = START, right Shift or \\ = SELECT, Space pause,
Backspace rewind, Tab fast-forward, L slow, comma/period step, B bookmark, Esc/F1 menu, F11 full
screen, F3 stats overlay. In the menu: D-pad + A (gamepad) or arrows + Space (keyboard); B closes it.

Sessions: opening a ROM records into a temporary project
(`~/.var/app/io.github.replaynes.ReplayNES/data/ReplayNES/Session/current.nesrec`, autosaved every
3 s, fully saved on quit, including when Steam closes the game). "Resume last session" in the menu
reopens it. Opening another ROM moves a temporary project with recorded content into
`~/Documents/ReplayNES/Projects/` first.

## Pacing (how it works)

`apps/linux/src/main.cpp`, `display_scheduler.h`, `vk_renderer.cpp`; pure logic ported from the
macOS app in `apps/linux/src/interim/` (to be replaced by the shared core in Step 3). See
`docs/FRAME_PACING.md` for the macOS design this follows.

* Vulkan FIFO swapchain; every present carries a present id, and a waiter thread
  (`VK_KHR_present_wait`) records when each picture reached the screen. Those timestamps give a vblank
  grid (refresh estimate + phase).
* One emulated frame per display frame, aimed at a specific vblank: 60 Hz every refresh (emulation
  at 60.000 Hz), 120 Hz every 2nd, **90 Hz (Deck OLED default) alternating 2 and 1 refreshes**
  (exactly 60.000 Hz, but pictures stay 22.2 / 11.1 ms on screen: a regular 3:2 pattern, like film
  on 60 Hz), other rates take the nearest refresh. The audio follows with dynamic rate control
  (SDL audio stream frequency ratio, within +-0.5 %). Logical time is the frame index; the host
  clock never changes what is emulated.
* Input is sampled just in time: `target vblank - lead`, lead = p99.5 of the sample -> submit work +
  1.5 ms + a penalty learned from missed vblanks (it absorbs the compositor's latch point before
  the vblank). Frames may overlap (lead > one refresh) when the work is heavy.
* A FIFO backlog (pictures queued one refresh late after a hiccup) is drained by moving the targets
  one refresh later once.
* Controllers are updated on their own ~1 kHz thread: SDL's Steam Deck HIDAPI driver blocks ~8 ms
  every ~3 s (lizard-mode reports), which must not land on the input sample.
* Without `VK_KHR_present_wait` the loop falls back to the host clock at the NES rate (FIFO only,
  no just-in-time sampling).
* **Displays slower than 60.0988 Hz** (Gaming Mode's 40-59 Hz settings, nested gamescope, 30 Hz):
  every refresh shows a new picture and up to two frames are emulated for it, so emulation keeps
  the NES rate (shared core `rnf_frame_budget`: the emulated time follows the displayed time,
  45 Hz -> 1,2,1,1,2,1...; a stall is dropped, not caught up). The extra frame is emulated in the
  slack right after the previous present (its own, earlier time slot); the shown frame keeps its
  just-in-time input sample. The audio level keeps one more frame. Below 30.05 Hz emulation slows
  down (two frames per present at most).
* **CRT display**: the CRT passes are recorded before the show pass of the same present and their
  GPU time (timestamp queries) is added to the input lead's work, so the picture still makes its
  vblank. When the GPU p90 no longer fits what the maximum lead leaves (`rnf_build_ahead`, the
  macOS BuildAhead rule), pictures are built after their present and shown one frame later; when
  it exceeds 80 % of a frame period the tube resolution steps down (adaptive, x0.85, >= 512 wide;
  back up below 55 %).

## Measurements (Steam Deck OLED, SteamOS 3 / kernel 6.18, Mesa 26.2 RADV in the Flatpak GL extension, 90 Hz panel, 2026-10-07)

`scripts/perf-smoke-deck.sh` (runs `replaynes-linux --perf-seconds N` on the Deck over ssh; 20 s
warm-up, 30 s measured; Super Mario Bros. unless noted; "on screen" = vkWaitForPresentKHR
returned; judder = a picture whose time on screen differs from what the cadence intends by more
than half a refresh). Gaming Mode runs were started over ssh in the running gamescope session,
with the window given gamescope's focus (the script does this; a window without focus is not
shown, yet its presents still "complete").

| Run | sample -> on screen p50 / p99 (ms) | event -> on screen p50 | emulate+filter p50 | judder/min | audio underruns | CPU % (process) |
|---|---|---|---|---|---|---|
| Gaming Mode (gamescope, X11), 90 Hz 3:2 | 6.75 / 6.95 | - | 1.7 ms | 4 | 0 | 15.1 |
| Gaming Mode, injected B presses every 0.25 s | 5.70 / 6.35 | 12.8 | 1.7 ms | 8 | 0 | 15.2 |
| Gaming Mode, test ROM (flash filter heaviest path) | 12.54 / 12.81 | - | 6.4 ms | 0 | 0 | 39.9 |
| Desktop Mode (Plasma X11), full screen | 5.70 / 6.84 | - | 1.7 ms | 12 | 0 | 15.1 |
| Desktop Mode, window 1280x722 | 5.39 / 5.97 | - | 1.6 ms | 14 | 0 | 15.0 |
| Desktop Mode, full screen, injected presses | 6.77 / 7.81 | 13.8 | 1.6 ms | 20 | 0 | 15.1 |
| Desktop Mode, full screen, test ROM | 11.94 / 12.10 | - | 6.3 ms | 4 | 0 | 39.8 |
| Desktop Mode, full screen, installed Flatpak | 5.77 / 6.63 | - | 1.7 ms | 8 | 0 | 15.2 |

Emulated 59.97-60.00 fps in every run above, DRC ratio ~1.0016 (emulation at 60.000 Hz instead of
60.0988 Hz), audio level 26-40 ms. The intended 3:2 pattern itself is not counted as judder.

After the CRT / flash / sub-60 Hz work (same Deck, Gaming Mode, SMB unless noted, `BIN=dev`):

| Run | sample -> on screen p50 / p99 (ms) | emulated fps | judder/min | underruns | notes |
|---|---|---|---|---|---|
| CRT off, 90 Hz 3:2 | 6.35 / 6.76 | 60.00 | 4 | 0 | the latency target run |
| CRT on, integer scale (tube 960x720) | 16.04 / 16.62 | 59.99 | 8 | 0 | GPU p50 9.3 / p90 9.5 ms |
| CRT on, fill (tube 1144x858) | 17.41 / 17.94 | 60.00 | 4 | 0 | GPU p50 11.7 / p90 11.8 ms |
| CRT on, fill, build-ahead forced | 27.68 / 34.21 | 59.98 | 22 | 1 | `REPLAYNES_CRT_BUILD_AHEAD=1`; not chosen at this GPU time |
| test ROM (flash filter heaviest path) | 6.45 / 6.63 | 60.00 | 4 | 0 | emulate+filter 1.8 ms (was 6.4) |
| nested gamescope `-r 45`, CRT off | 8.35 / 9.03 | 60.03 | 8 | 0 | 45.0 Hz presents, 1131 of 2698 with 2 frames (60 s) |
| nested gamescope `-r 45`, CRT on (fill) | 17.02 / 18.02 | 60.04 | 4 | 0 | GPU p90 11.8 ms |

Nested gamescope only offers divisors of the 90 Hz panel (`-r 40` also ran at 45 Hz); Gaming
Mode's own 40-59 Hz settings need a Steam-launched game and were not measured. Earlier 45 Hz runs
(before the audio level fix, and one 45 s run after it) had 2-7 underruns around nested-compositor
stalls (45 ms present gaps).

Notes:
* The flash filter's heaviest path cost ~4.5 ms on the Deck's CPU before the 2026-10-07 rewrite
  (now 0.29 ms mean / 0.46 ms p99 in `replaynes-cli bench-flash`, bit-exact; docs/FLASH_REDUCTION.md) (powersave governor, bursty
  load keeps the clock low); the lead grows to cover it, so such pictures arrive ~12.5 ms after the
  sample instead of ~6 ms, still without judder or underruns.
* Before the controller thread, the HIDAPI stall caused a missed vblank every ~3.4 s; before the
  present-wait thread (one frame in flight) the heavy test ROM ran at 51 fps at 90 Hz.
* Not measured: 60 Hz panel rate (Desktop Mode exposes only 90 Hz on the OLED; in Gaming Mode the
  rate follows Steam's per-game setting, which needs a Steam-launched game). Nested gamescope
  (`gamescope -r 60` inside Plasma at 90 Hz) is not a useful proxy: its present timing runs at
  45 Hz and the app then emulates at 45 fps (see open issues).

Measure yourself:

```sh
scripts/perf-smoke-deck.sh 30                       # generated test ROM, the Deck's current session
ROM=smb WARMUP=20 INPUT=1 scripts/perf-smoke-deck.sh 30
EXTRA_ARGS=--fullscreen SESSION=desktop scripts/perf-smoke-deck.sh 30
```

Or from a Steam shortcut in Gaming Mode (launch options), then read the JSON line:

```
--rom "/home/deck/Documents/ReplayNES/ROM/<game>.nes" --perf-seconds 60 --stats-log /home/deck/Documents/ReplayNES/perf.jsonl
```

## Export (MP4)

The exporter (`apps/linux/src/export/`, the Linux port of the macOS `MP4Exporter`) renders the
active take on a fresh core (`rn_renderer`, the same frames and renderer hash as the macOS export)
and encodes H.264 High (GOP 120, BT.709 limited range, bit rate `rnf_export_video_bitrate`) + AAC
192 kbit/s 48 kHz mono into MP4 with FFmpeg from the runtime. Timestamps are exact: track timescale
39375000, frame f at `(f - start) * 655171`, so ffprobe reports `avg_frame_rate=39375000/655171`.
Encoder: `libx264` (from `org.freedesktop.Platform.codecs-extra`), else `h264_vaapi` (works on the
Deck, needs `--device=dri`), else `libopenh264`. Flash reduction and an optional CRT post-process
apply to the exported picture only. Headless tool (also in the Flatpak):

```sh
flatpak run --command=replaynes-export io.github.replaynes.ReplayNES \
    --project ~/Documents/ReplayNES/Projects/<name>.nesrec --out ~/Documents/ReplayNES/<name>.mp4 \
    [--start N --end N] [--preset 0-6] [--flash 0-3] [--par87] [--no-crop] [--verify-hash]
flatpak run --command=replaynes-export io.github.replaynes.ReplayNES --self-test /tmp/rn-export-test
```

`--crt` (also F6 in the app while the CRT display is on) applies the CRT model offline on a
window-less Vulkan device (`apps/linux/src/render/crt_export.cpp`, the macOS CRTExportRenderer):
same pipeline as the display, synchronous tube plan, frames strictly in order, so two exports are
identical (decoded `framemd5` equal) and the renderer hash is unchanged. On the Deck a 30 s SMB CRT
export at 1280x960 takes ~83 s (22 fps; GPU-bound).

Measured on the Deck OLED (2026-10-07, Platform 25.08 / FFmpeg 7.1, 1280x960): a 30 s (1800-frame)
Super Mario Bros. take exports in 8.2 s with libx264 (219 fps, 3.6x real time) and 9.8 s with
h264_vaapi; the self-test ROM's full-screen noise (worst case for the encoder) runs at 58 fps.
Renderer hashes are identical on the Deck and on macOS.

## Open issues

1. 40-59 Hz were verified in a nested gamescope at 45 Hz only (see Measurements); below 30.05 Hz
   emulation still slows down.
2. 90 Hz shows the 3:2 pattern; 60 Hz (QAM) is recommended until a measured comparison exists.
3. Gamepad navigation of the menu was verified with keyboard events (Space/arrows) over ssh; the
   built-in controls themselves need a hands-on check (Steam Input template, R3 menu).
4. Feature parity (library, timeline, practice, takes, settings + remap, ja) and the CRT / export
   menus are Step 3 (the hooks: `VkRenderer::setPostProcess` / `postProcessStatus`,
   `render/post_process.h`; `exportProject` / `ExportJob`, `export/mp4_export.h`;
   `crtExportProcessorFactory`, `render/crt_export.h`).
5. CRT at full screen costs ~11.8 ms of GPU per frame on the Deck (RADV, 1600 MHz): 60 fps holds,
   but sample -> screen grows to ~17.4 ms. See docs/CRT_PORT.md, "Vulkan port".
