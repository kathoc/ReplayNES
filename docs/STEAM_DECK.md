# ReplayNES on Steam Deck / Linux

Status 2026-10-07: **feature parity preview** (plan `docs/plans/2026-10-07-steam-deck-plan.md`,
Step 3) on the shared frontend core (`frontend/`): ROM library start screen, record / replay /
rewind / fast-forward / slow / step, filmstrip timeline with A/B ranges, takes, bookmarks, practice
(A/B repeat), Reset Project, autosave + resume, Save / Save As, settings with the controller
diagram, the CRT display (Vulkan port of the nesterm model), MP4 export, flash reduction, English /
Japanese, display-locked low-latency pacing (also full speed on displays slower than 60 Hz).

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
down). The app only has access to `~/Documents/ReplayNES` (`--filesystem=xdg-documents/ReplayNES`)
and to your Trash (`--filesystem=xdg-data/Trash`, for the "Reset Project" backup and projects
replaced by Save As).

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

## Using ReplayNES

**Start screen = Library.** The ROMs in `~/Documents/ReplayNES/ROM` are listed (search field at the
top; copy files in Desktop Mode with Dolphin, then "Reload" - the list also refreshes by itself
while it is on screen). Pick a ROM: **Play** creates the project
`~/Documents/ReplayNES/Projects/<ROM name> <date time>.nesrec` and starts recording at once
(autosaved); **Continue** under "Projects for This ROM" reopens one (matched by the ROM's SHA-256, so
renaming the ROM file is fine); **Try Without a Project** plays in a temporary session you can save
later. "Open Project…" (header) browses `~/Documents/ReplayNES`.

**Playing.** Nothing is drawn over the game while it runs (status badges appear only when paused,
rewinding, fast-forwarding, slow, practicing or reducing flashes). Pause (R1) and the **dock** appears
at the bottom: the glowing red **Record** toggle (record mode; gray = playback mode, which plays the
recorded take), go to start, rewind (hold), step back, play / pause, step forward, fast-forward
(hold), Slow, Practice, Integer / FILL and "···" (bookmark, previous take, re-record from here,
advance N frames, soft reset / power cycle). The dock is for touch / trackpad (a tap while playing
shows it for a moment); with the controller, paused D-pad ←/→ steps frames and R2 / L2 rewind /
fast-forward.

**The menu: R3 (press the right stick)**, or Esc / F1. The game pauses and nothing reaches it while
the menu is open. L1 / R1 switch tabs, the D-pad moves, A chooses, B goes back / closes the menu;
"▶ Resume" (top right) returns to the game, playing.

| Tab | What is there |
|---|---|
| Playback | The dock with the **filmstrip timeline**: one picture per 5 s at its natural size (10 / 20 / 40 s ... once the take outgrows the width), the newest picture revealed as you record, 1-pixel playhead (red while recording), bookmarks (yellow), A/B ranges (numbered colour bands). Touch / mouse: drag the strip to move (silent, paused), drag the thin band above it to set the A/B range of the selected section, drag a range's end to adjust it, tap a range to practice it. Controller: select the timeline and press A, then ◀ ▶ move the playhead (hold = faster), ▲ ▼ ±5 s, X / Y set A / B there, B ends. Below: section picker, "Set A Here", "Set B Here", "Practice This Section", "Add Bookmark". |
| Takes | "Re-record from Here", "Back to Previous Take", Reset Project…, and every take with its branch point ("Switch to This Take"). |
| Bookmarks | Add (also the B key), rewind to, rename (Steam's on-screen keyboard opens), delete. |
| Practice | The 8 A/B sections: set A / B, practice (A to B, a short hold, rewinds to A, repeats; nothing is recorded), rename, clear. While a practice run plays, a small pill shows the progress and "Stop Practicing". |
| Library | The start screen (switching ROM asks to save first, see below). |
| Settings | Display (Integer / FILL, 8:7, overscan, full screen, latency overlay, UI size, flash reduction, CRT display); Audio & Controls (volume, pause after rewind / fast-forward, autosave interval, paused D-pad stepping, language); Controller (a diagram of the connected pad - Steam Deck / Xbox / PlayStation / Nintendo / other layouts - where pressed buttons light up: select a button, press A and pick its action; "Reset Pad N to Defaults"); Game Input & Hotkeys (assign by pressing a key or button, turbo, SOCD, stick threshold). |
| Controls Guide | The tables below. |

Header: "Save" (a temporary session asks where, in a file chooser rooted at
`~/Documents/ReplayNES/Projects`; an existing project of the same name is moved to the Trash first)
and "Project ▾" (Save As…, Export…, Open Project…, Reset Project…, Close Project, Quit).

**Reset Project…** starts the project over from power-on (all takes, bookmarks and the take history
are deleted; "Keep A/B repeat sections" is on by default). A saved project is first copied and the
copy moved to the Trash (`~/.local/share/Trash`, freedesktop.org Trash specification), where Dolphin
can restore it.

**Autosave and resume** work like on macOS. The session is autosaved every 2 s (Settings) while you
play and whenever you pause. Quitting (the menu, closing the window, Steam closing the game) never
asks: the next start reopens the last project or temporary session, paused where you were
("Resumed where you left off"). A temporary session lives in
`~/.var/app/io.github.replaynes.ReplayNES/data/ReplayNES/Session/current.nesrec` (`resume.json` next to
it). Starting another ROM or project asks "Do you want to save?" when a temporary session has
recorded content (Save… / Don't Save) or a project has unsaved changes; a temporary session left
from an earlier run is offered for saving first. A second running copy of ReplayNES neither resumes
nor keeps sessions.

**Language:** Japanese when the system's first language is Japanese, English otherwise
(`REPLAYNES_LANG=ja|en` overrides it). Japanese text uses the system's CJK font through fontconfig
(SteamOS: Noto Sans CJK); without one the UI stays in English.

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | NES D-pad |
| B (east) / A (south) | NES A / NES B (by position, as on macOS: Nintendo's A/B) |
| Y (north) / X (west) | turbo A / turbo B |
| Menu (≡) / View (⧉) | START / SELECT |
| R2 hold | rewind |
| L2 hold | fast-forward (recorded part only) |
| L1 | slow motion 1/2 on/off |
| R1 | pause / play |
| D-pad left/right while paused | step back / forward one frame (repeats when held) |
| R3 (right stick click) | the ReplayNES menu (reserved: cannot be assigned) |

In the menu: D-pad move, A choose, B back / close, L1 / R1 tabs, Start close.

Keyboard: arrows, X = A, Z = B, S/A = turbo, Enter = START, right Shift or \\ = SELECT, Space pause,
Backspace rewind, Tab fast-forward, L slow, comma/period step, B bookmark, Esc/F1 menu, F11 full
screen, F3 statistics overlay. Every assignment can be changed in Settings (saved in
`~/.var/app/io.github.replaynes.ReplayNES/config/ReplayNES/bindings.json`, the macOS format).

## UI and latency

The UI is Dear ImGui drawn in the same Vulkan render pass as the game picture (one present per
frame; gamescope composites the window as a whole anyway). While playing it costs ~0.05 ms per frame
(an empty ImGui frame; badges and the practice pill are a few quads), and ImGui does not read the
gamepads at all (its polling would contend with the controller thread): the controllers drive ImGui
only while the menu, the library or a dialog is up, and the game is paused then. Work that is not
frame-critical runs after the present, in the slack before the next input sample: autosave (while
playing only with >= 8 ms of slack), the resume record (written by a background thread), library
scan results and filmstrip thumbnails (pictures the game shows anyway; missing ones are rendered
from the take's checkpoints on idle-priority worker threads).

## Pacing (how it works)

`apps/linux/src/main.cpp`, `display_scheduler.h`, `cadence.h`, `vk_renderer.cpp`; the decisions
shared with macOS come from the frontend core (`rnf_refreshes_per_frame`, `rnf_input_deadline` with
Linux limits, `rnf_audio_rate`); `cadence.h` adds the 3:2 lock and the refresh estimate from
present timestamps. See `docs/FRAME_PACING.md` for the macOS design this follows.

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

After the Step 3 UI (feature parity on the shared core, UI hidden while playing; installed Flatpak,
Gaming Mode, 90 Hz 3:2, SMB, 20 s warm-up + 30 s, no other instance running - the script now
refuses to start / warns when one is):

| Run | sample -> on screen p50 / p99 (ms) | event -> on screen p50 | ui stage p50 | judder/min | audio underruns | CPU % (process) |
|---|---|---|---|---|---|---|
| UI build, CRT off | 5.54 / 5.79 | - | 0.05 ms | 0 | 0 | 14.5 |
| UI build, injected B presses | 4.54 / 5.01 | 13.1 | 0.05 ms | 4 | 0 | 14.2 |
| UI build, `--crt` (960x720 RF, GPU p50 9.4 ms) | 15.31 / 15.49 | - | - | 0 | 0 | 16.1 |
| A/B same session: UI build (2 runs) | 4.68 / 5.15, 5.19 / 5.36 | - | 0.05 ms | 4, 0 | 0 | 15.2-15.5 |
| A/B same session: Step 2 skeleton (2 runs) | 6.29 / 6.89, 6.26 / 7.31 | - | 0.06 ms | 8, 12 | 0 | 14.9-15.0 |

Runs made while another ReplayNES measurement ran at the same time (two windows fighting for
gamescope's focus) showed the input lead running away to 15-22 ms and ~1000 judder/min for both
builds; those are invalid, hence the guard in `scripts/perf-smoke-deck.sh`.

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

In the app: menu -> "Project ▾" -> **Export…** (codec / encoder, size preset, overscan crop, 1:1 or
8:7, flash reduction, CRT effect, whole take or a frame range, file name; the MP4 goes to
`~/Documents/ReplayNES/Exports/`). The export runs on a worker thread with a progress bar and
Cancel; "Run in Background" returns to the game (a small progress badge stays top right).
The CRT display itself is Settings -> Display -> CRT Display (`--crt` turns it on for one run, for
measurements).

`--crt` of `replaynes-export` (and "Apply CRT Effect" in the app) applies the CRT model offline on a
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
3. Gamepad navigation was verified with scripted ImGui gamepad events and screenshots of the
   presented frames (`scripts/ui-check-deck.sh`); a hands-on pass with the built-in controls
   (Steam Input template, R3, holding A on the rewind / fast-forward buttons) and Steam's on-screen
   keyboard for renaming (SDL_StartTextInput from the Flatpak) is still to do.
4. Japanese needs a CJK font on the host (SteamOS has Noto Sans CJK); no font is bundled. Without
   one the UI stays in English.
5. CRT at full screen costs ~11.8 ms of GPU per frame on the Deck (RADV, 1600 MHz): 60 fps holds,
   but sample -> screen grows to ~17.4 ms. See docs/CRT_PORT.md, "Vulkan port".
