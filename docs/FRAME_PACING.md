# Frame pacing and input latency (live play)

Status: adopted 2026-10-06 (macOS); updated the same evening (section 4b: CRT, main-thread
display-link updates, backlog drain, refresh estimate). Replaces the host-clock tick + present thread +
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

The frame log also has the GPU execution of each new frame's command buffers (`gpuStart`,
`gpuEnd`). perf-smoke runs with the CRT model off unless `CRT=1` (it used to inherit the user's
setting, see 4b).

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

Same probe on an external fixed-refresh display (BenQ EX2710U at 1920x1080, 120 Hz; a second
display at 144 Hz beside it): full screen direct 10.9 ms (deadline -> screen 1 refresh), window
19.25 ms (composited, 2 refreshes on this display). Under heavy load from other apps (the window
server at 50-70 % CPU) even the probe has 5-9 % of its full-screen presents dropped (the window
server skips a flip; the next present shows the picture a refresh late): 200-460 judder/min.

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
  plus a penalty (+0.5 ms, ≤ 3 ms, decaying) for commits closer than 0.5 ms to the deadline and,
  direct to the display, for new frames whose present was dropped or late (latch misses, 4b).
  Typical lead 2.3-2.6 ms; 4.6 ms on a flash-heavy picture (flash filter ~2 ms worst case).
* `AudioRateControl` + `AudioResampler` (RetroArch-style DRC): ratio = nominal/actual frame rate
  × (1 ± ≤ 0.5 %) from the smoothed ring level (target 21 ms), 4-point Hermite interpolation.
  Measured ratio 1.0016 at 60.000 Hz, 0 underruns, level 24-32 ms.
* Frame workgroup (`rn_frame_workgroup.c`, `AudioWorkIntervalCreate` + `os_workgroup_interval`
  start/finish with the frame's deadline): the burst of ~1-2 ms every 16.7 ms otherwise runs at
  a low CPU clock. Bench: mean 4.2 → 2.1 ms, p99 7.9 → 2.8 ms; in the app on a flash-heavy
  picture the lead drops from 10 ms (cap) to 4.6 ms (-4 ms latency) and the emulation thread
  CPU from 16 % to 14 %.
* Display-link quirk: Core Animation sometimes delivers an update on the main thread: AppKit's
  transaction flush dispatches "deferred" display links (`stepTransactionFlush` ->
  `CA::Display::DisplayLink::dispatch_deferred_display_links`), in full screen on every key
  event (measured: 40-110 per minute with injected key presses, 0 without). They used to be
  skipped: that refresh lost its present (judder) and, on a frame refresh, half a frame of
  emulated time. They are now handed to the emulation thread's run loop
  (`CFRunLoopPerformBlock`, processed ~30 µs later, ~6.5 ms before the deadline) and handled like
  any other update; updates older than the last one handled are dropped (no out-of-order
  presents). `foreignCallbacks` counts them.
* Refresh estimate (`DisplayCadence`): the median of the last 15 timestamp deltas, refined by an
  EMA. The former rule (a shorter delta always became the estimate, longer ones were only
  refined within +-25 %) could stick at a wrong rate after one odd delta (seen: 157, 241, 316 Hz
  on a 120 Hz display, i.e. free cadence or the wrong k). The game view also starts a new display
  link when its window moves to another display.
* Backlog drain (`BacklogDrain`): a one-drawable backlog (every present a refresh late after a
  hiccup) is drained by skipping one present that would only repeat the picture. Direct to a
  fixed-refresh display (`NSScreen` min == max refresh interval; `PresentPath`: every one of the
  last 60 updates had presentDelay = 1 refresh, since Core Animation sometimes predicts one
  refresh for a composited window too) a late
  present can only be such a backlog: drained after 4 late presents (at most every 0.25 s).
  Composited, a busy window server also shows presents a refresh late (no skip fixes that), and
  on a variable-refresh panel a skipped refresh shifts the panel's timing: there it stays rare
  (60 late presents, at most every 5 s), as before.
* GPU-heavy pictures (`BuildAhead`, GameRenderer): the CRT model needs 8 ms of GPU per frame at
  1080p on M1 Max (p95 12 ms; its passes now run in their own command buffer and are timed).
  Direct to the display a picture must be finished by about 1.5 ms before its refresh, so a CRT
  picture built after the input sample always missed it, queued behind it and stayed a refresh
  late (sticky backlog; with three drawables the display link was then starved: skipped
  refreshes, emulation at 57-59 fps, audio underruns, 150-900 judder/min). When the p90 of the
  build time exceeds presentDelay - 2.5 ms (presentDelay = targetPresentationTimestamp -
  targetTimestamp, the largest of the last 60 updates; back below presentDelay - 4 ms), the
  picture is built with the frame and shown by the next refresh's present (the frame refresh
  presents the previous picture again, or skips that present to drain a backlog). Every CRT
  picture then appears exactly one refresh later (19.7-19.9 ms instead of a late 19.6-20.1 ms with
  the side effects). Composited (window: presentDelay = 2 refreshes) the CRT picture fits and is
  built and shown in the same refresh as before.
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
* Correction (4b): with the CRT model off the app does go direct in full screen (10.7 ms on an
  external 120 Hz display, the probe's 10.9 ms). The +1 refresh left in later full-screen runs
  came from the CRT model: perf-smoke did not pin it and the user's setting was on, so those
  runs measured CRT pictures that missed their refresh (4b).
* Measurements share the machine with the user's other apps (video playback in a browser during
  several runs); the window figures for judder vary between runs (4-60/min) with that load.

## 4b. Update 2026-10-06 evening (external BenQ EX2710U, 1920x1080 at 120 Hz fixed refresh)

A full-screen run of f6c60f5 with the user's settings measured 19.45 / 21.16 ms, every frame a
refresh after its target, 3459 of 3539 frames missed, judder 164/min, 43 audio underruns,
emulation at 59.03 fps. Findings, each measured:

1. No regression: f6b8c47 and f6c60f5 measure the same (CRT off 10.60 / 10.70 and 10.71 / 10.72
   ms; CRT on 19.64 / 21.15 and 19.62 / 21.16 ms, both with underruns and 135-900 judder/min).
   No pacing code changed between them.
2. Not composited: hiding every layer but the game layer, ordering out the full-screen toolbar
   window, removing the toolbar, dropping the layer's background colour (all via test actions,
   `dumpLayers` logs the tree) changed nothing; the standalone probe in the app's configuration
   (textured quad, background colour, toolbar, audio engine, nested view) stays direct.
3. The CRT model (`crtEnabled` = on in the user's preferences, inherited by perf-smoke): GPU
   7.5-8.4 ms p50, 9.5-12 ms p95 per frame (separate command buffer: 8.2 / 11.8 ms), constant
   with a background GPU load (not a clock issue). With `-crtEnabled NO` the same build is direct:
   10.71 / 10.72 ms, 0 underruns. The CRT picture missed every refresh and the resulting backlog
   starved the display link (see 3 A, BuildAhead): emulation 57-59 fps, beyond the DRC's +-0.5 %:
   that is where the underruns came from.
4. Main-thread display-link updates: on every key event in full screen (3 A); skipped before.
5. Backlog drain and the refresh estimate (3 A): the old estimator stuck at 241-450 Hz in several
   runs of f6c60f5 ("free" cadence or k = 4 on a 120 Hz display).
6. Window-server drops: with other apps keeping the window server busy, 2-8 full-screen presents
   per second are dropped (also by the probe). Direct to the display these are latch misses (the
   window server latches earlier when busy): with a fixed lead of 2.5 / 4 / 6 ms, 225 / 52 / 25
   drops per 25 s. They now count as input-deadline misses (`PresentPath` direct, not built
   ahead, the present before on time; `-latchFeedback NO` disables): under that load the lead
   settles at 5.8 ms (sample -> screen 14.1 ms) with 3-33 drops per 30 s instead of 141-235.
   Composited, drops do not depend on the lead and are not counted.

Before = f6c60f5, after = this update; interleaved runs, 30 s each after 8 s, key presses
injected, Super Mario Bros. All runs were made while other apps loaded the window server (an
illustration app at 50-100 % CPU, the window server at 40-70 %): the standalone probe measured in
the same period drops 3-13 % of its presents (215-460 judder/min), so judder here is mostly the
environment. ms; judder per minute; dropped = new-frame presents never shown.

| Run | sample->screen p50 / p99 | event->screen mean | judder/min | missed refreshes | dropped | audio underruns | emulated fps |
|---|---|---|---|---|---|---|---|
| Before, full screen, CRT off | 10.79 / 19.12 | 24.9 | 586 | 101 | 141 | 2 | 59.87 |
| **After, full screen, CRT off** | 14.10 / 22.43 (lead 5.8 under load) | 22.9 | 328 | 145 | **28** | 9 | 59.59 |
| After, same, second run | 14.12 / 14.12 | 24.8 | 360 | 0 | **3** | 0 | 59.99 |
| Before, full screen, CRT on | 20.12 / 21.64 (every frame a refresh late) | 28.4 | 805 | 1209 | - | 73 | 56.97 |
| **After, full screen, CRT on** | **20.32 / 28.54** (built ahead) | 33.8 | **19** | 22 | 1 | **0** | **60.00** |
| Before, window, CRT off | 19.02 / 27.45 | 36.1 | 25 | 397 | - | 0 | 59.98 |
| **After, window, CRT off** | **18.83 / 18.85** | 34.6 | **0** | 0 | 0 | 0 | 60.00 |
| Before, window, CRT on (3 runs) | 19.2-19.7 / 21.3-29.5 | 34-35 | 62-583 | 0-403 | 16-144 | 0-1 | 59.9-60.1 |
| After, window, CRT on (3 runs) | 19.4-21.0 / 27.7-29.3 | 35-36 | 269-563 | 57-638 | 72-141 | 0 | 59.95-59.98 |

Calm period earlier the same evening (window server < 20 % CPU, before the latch feedback and
the drain change), plain picture: before 10.71 / 10.72 ms full screen (judder 29/min, 0 underruns,
2-6 drops per 30 s), 18.84 / 18.95 ms window (judder 0); with the main-thread update forwarding
10.70 / 10.81 ms full screen (judder 46/min), 18.84 / 18.85 ms window.

Reading: with the CRT off, full screen is direct before and after (10.7 ms when the window server
keeps up; under heavy load the latch feedback trades ~3.3 ms for 5-50x fewer dropped frames). With
the CRT on, the latency stays one refresh above the plain picture (now by design: built ahead) but
judder drops from 800 to ~20/min, underruns from 60-75 to 0 and emulation is back at 60 fps. The
window path is unchanged (CRT: within the noise of the load; plain: the fast drain is not used
there, the corrected estimate removes the stuck states).

## 5. Budget on smaller devices

Per frame on M1 Max (P-core, with the workgroup hint): emulate 0.35-0.45 ms, flash filter
0.1 ms typical / 2 ms worst case (large flashing areas), copy + texture upload + encode 0.25 ms,
GPU for the plain picture negligible (CRT model: several passes, measure per device). An A15-class
iPhone core is roughly 0.8x, an A12-class one ~0.5x: ~1-1.5 ms typical, ≤ 5 ms worst case per
16.7 ms frame — inside the lead the controller adapts to (cap 10 ms), at ~6-10 % of one core.
Run-ahead 1 would add ~0.4-0.8 ms. Display-locked pacing also suits iPhone directly: 60 Hz
models present every refresh, ProMotion models every 2nd at 120 Hz.

## 5b. Display watchdog (2026-10-11)

Emulation and audio never wait for the display, so anything that stops pictures from reaching the
screen freezes the picture while the game plays on (reported on iPhone 17 Pro with the CRT model).
Stuck states found and fixed in the shared Metal path (`CRTRenderer` / `GameRenderer`), and the same
classes in the Vulkan / D3D11 CRT paths:

- **Line count vs the tube in use.** A new CRT line count (Reduced lines) took effect at once while
  its tube plan was still being built in the background: the old tube ran with stage buffers sized
  for the new count and read / wrote past them (Metal shader validation: `tube_h` invalid loads on
  every such frame; a GPU address fault on iOS, after which the process's command buffers can be
  ignored). The stages now follow the tube in use (`key.lines`); the new count starts with its plan.
- **Stale output.** A frame whose CRT passes could not be encoded (no plan yet, stage buffers not
  allocatable under memory pressure) or whose build command buffer failed on the GPU left the tube
  output flagged valid, so every later present showed that old picture. A failed encode / a failed
  build now discards the output and the temporal state (the plain picture is shown until a build
  succeeds).
- **Non-finite temporal state.** NaN / inf in the AGC gain or the supply state (shared buffers fed
  back frame to frame) would stay forever; checked on the CPU before each frame and reset.
- **Silent failures.** No command buffer status was looked at: every display command buffer now
  reports failures (`DisplayHealth`, logged: the first 5, then every 300th, with the Metal error
  code: timeout, pageFault, outOfMemory, notPermitted, ...).

The watchdog (`rnf_display_watchdog`, shared core) covers what is left, including a display link
that stops calling back while the view is visible (the emulation thread then silently falls back to
the host clock, which exists for hidden windows): the frontend reports each published picture and
each picture **confirmed on screen** (presented, and the GPU work that built it completed without
error - so a stale CRT output does not count). While the viewport is visible (macOS: window
occlusion state), a picture waiting more than 250 ms with emulation still advancing triggers
RECOVER: fresh command queue, nothing pending, CRT output and temporal state discarded, plain
picture for one present, and a new display link if its callbacks stopped. If the next 250 ms still
show nothing: RESTART (the CRT renderer rebuilt from scratch, the display link recreated), then a
2 s back-off. Every action is logged with why (`ReplayNES: display watchdog: ...`: wait, link
callback age, last confirmed frame vs latest, GPU errors and the last one, CRT / build-ahead state)
and counted (`gpuErrors`, `watchdogActions` in the stats log). Pause, the menu and seeks never
trigger it. Test hook: `-crtInjectGPUError N` reports every N-th CRT build as failed.

**Desktop (Linux Vulkan, Windows Direct3D 11).** The CRT renderers (`CrtRenderer`,
`CrtRendererD3D11`) have the same fixes: stages and rows follow the tube in use (`key.lines`), a
failed encode clears the output (the plain picture is shown), `discardOutput()` drops the output and
the temporal state, and the AGC / supply state is checked for NaN / inf before each frame (Vulkan:
small host-visible buffers; Direct3D 11: a staging copy of a recent frame read without waiting) -
covered by the shared conformance checks (`crt_conformance.h`, "display recovery"). The presenters
log every failed fence wait / acquire / submit / present / resize with its `VkResult` / `HRESULT`
(`display_health.h`: the first 5, then every 300th) and recover what they can see: a lost Vulkan
surface or device is recreated (the device with every resource, the ImGui backend included), a
failed submit renews its frame slot's fence and semaphore, frame-slot fence and image waits time out
after 1 s instead of blocking the loop; Direct3D 11 recreates the device on
`DXGI_ERROR_DEVICE_REMOVED` / `RESET` / `HUNG` and the swap chain after other present / resize
failures. The frame loop (`app.cpp`) runs the watchdog over each published picture and each
successful present whose CRT build did not fail, reset while the window is minimised, hidden or
occluded: RECOVER = CRT output discarded + plain picture for one present, RESTART = CRT renderer and
swap chain recreated (the device after a loss); CRT builds still failing after two restarts are shown
plain until the CRT is switched off and on. Logged as `ReplayNES: display watchdog: ...` with the
wait, the last present and `Renderer::displayHealth()` (failures, device / swap chain / CRT state).

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
CRT=1 ...                                                                # with the CRT model (default off)
ACTIONS="10:dumpLayers" ...                                              # more --test-actions
WARMUP=150 ...                                                           # long take
EXTRA_ARGS="-frameWorkgroup NO" ...   # also -repeatPresents NO, -backlogDrain NO, -latchFeedback NO,
                                      # -inputLeadMs <ms>, -fullScreenAutoHide NO, -filmstripThumbnails NO
```
The app must be visible (ACTIVATE=1); an occluded window is not presented at all. The window is
moved to the menu-bar screen first (`screen:0`); check the pacing line (e.g. "120 Hz / 2") and
"chromeHidden" (100 % unless the pointer moved) in the summary. Do not take
screen captures during a run (a capture permission prompt over a full-screen app forces
composition).

## 8. Open issues

1. CRT model: built a refresh ahead it is shown at sample + ~19.8 ms (120 Hz, direct). Sampling
   later in that mode (wait until the next refresh minus the measured sample -> build-end time)
   would give ~15 ms at its p99 GPU time; needs the GPU completion fed back into the deadline
   controller. A cheaper CRT (or a smaller tube at 1080p) would allow the same refresh again.
2. Under heavy window-server load the plain picture runs at ~14 ms (lead raised by the latch
   feedback) and still has 0.1-1 drops per second; the penalty decays slowly (0.1 ms per 10 s),
   so after the load ends it takes up to ~5 min to return to 10.7 ms. A faster decay would trade
   that for more drops at equilibrium; needs a calm-machine measurement of the final build.
3. Re-validate the backlog drain and BuildAhead on the built-in ProMotion panel (variable refresh
   keeps the rare drain; not measured since this update). Run-ahead (B) needs an engine API
   decision; keyboard input still passes through the main thread.
