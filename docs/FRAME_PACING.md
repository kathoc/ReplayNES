# Frame pacing and input latency (live play)

Status: adopted 2026-10-06 (macOS). Replaces the host-clock tick + present thread +
`present(atTime:)` lead of 0.2.x. Code: `apps/macos/Sources/App/EmulationController.swift`
(loop), `MetalView.swift` (render / present), `Sources/Core/DisplayPacing.swift` (pure logic:
`DisplayCadence`, `InputDeadline`, `AudioRateControl`), `Sources/Core/AudioResampler.swift`,
`Sources/Core/C/rn_frame_workgroup.c`, `App/FullScreenChrome.swift`. Measured with
`scripts/perf-smoke.sh`.

## 1. Goal and what is measured

Input that reaches the screen as early as the display allows, every emulated frame on screen for
exactly the same time (no judder), low CPU, nothing given up (rewind, slow motion, fast-forward,
frame step, practice, filmstrip, CRT, flash reduction, Syphon).

`scripts/perf-smoke.sh` runs the real app (temporary session/library roots, preferences only via
launch arguments) and logs one CSV row per presented frame (`--frame-log`):

| Stage | From → to |
|---|---|
| JIT wait | display callback → input sample (deliberate: sample as late as possible) |
| emulate | input sample → `rn_step` returned |
| render | `rn_step` returned → command buffer committed (flash filter, copy, texture upload, encode) |
| commit → on screen | commit → `MTLDrawable.presentedTime` (GPU, compositor, display pipeline) |
| **input sample → on screen** | the pipeline latency we control |
| input event → on screen | `NSEvent.timestamp` / GameController `lastEventTimestamp` → on screen (INPUT=1 injects key presses; includes the wait for the next frame's sample, ~8 ms on average for real input) |

Judder = consecutive frames whose time on screen differs from the steady one by more than half a
refresh (each picture's first appearance, including a repeat present when its own drawable was
dropped). Also: frames never shown, dropped drawables, audio underruns and resampling ratio, CPU
seconds per thread (emulation thread, main thread, process), work per frame.

## 2. What the display path costs on macOS (M1 Max, 120 Hz ProMotion, macOS 27)

Measured first with a stand-alone probe (CAMetalDisplayLink, synthetic 0.6 ms of work, sample
2 ms before the deadline):

| Path | deadline → on screen | input sample → on screen |
|---|---|---|
| Window (composited), CAMetalDisplayLink | 4 refreshes = 33.3 ms | 35.9 ms, 0 judder |
| Window, own drawables presented ASAP | 2-3 refreshes | 19-21 ms, **400-1100 judder/min** |
| Window, `present(atTime:)` (old design) | ~3-4 refreshes | 36-44 ms, judder |
| Full screen, layer covering the screen, nothing above it (**direct to display**) | 1 refresh = 8.3 ms | **10.9 ms**, 0 judder in short runs |
| Full screen with any view above the layer, or a layer smaller than the screen | 2 refreshes | 19-21 ms |
| Full screen, free-running at 60.0988 Hz (VRR) | variable | 9.3 ms mean, judder (presents quantised to 4.17 ms) |

Consequences:
* The window compositor costs 3 refreshes; no presentation trick removes them without judder.
  Lowest latency needs full screen with nothing drawn over the game layer (macOS then flips the
  layer directly; the Metal HUD shows "Direct").
* ProMotion cannot show 60.0988 Hz evenly (variable refresh quantises to 240 Hz steps), so the
  emulation follows the display instead (60.000 Hz) and the audio is resampled.
* Committing later than ~1.5 ms before the display link's `targetTimestamp` sometimes misses.

## 3. Approaches

**A. Display-locked emulation (adopted).** A `CAMetalDisplayLink` on the emulation thread's
run loop (preferred 120 Hz, frame latency 1). `DisplayCadence` starts a frame on every k-th
refresh when the refresh rate is a multiple of the NES rate within 0.5 % (120 Hz: every 2nd,
60 Hz: every one, 240 Hz: every 4th), otherwise picks the nearest refresh (144 Hz, 75 Hz: mixed
cadence is unavoidable there). Such a callback runs queued commands, waits until
`targetTimestamp − lead`, samples input, emulates one frame, renders it into the link's drawable
and presents it for exactly that refresh; autosave, thumbnail capture and status follow after the
commit. The other refreshes present the same picture again ("repeat"): presenting at a constant
120 Hz keeps the compositor and the ProMotion panel in a steady cadence (without repeats the
window path judders 120-210/min and full screen far more). Logical time is the frame index:
determinism, recordings and export are unaffected; pause, frame advance, slow 1/2 / 1/4, rewind,
fast-forward and practice are per-frame decisions as before. Without a visible viewport
(minimised, occluded) the thread keeps time on the host clock at 60.0988 Hz.
* `InputDeadline`: lead = 99.5th percentile of the sample→commit work over ~10 s + 1.5 ms,
  plus a penalty (+0.5 ms, ≤ 3 ms, decaying) for commits closer than 0.5 ms to the deadline.
  Typical lead 2.3-2.6 ms; 4.6 ms on a flash-heavy picture (flash filter ~2 ms worst case).
* `AudioRateControl` + `AudioResampler` (RetroArch-style DRC): ratio = nominal/actual frame rate
  × (1 ± ≤ 0.5 %) from the smoothed ring level (target 21 ms), 4-point Hermite interpolation.
  Measured ratio 1.0016 at 60.000 Hz, 0 underruns, level 24-32 ms.
* Frame workgroup (`rn_frame_workgroup.c`, `AudioWorkIntervalCreate` + `os_workgroup_interval`
  start/finish with the frame's deadline): the burst of ~1-2 ms every 16.7 ms otherwise runs at
  a low CPU clock. Bench: mean 4.2 → 2.1 ms, p99 7.9 → 2.8 ms; in the app on a flash-heavy
  picture the lead drops from 10 ms (cap) to 4.6 ms (-4 ms latency) and the emulation thread
  CPU from 16 % to 14 %.
* Display-link quirk: Core Animation sometimes delivers an update on the main thread (during the
  main thread's transaction flush while the layer geometry changes). Those updates are skipped
  (they would race the emulation thread); `foreignCallbacks` counts them (0-5 per minute of
  layout changes, 0 in steady play).
* Window compositor drops: ~0.1-0.5 % of new-frame drawables are never shown in a window (a
  repeat shows the picture one refresh later); independent of how early the frame is committed
  (still there with a 9 ms lead), so it does not raise the lead.

**B. Run-ahead (measured, not implemented).** Core cost per displayed frame (internal C++
bench, Nestopia on M1 Max): step 0.35-0.39 ms, saveState 8 µs, loadState 4 µs; run-ahead 1 =
0.72-0.79 ms, run-ahead 2 = 1.04-1.15 ms per frame. It removes 1-2 frames (16.7-33 ms) of the
game's own input lag and costs ~0.4 ms of lead per frame of run-ahead. It needs a new engine C API
(a display-only step on a second core instance / state round trip that never touches the
session or timeline), i.e. a public API decision; recording would still store the real input at
the real frame. Recommended next step, optional per game (low-end phones).

**C. Compositor path (adopted for full screen).** Full screen hides everything drawn over the
game while playing (transport bar, sidebar, toolbar, badges, practice panel, latency overlay;
`FullScreenChrome`): the layer covers the screen, opaque, no transform. Pointer movement or
pausing brings the chrome back (composited again while visible). `-fullScreenAutoHide NO` keeps
it. Layer: opaque, `framebufferOnly`, `displaySyncEnabled`, `presentsWithTransaction = false`,
3 drawables (the link needs them at 120 Hz).

**D. UI isolation (done).** Nothing in the frame path waits for the main thread: commands are a
locked queue drained before the JIT wait, status is posted asynchronously, the thumbnail capture
and autosave run after the commit. Keyboard events still arrive through the main thread (their
own timestamps are used for the measurement); controllers use their own queue. The filmstrip
now keeps one thumbnail per 5 s of game time (then 10, 20, 40 s ... as the timeline scale doubles):
live capture at most once per 5 s, a fraction of the background generation. Thumbnails on vs
off (`-filmstripThumbnails NO`) made no measurable difference to latency, judder or CPU in the
long take, so they stay.

**E. Rejected / not needed.** CVDisplayLink (CAMetalDisplayLink gives the drawable and both
deadline and presentation time); `present(atTime:)` scheduling (old design: latency and judder);
VRR free-run at 60.0988 Hz (judder); skipping repeats to drain a queued drawable on every late
present (destabilises a variable-refresh panel; kept only as a rare drain of a steady one-drawable
backlog, at most every 5 s).

## 4. Results (M1 Max, built-in 120 Hz ProMotion, Super Mario Bros. unless noted; 60 s after 8 s
warm-up; "long" = after 150 s of recording with the filmstrip growing; ms; judder per minute)

Old = 0.2.x host-clock pacing + present thread (built from the same instrumentation, `-framePacing
hostClock` of commit 21fc20e). New = this design. Test ROM = generated ROM whose picture keeps the
flash filter on its heaviest path (worst case for the per-frame work).

| Run | sample→screen p50 / p99 | event→screen mean | emulate + render mean (p99) | lead | judder/min | dropped | audio underruns | CPU emu / main / process % |
|---|---|---|---|---|---|---|---|---|
| Old, window | 36.0 / 40.1 | 47.3 | 1.32 (2.7) | – | 44.0 | 0 | 0 | 6.6 / 6.0 / 19.1 |
| **New, window** | 35.8 / 37.9 | 47.6 | 0.67 (0.76) | 2.3 | 8.9 | 4 | 0 | 5.0 / 6.7 / 20.6 |
| Old, full screen | 41.4 / 46.0 | 48.9 | 2.39 (4.4) | – | 121.0 | 0 | 0 | 11.7 / 1.3 / 24.8 |
| **New, full screen (chrome hidden)** | 19.3 / 21.2 | 35.4 | 0.68 (0.97) | 2.5 | 9.8 | 6 | 0 | 4.9 / 1.9 / 18.3 |
| New, full screen, chrome visible | 19.3 / 21.3 | 30.6 | 0.71 (1.0) | 2.6 | 125.9 | 75 | 0 | 5.2 / 10.4 / 26.2 |
| Old, window, test ROM | 38.0 / 42.1 | 48.2 | 3.24 (8.9) | – | 23.6 | 0 | 0 | 18.7 / 4.6 / 28.0 |
| New, window, test ROM | 38.0 / 38.3 | 47.3 | 2.13 (3.1) | 4.6 | 8.9 | 4 | 0 | 13.9 / 7.3 / 29.3 |
| New, window, test ROM, no workgroup | 41.6 / 41.6 | 50.6 | 2.47 (6.7) | 10.0 | 3.9 | 2 | 0 | 16.0 / 5.2 / 27.0 |
| Old, window, test ROM, long | 41.0 / 45.9 | 50.9 | 3.13 (6.9) | – | 15.7 | 0 | 0 | 18.0 / 4.5 / 27.3 |
| New, window, test ROM, long | 37.9 / 37.9 | 50.8 | 2.11 (3.1) | 4.6 | 25.6 | 8 | 0 | 13.6 / 7.8 / 29.8 |
| New, window, test ROM, long, no thumbnails | 37.9 / 37.9 | 51.8 | 2.14 (3.1) | 4.6 | 16.7 | 8 | 0 | 13.8 / 6.7 / 27.9 |
| New, full screen, test ROM, long | 21.1 / 21.2 | 35.7 | 2.02 (2.9) | 4.5 | 14.8 | 7 | 0 | 12.7 / 2.0 / 25.3 |

Reading the table:
* Full screen: input sample → screen 41 → **19 ms**, judder 121 → **10 per minute**; the
  emulation thread uses less than half the CPU (no present thread, no hand-off, clock hint).
* Window: the compositor's 33 ms dominate; the new path makes it constant (p99 − p50 = 2 ms
  instead of 4-10 ms) and cuts judder 3-5x.
* Event → screen adds the wait for the next frame's input sample (frames start every 16.7 ms; the
  injected presses are not uniformly distributed, real input averages ~8 ms).
* Full screen reached 19 ms instead of the probe's 10.9 ms: the game layer stayed composited
  (deadline → screen = 2 refreshes). Cause found afterwards: SwiftUI kept the (hidden) title-bar
  safe area, so the layer was 3024x1794 in a 3024x1898 full-screen window; immersive play now
  ignores the safe area and drops the background view (layer 3024x1898, like the probe). Any
  other window over the screen (a permission prompt, a Notification Center banner) also forces
  composition; with such a window present even the probe measures 19.2 ms. When direct, a
  one-drawable backlog can also appear after a hiccup (+1 refresh); a rare drain removes it.
* Measurements share the machine with the user's other apps (video playback in a browser during
  several runs); the window figures for judder vary between runs (4-60/min) with that load.

## 5. Budget on smaller devices

Per frame on M1 Max (P-core, with the workgroup hint): emulate 0.35-0.45 ms, flash filter
0.1 ms typical / 2 ms worst case (large flashing areas), copy + texture upload + encode 0.25 ms,
GPU for the plain picture negligible (CRT model: several passes, measure per device). An A15-class
iPhone core is roughly 0.8x, an A12-class one ~0.5x: ~1-1.5 ms typical, ≤ 5 ms worst case per
16.7 ms frame — inside the lead the controller adapts to (cap 10 ms), at ~6-10 % of one core.
Run-ahead 1 would add ~0.4-0.8 ms. Display-locked pacing also suits iPhone directly: 60 Hz
models present every refresh, ProMotion models every 2nd at 120 Hz.

## 6. Mapping to other platforms

| Platform | Display-locked tick + deadline | Present for a given refresh | Low-latency path |
|---|---|---|---|
| iOS / iPadOS 17+ | `CAMetalDisplayLink` (same code), `preferredFrameRateRange` 60/120 | link drawable | full-screen app layer is direct; avoid UIKit views over the game layer |
| Windows | DXGI flip-model swap chain with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, `SetMaximumFrameLatency(1)`, wait on the waitable object then sleep to the JIT point; `DwmGetCompositionTimingInfo` / `IDXGISwapChain::GetFrameStatistics` for timestamps | `Present(1, 0)` (sync interval 1; 2 at 120 Hz) | independent flip / MPO in borderless full screen; no overlays |
| Linux Wayland | `wp_presentation` feedback (+ `wp_fifo_v1` / `wp_commit_timing_v1` where available), frame callbacks | commit with `wp_commit_timing` target time | direct scan-out of a full-screen opaque surface |
| Linux X11 / Vulkan | `VK_KHR_present_wait` + `VK_KHR_present_id` (or `VK_EXT_present_timing` / `VK_GOOGLE_display_timing`), FIFO present mode | present id per refresh | unredirected full-screen window |
| Android | `AChoreographer_postVsyncCallback` (deadline + expected present time), or Swappy | `eglPresentationTimeANDROID` / `VK_GOOGLE_display_timing` | SurfaceView, not TextureView |

The pure logic (`DisplayCadence`, `InputDeadline`, `AudioRateControl`, resampler) carries over
unchanged; only the callback source and the present call differ.

## 7. Running the measurement

```
scripts/build-macos.sh                       # or any Release build of ReplayNES.app
caffeinate -d scripts/perf-smoke.sh build/ReplayNES.app 60 "<rom>"      # window
FULLSCREEN=1 ACTIVATE=1 INPUT=1 scripts/perf-smoke.sh ...                # full screen + key presses
WARMUP=150 ...                                                           # long take
EXTRA_ARGS="-frameWorkgroup NO" ...   # also -repeatPresents NO, -backlogDrain NO,
                                      # -inputLeadMs <ms>, -fullScreenAutoHide NO, -filmstripThumbnails NO
```
The app must be visible (ACTIVATE=1); an occluded window is not presented at all. Do not take
screen captures during a run (a capture permission prompt over a full-screen app forces
composition).

## 8. Open issues

1. Confirm full-screen direct-to-display in the app (~11 ms expected) with no other window on
   screen; if SwiftUI's hosting layers still prevent it, host the game layer in a dedicated
   borderless full-screen window. Keep the one-drawable backlog from forming on a
   variable-refresh panel (currently drained at most every 5 s).
2. Run-ahead (B) needs an engine API decision.
3. Keyboard input still passes through the main thread (GCKeyboard on the controller queue would
   remove that dependency).
