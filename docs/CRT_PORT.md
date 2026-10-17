# CRT Display: Port from nesterm

ReplayNES's "View → CRT Display" is a Metal port of the physical CRT model ("CRT (physical model, experimental)")
bundled with the web version of nesterm (kathoc, MIT). nesterm's `vendor/crt/` is code exported from the author's
research project crt-physical-model via `EXPORT-MANIFEST.json` (commit `919c156329dae4f2a832460fb3726a5f09c20213`, dirty),
and this snapshot is what the port is based on. It is not a new design: each WebGL shader and CPU routine is mapped
one-to-one, and the constants, formulas, evaluation order, and default values are used as they are.

As nesterm itself states, the model is an **uncalibrated experimental model** and does not reproduce any real television
(National, etc.). Ledger IDs (M1-*, M3-*, M4A-*, etc.) are kept in the source comments.

## Pipeline (same order as nesterm's delivery mode)

The `frame` handling in `web/physical-worker.mjs` (source = `pixels`):

```
PPU code (9-bit) ─ RF/IF (FFT overlap-save, 1025-tap complex FIR) ─ receiver (clamp, burst, sync) ─ AGC
  ─ [virtual experiment with 110-220 scanlines] ─ [high-voltage supply/ABL] ─ [horizontal spot] ─ tube face (slots + detector + scatter)
  ─ [phosphor persistence] ─ sRGB display
```

In ReplayNES everything runs as compute passes inside a single Metal command buffer, encoded in the draw callback that
receives a new emulation frame and then `present`ed immediately afterward (no worker round trips, no frame queue, no GPU-to-CPU readback).

## Correspondence table

| nesterm (file / function, shader) | ReplayNES (file / function, kernel) |
|---|---|
| `vendor/crt/adapters/nesjs.mjs` `installCodeTap` (PPU 9-bit code + line-start sample) | `engine` `rn_video_indices` / `rn_renderer_video_indices` (`NestopiaCore::stepFrame` copies `Ppu::GetScreen()` and `GetBurstPhase()` at the same time as drawing), `CRTComposite.rowPhases(burstPhase:)` |
| `reference/composite.mjs` `signalFixture`, `signalVoltage` | `CRTModel.swift` `CRTComposite` (`voltageLUT` = rf-webgl's `voltageTexture`) |
| `reference/rf.mjs` `designIF`, `noiseSigma`, `carrierToNoiseDb`, `hash32`, `noiseKey`, `gaussianAt` | `CRTRF.designIF/noiseSigma/carrierToNoiseDb/hash32/noiseKey`, MSL `lowbias`/`gaussAt` |
| `backends/rf-webgl.mjs` `kernelSpectrum`, twiddles | `CRTRF.kernelSpectrum`, `CRTRF.twiddles` |
| rf-webgl `initCodes` | MSL `rf_init_codes` (`rfInitValue`) |
| rf-webgl `butterfly2` / `butterfly` ×12 stages, `multiply`, `extract` | MSL `rf_butterfly2` / `rf_butterfly` / `rf_multiply` / `rf_extract` (literal-translation version), and `rf_forward_tg` / `rf_inverse_tg` (default), which perform the same operations in threadgroup memory |
| `backends/receiver-webgl.mjs` `reduction` | MSL `rx_reduce` |
| The CPU-side AGC loop in receiver-webgl `process()` (`GatedAGC.advance` for 240 lines after `readPixels`) + `reference/agc.mjs` | MSL `rx_agc` (runs the same line loop, the same gate condition, and the same control law on the GPU in a single thread) |
| receiver-webgl `prepare`, `decode` | MSL `rx_prepare`, `rx_decode` |
| `backends/raster-webgl.mjs` `RasterWebGL` (area integration over the scanline count; composite RGB is sRGB-decoded) | MSL `raster_area` |
| `backends/supply-webgl.mjs` `meanProgram/rowProgram/stateProgram/resampleProgram` + `reference/supply.mjs` constants | MSL `supply_mean/supply_row/supply_state/supply_resample`, `CRTSupply` |
| `backends/spot-h-webgl.mjs` + `reference/spot-h.mjs` `extraSigmaSamples` | MSL `spot_h`, `CRTTube.extraSigmaSamples` |
| `reference/tube.mjs` `createTubePlan` (`detectorMaps`, `growthTables`, ambient light `renderRawTube(powered:false)`, scatter kernel) | `CRTTube.makePlan` (CPU, only when the output size changes, in the background) |
| `backends/tube-webgl.mjs` `packMaps`/`packGrowth`/`colsum` | `CRTTube.Plan` (same layout) |
| tube-webgl `hprog`, `vprog` (fixed / growth), `sprogs[0/1]`, `mixprog`, `litprog`, `persistprog`, `showprog` | MSL `tube_h`, `tube_v` / `tube_v_growth`, `tube_scatter` (literal translation) and `tube_scatter_x16/_y16` (default), `tube_mix`, `tube_lit`, `tube_persist`, `show_fragment` / `show_kernel` |
| tube-webgl `setPersistence`, `persistenceWeights`, slots (M4B-GAP) | slot management in `CRTRenderer.encode`, `persistenceWeights()` |
| `reference/phosphor.mjs` `frameFractions` | `CRTPhosphor.frameFractions` |
| `web/physical-worker.mjs` `configure`/`reset`/`frame` (stage order, default effect values) | `CRTRenderer.configure` / `reset` / `encode` |
| `web/physical-preview.mjs` `displaySize` (fits CSS×DPR to 4:3, 256-1600) | `CRTRenderer.tubeSize(forDestination:)`, `GameRenderer.crtViewport` |
| `web/index.html` / `web/app.mjs` CRT settings (growth, persistence, supply, signal 20-90 dBµV, scanlines) | `App/CRTSettings.swift` (Settings → Display & Audio → CRT Display, stored via `@AppStorage`) |

Default values (same as nesterm's shipped ones): scanlines 240, thicker scanlines where brighter = on, phosphor persistence = on,
wider and dimmer on bright screens = on, signal strength 65 dBµV, ambient light 40 lx (no control in nesterm's UI),
sample count 4, noise seed 1. nesterm has no presets, so ReplayNES likewise offers only "Reset to nesterm defaults".
CRT Display itself is off by default (the conventional crisp display).

## Input and determinism

- nesterm uses the PPU bitmap of @nesjs/core (6-bit color + 3-bit emphasis) and line-start sample positions obtained by counting PPU clocks.
  ReplayNES exposes Nestopia's `Video::Screen` (the same 9-bit format: `(color & greyscale) | emphasis<<6`) through
  `rn_video_indices` (an additional display-only API; it is not part of the state or hash, and is verified in `tests/test_render.cpp`).
- Carrier phase: the nesjs tap uses `8 x PPU dot count mod 12`. ReplayNES derives the colour-burst phase `b` from the CPU's
  monotonic master-clock counter at the frame boundary (`NestopiaCore::burstPhaseAtFrameEnd`: `b = 2 x dots mod 3`, dots = master clocks / 4).
  A frame advances it by +1 (89342 dots) or +2 (89341, odd-frame dot skip), i.e. `8 x dot count mod 12 = 4b`
  (the difference in origin falls within the scope of nesterm's `phaseOrigin: 'assumed-relative'`). In uninterrupted play this is exactly
  Nestopia's `Ppu::GetBurstPhase()`, but that counter is not saved in states (`Ppu::LoadState` resets it to 0), whereas the master-clock
  counter is (CPU `CLK` chunk): the phase of a frame is the same whether it was reached by play, seek, rewind, take switch or a loaded state.
  Display only: emulation, states, hashes and the core compatibility ID are unchanged.
- The frame number (the key for persistence history and RF noise) is the core's frame number. When the number does not increase, as with a rewind or seek
  (or a re-encoded still), the receiver, supply, and persistence states restart (nesterm's `reset`), and so does the first frame of a renderer.
  ReplayNES restarts them from **the current picture held** rather than from an idle tube with empty history (intentional difference 6).
- If flash reduction has modified the frame, then for safety the reduced RGB image is sent to the tube face as nesterm's "composite RGB" input
  (`COMPOSE-ASCII-RGB`: 512x240, sRGB-decoded, no RF). Normal frames take the PPU code -> RF path.
- MP4 export and Syphon use the same `CRTRenderer`. The same frame sequence yields the same image
  (`CRTTests.testSameFrameSequenceIsBitIdentical`).

## Intentional differences (an honest list)

1. **AGC runs on the GPU**: nesterm reads the receiver statistics back to the CPU with `readPixels`, runs `GatedAGC` (double precision), and
   re-uploads the gain. Here the same loop runs in a single-thread Metal kernel (single precision),
   eliminating the readback. The difference from the reference is about 1e-6. A line with signal loss (amplitude < 1e-12) does not cause the frame to be dropped
   (nesterm does not display such a frame; it cannot actually happen because the burst is always synthesized).
2. **Packet validation and the gap limit are omitted**: nesterm throws on sample gaps longer than 1 second and on duplicate packets, but
   fast-forward and rewind are routine in ReplayNES, so gaps are ignored (the AGC does not change while the gate is disabled, so the result is the same),
   and a number that goes backward triggers a reset.
3. **Restructuring for speed (numerically identical)**: the 12 FFT stages are computed in threadgroup memory, and the scatter (about 90 taps)
   in a register window. Operations, order, and weights are the same, and the results match the literal-translation version bit for bit
   (`testOptimizedKernelsAreBitIdenticalToDirectPort`).
4. **Display layout and picture geometry**: the tube face is always 4:3 (the same face as nesterm's). nesterm maps
   the 256 active dots across the whole face width (a dot 1.25 times as wide as a line is high); ReplayNES maps them
   to the 32/35 of the width the NTSC raster gives them, so a dot is 8:7 like the plain display's "8:7 Pixels" and
   the black bands beside the picture are the line's border and blanking (section "Picture geometry"). The
   "8:7 Pixels" setting itself is not used with CRT Display (the tube has that shape already).
   "Hide overscan" crops 8/240 from the top and bottom of the tube face. 1x/FILL fit 4:3 to the same height as the normal display.
5. **Internal resolution cap of 1600x1200**: the same as nesterm's OUTPUT-RESOLUTION-SPEC (256x192 to 1600x1200, the work cap for the detector).
   For larger destinations such as full screen, the display pass enlarges it with bilinear interpolation in linear light
   (in nesterm the browser enlarges the canvas).
6. **History starts from the picture held**: after a discontinuity (seek, rewind, take switch, load, the first frame), nesterm starts the
   persistence history empty and the supply idle. A still shown that way (ReplayNES pauses on such frames) got only the first frame's share of
   each phosphor's light — green and blue emit about 10% of a frame's energy in later frames, red almost none — so it was tinted purple, and
   its ABL / anode load differed from continuous play. Here the oldest persistence slot also stands for the ordinals before it (M4B-GAP
   extended to the start of history) and the supply starts from the steady state of the first picture (`supply_state` prime mode). The AGC
   still starts from its initial gain (it settles within the frame; < 1e-3). The conformance harness reproduces nesterm's start by showing a
   black frame first (black held = empty history and an idle supply), and checks that a still after a seek equals the same picture shown
   in continuous play (`stillAfterSeek`).
7. **Not ported**: nesterm's "TV (legacy model)" (`web/tv-raster.mjs` / `tv-signal.mjs`) and the ASCII display are out of scope.
   Numerical precision is single precision, as in WebGL (the persistence ring is half float, as in nesterm).
8. **No signal and power-on** (a ReplayNES addition, next section): receiver conditions nesterm does
   not model (no carrier, an unlocked sync). Off (the default "normal" reception), every kernel runs
   exactly as before: the conformance checks above are unchanged and `receptionNeutralIsIdentity`
   checks it bit for bit.

## Picture geometry (ReplayNES change)

The NES picture has one shape on every display: an object (a 16x16 sprite) is as wide relative to
its height on the CRT as with "8:7 Pixels", in the MP4 export and in the live view, on Metal,
Vulkan and Direct3D 11.

Derivation (NTSC):

- Master clock 6 x 315/88 MHz = 21.477272 MHz; the PPU outputs a dot every 4 clocks: 5.369318 MHz.
  The 256 active dots take 47.68 us of the line.
- A 4:3 NTSC picture has square pixels at 12 3/11 MHz (640 per active line, 480 lines per frame,
  240 per field).
- So a dot is 12.2727 / 5.3693 = 16/7 square pixels wide and a field line 2 high: a pixel aspect
  ratio of **8:7** (`RNF_NES_PIXEL_ASPECT`, the plain display's and the export's 8:7).
- On a 4:3 face whose height the 240 lines fill (a line = H/240), a dot is 8/7 x H/240 wide, and the
  256 dots span 256 x 8/7 x H/240 = 32/35 x 4H/3: **32/35 (91.4 %) of the face width**, centred
  (`RNF_CRT_ACTIVE_WIDTH`, `rnf_crt_active_width()`; `rnf_crt_pixel_aspect(a)` = (4/3 a / 256) / (1/240)
  = 8/7 at 32/35, 1.25 at 1). On the model's 280 x 210 mm face: a dot is 1.000 mm, a line 0.875 mm,
  the picture 256 mm wide with a 12 mm band (12 dots, 24 drive samples) on each side.
- The bands are the rest of the line (the NES's 26 border dots and the blanking; about 280 dot widths
  fit a 4:3 line): the model's composite signal is at blanking level there (`rfInitAt`: 0.312 V
  outside samples 36..2084), so the beam draws black, and the face shows only its ambient light.

nesterm's mapping put the 512 drive samples across the whole 280 mm (1.25:1, 9.4 % too wide): the
cause of the CRT picture looking wider than "8:7 Pixels". The change is in the shared tube plan, so
every backend and the export follow it:

- `CrtSettings::activeWidth` / `CRTRenderer.Settings.activeWidth` (default `RNF_CRT_ACTIVE_WIDTH`; 1 =
  nesterm's mapping, which the conformance comparisons with the fixtures keep using) goes to the tube
  plan (`makePlan(activeWidth:)`, `Plan.activeWidth`, part of the tube key).
- Horizontal detector maps: the drive position of face coordinate x is (x - 12 mm) / 256 mm x 512 - 0.5
  (nesterm: x / 280 mm x 512 - 0.5); the beam's taps beyond the drive weigh nothing (the beam Gaussian
  is normalised over its whole support: black beyond the ends). Vertical maps, slot mask, detector,
  scatter, ambient light: unchanged (all on the 4:3 face).
- Quantities in drive samples use the drive's sample pitch (0.5 mm instead of 0.547 mm): the
  horizontal spot growth (`extraSigmaSamples(activeWidth:)`, M4C-SPOT-H) and the fast path's
  drive-domain scatter (`DriveScatter`; kappa = mean emission per unit drive over the columns the
  drive covers).
- Fast path: the scatter source is 512 + 2 x 24 samples wide (`DriveScatter.margin`: the face beside
  the drive), so the 2 mm glow of a bright edge reaches into the band as in the reference path's
  full-resolution scatter; `tube_v_fast` samples it at (x + 0.5) x scale + offset (`FastTubeParams.scat`;
  the fast tube parameters are 80 bytes).
- Not modelled: the supply's widening of bright pictures (M4C) still resamples within the drive, so
  at full white about one dot per side is pushed out instead of into the band (as at the face edge
  in nesterm).

Checks: `tests/test_frontend_crt.cpp` "crt geometry" (the clock derivation, the constants, the export's
8:7 within 0.5 % on every canvas); `pictureGeometry` in `crt_conformance` (Vulkan, Direct3D 11) and
`CRTTests.testPictureGeometryIs87` (Metal): three 16x16 white squares 128 dots / 128 lines apart through
the RF path with default settings, the centroids of their light (Metal: the same frames on black subtracted; shared harness: no ambient light)
on the face, both kernel sets, 640x480 (Metal also 1144x858), pixel aspect 8:7 within 0.5 % and centred within
0.5 %; `replaynes-export --self-test` measures the same squares in the CRT export's canvas (1280x960,
1920x1080). Measured (M1 Max):

| Path | Before (nesterm mapping) | After |
|---|---|---|
| Metal, reference and fast, 640x480 / 1144x858 | 1.2501 (+9.4 %) | 1.1433 (+0.03 %) |
| Vulkan (MoltenVK), reference and fast, 640x480 / 1144x858 | 1.2504 (+9.4 %) | 1.1433 (+0.04 %) |
| CRT MP4 export (Vulkan processor), 1280x960 / 1920x1080 canvas | 1.2505 (+9.4 %) | 1.1433 (+0.04 %) |
| Direct3D 11 (WARP, CI x64 runner), reference and fast, 640x480 / 1144x858 | - | 1.1433 (+0.04 %) |
| Plain 8:7 export, 1280x960 crop 8 (1170x896 for 256x224) | 1.1426 (-0.02 %) | unchanged |

## No signal and power-on (ReplayNES addition)

With CRT Display on, a picture without a signal is snow and a power-on / reset goes through it:

- **No signal** (frame 0: the machine has not been switched on yet; e.g. a project opened or sought
  to its start): the RF input is the receiver's thermal noise only (carrier level 0, the M3-NOISE
  term unchanged), the gated AGC runs up to its maximum gain, the colour killer is on. The snow
  moves (a new noise field per NES frame, also while paused).
- **Power-on / reset** (the step from frame 0, or a step that applies a soft reset / power cycle,
  recorded or live, also in playback): `RNF_CRT_POWER_ON_FRAMES` = 60 frames. 24 frames of snow, then
  the carrier comes up over ~10 frames (60 dB, logarithmic) and the AGC follows it (the picture
  emerges from the snow), the clamp keys on the back porch again, the colour killer releases, and
  the deflection locks: a vertical roll of 1.5 screens slowing into place (the vertical blanking
  passes as a black bar) and a bending horizontal tearing with line jitter, both decaying. Audio:
  white noise ("hiss", low-passed, about -24 dBFS RMS) replaces the game sound and fades out as
  the picture locks.
- **Flash reduction** (any level): static low-contrast grain and a monotonic fade instead
  ([FLASH_REDUCTION.md](FLASH_REDUCTION.md#crt-snow-and-power-on)).
- **MP4 export** with the CRT effect starts with the same power-on over the first 60 frames of the
  range (snow, then the picture locks, with its hiss), whatever the range's first frame is.
  Deterministic: fixed noise seeds, picture counter from the export's first frame. Resets inside
  the range are not shown in exports (the export renderer has no per-frame events).

Display / audio output only: the sequence is derived from what is shown (frame numbers and the
events a step applied, `rnf_crt_power`), never written anywhere; emulation, the input log,
`.nesrec`, states and hashes are unchanged (`testCRTExport`: same renderer hash with and without).
The plain picture (CRT off, or the display watchdog's FALLBACK) shows no snow.

Model (shared logic in the frontend core: `rnf_crt_reception_at` / `rnf_crt_reception_receiver`,
`frontend/src/crt_reception.cpp`; the same receiver settings on Metal, Vulkan and Direct3D 11):

| Stage | Normal | No signal / power-on |
|---|---|---|
| RF (`rf_*`, `RFParams.signalLevel`) | carrier 1, noise key = frame ordinal | carrier 0 .. 1; noise key = picture counter (flash reduction: constant) |
| AGC (`rx_agc*`, `AGCParams.min/maxGain`) | GatedAGC 1/16 .. 16 | maximum = the gain that brings the receiver noise (sigma 10^(-CNR/20) of the carrier) to a luma sigma of 0.32; flash reduction: min = max (pinned) |
| decode (`rx_decode*`, `RxParams`) | chroma 1, lift 0 (branch skipped) | colour killer gain; luma lift of a detector without a keyed clamp (snow on grey) |
| deflection (`rx_deflect`, new) | not dispatched | screen row y shows signal line y + roll (mod 262 lines, blanking black); lines displaced by tear sin(2 pi (y c / rows + phase)) + jitter hash; RGB input: picture gain (the fade) |

The deflection pass runs on the drive after the receiver (and the line-count raster), before the
supply; the temporal state continues through the sequence (its frames take the next ordinal), so
the phosphors decay into the snow and out of it. Checks: `tests/test_frontend_crt.cpp` (sequence,
receiver settings, tracker, hiss), `apps/macos/Tests/CRTReceptionTests.swift` and the
`crt_conformance` harness (`receptionNeutralIsIdentity`, `noSignalSnow`,
`noSignalSnowReducedIsStatic`, `deflectRollAndGain`, `powerOnSettles`).

## Verification

- `apps/macos/Tests/CRTTests.swift` + `CRTConformance.swift`: compare the Metal output against `tests/fixtures/crt/reference.json`, generated with nesterm's CPU reference model
  (`AGCReceiver`, `receiveRF`+`decodeLine` (with noise), `renderTube`, `evaluateGrowthPlan`+`mixTubeOutput`,
  `rasterRowWeights`, `SupplyState`+`applySupply` (3 frames), `applySpotH`, and persistence via `frameFractions`). Generate with:
  `NESTERM_CRT=<nesterm>/vendor/crt node tools/crt-reference/generate-fixtures.mjs`.
  Maximum errors on an M1 Max: receiver 1e-6, tube face 2e-7, supply 3e-5 (the 25 kV recurrence in single precision), persistence 2.4e-4 (half).
- A frame from a real ROM (Super Mario Bros.) was drawn at 512x384 with nesterm's CPU reference chain (receiver -> supply -> horizontal spot -> tube-face growth)
  and compared with the Metal version in 8-bit: maximum difference 1/255 (mean 0.13).
- `tests/test_render.cpp` "video indices": the code-to-RGB correspondence, an unchanged state hash, agreement between session and export, and
  the mock core returning "unsupported"; the burst phase of a frame is identical by straight play, seek, rewind and a raw state load
  (both frame parities, around checkpoints) and advances by 1 or 2 every frame.

## Fast path (live display)

The live view (macOS/iOS `MetalView`, Syphon, the Vulkan and Direct3D 11 live views) renders with
`CRTRenderer.Quality.fast` / `CrtQuality::fast`; the conformance tests and the MP4 export keep
`.reference`, everything described above (the 1:1 port and its bit-identical restructurings). The fast
path is the same model, stage order and temporal state (AGC, supply, persistence slots, M4B-GAP, the
history start of intentional difference 6) with the work restructured; where it approximates, the
error is far below 8-bit display precision. Kernels: `CRTShaders.fast` (MSL, fast math),
`apps/linux/shaders/crt/*_fast.comp`, `scatter_drive.comp`, `show_h.frag`, `show_kernel_h.comp`
(GLSL; HLSL generated from them). Setup tables: `CRTTube.fastPlan` / `driveScatter`
(`crt::tube::fastPlan` / `driveScatter`).

| Stage | Reference | Fast path | Effect on the result |
|---|---|---|---|
| RF/IF | complex FIR h (1025 taps), 214 complex FFT-4096 blocks, forward and inverse in two passes | the receiver only reads Re(IF output) = signal * Re(h): two real blocks per complex FFT (block 2m real, 2m+1 imaginary), Stockham radix-8 in registers + 16 KB exchange, forward, spectrum multiply and inverse in one threadgroup; real carrier | float rounding (~1e-6) |
| receiver | serial per-row reductions, one-thread AGC with logs/exps per row | parallel row statistics; AGC target clamp(-log sync) per row in parallel (log(gain*sync) = log gain + log sync), serial loop of multiply-adds; prepare + decode + row mean per row in threadgroup memory | rounding (~1e-6) |
| supply | one thread per row loops over the rows above (O(rows^2)) | the affine anode recurrence as a parallel scan (v_j = a^(j+1) v0 + P_j), prime mode from P_last | float32 rounding of the 25 kV recurrence (~1e-5 relative) |
| horizontal spot | 7 exps per tap normalisation, per output tap | weights per source sample: e_k = e1^(k^2) | rounding |
| persistence | 7 half-float slots of the tube output (56 B/pixel read) | slots of the tube **drive** (512 x lines, half4); the tube renders the persistence-weighted drive | exact for the linear stages; the beam-current spot growth of the afterglow (< 12 % of green/blue, < 1e-6 of red) is computed at its weighted rather than its original current: a faint ghost of a moving bright object is a little thinner |
| scatter | 2 mm Gaussian (σ 8 px at 1144 wide, 67-93 taps per axis) on the full-resolution emission | the same Gaussian on the drive: blurred by sqrt(σ_scatter² + σ_spot² + σ_detector² - bilinear) per axis, times kappa = mean emission per unit drive (flat field of the plan, 0.749) x 0.12, bilinear at each pixel | the scatter is 8x wider than spot, detector and slot pitch, so the mask / scanline structure it sees is gone either way; edges of bright areas differ by < 1 % of the 12 % scatter share |
| vertical spot growth | 33 growth-level tables, linear interpolation per tap | a cubic in the beam-current fraction per tap (least squares over the 33 levels) | max 5e-4 of the row's largest weight |
| tube passes | horizontal (float4), vertical (emission), 2 scatter, lit, persist: 5 full-resolution float4 buffers | horizontal pass staged in threadgroup memory, taps in registers; one full-resolution pass (emission + scatter + ambient computed in place) writing half4 | half-float output (5e-4 relative) |

Measured against the reference (`scripts/bench-crt-macos.sh --compare reference:fast`, M1 Max; PSNR of
the sRGB-encoded tube picture before 8-bit quantization, every 10th frame of 300):

| Sequence | 640x480 | 1144x858 | 1600x1200 |
|---|---|---|---|
| Super Mario Bros. scrolling (mean / worst frame PSNR) | 69.7 / 69.3 dB | 68.2 / 66.9 dB | 66.4 / 64.7 dB |
| Gradius (sprites on black) | 79.6 / 66.9 dB | 73.8 / 60.6 dB | 71.2 / 58.4 dB |
| mean ΔE76 (SMB / Gradius) | 0.03 / 0.01 | 0.04 / 0.01 | 0.04 / 0.02 |

The largest differences (up to 11/255 at a few pixels of the worst frame) are at the edges of moving
sprites (the afterglow's spot shape) and of the bright HUD bar (scatter edge); the ×10 difference
images show nothing else. The conformance harnesses bound it on every backend
(`testFastPath*`, `crt_conformance`: PSNR > 45 dB with defaults at 320x240 and 640x480, effects off,
160 lines and RGB input, a still after a seek equal to continuous play, determinism): 65.9-73.5 dB on
Metal, MoltenVK, WARP and the Parallels adapter alike.

GPU time, M1 Max, Super Mario Bros. frame sequence, all effects, one command buffer per frame
(`scripts/bench-crt-macos.sh`; back to back = each frame waited for, high clocks; paced = one frame
per 1/60 s like the live view, where the GPU's clock management lowers the clock for light loads):

| Tube | reference back to back | fast back to back | reference paced 60 Hz | fast paced 60 Hz |
|---|---|---|---|---|
| 640x480 | 1.79 ms | 0.39 ms (4.6x) | 4.78 ms | 1.29 ms (3.7x) |
| 1144x858 | 2.71 ms | 0.46 ms (5.8x) | 5.89 ms | 1.48 ms (4.0x) |
| 1600x1200 | 4.18 ms | 0.58 ms (7.3x) | 7.13 ms | 1.84 ms (3.9x) |

Per pass at 1144x858 (back to back, median, each pass in its own encoder): reference: FFT 0.38,
receiver 0.12, supply 0.19, horizontal spot 0.23, tube_h 0.42, tube_v_growth 0.43, scatter 0.59,
lit + persist 0.33, show 0.05 ms. Fast: rf_fast 0.08, receiver (stats, AGC, decode) 0.05,
supply_fast 0.01, drive_post_fast (resample + spot + persistence + scatter x) 0.06, scatter y 0.02,
tube_h_fast 0.10, tube_v_fast 0.11, show 0.03 ms.

The fast kernels need 32 KB of threadgroup memory and 512-thread groups (all Metal GPUs ReplayNES runs
on, RADV, lavapipe, Direct3D feature level 11_0); tubes whose horizontal maps have more than 24 taps
or a 64-column tile reaching more than 160 drive samples (narrower than ~250 pixels) render with the
reference kernels.

Adaptive resolution on macOS/iOS (`CRTAdaptiveScale` in `MetalView`, the policy of the desktop
`CrtDisplayPolicy`): when the p90 of the last 60 picture builds' GPU time exceeds 80 % of the NES frame
period the tube is rendered at 0.85x (at least 512 wide) and enlarged by the show pass; it steps back
up below 55 %, at most one step per 60 builds.

Benchmarks: `replaynes-cli dump-ppu <rom> --out seq.ppu --frames N --skip S --press start@40,right@180+900,...`
writes a frame sequence of raw PPU output (no ROM data) for `scripts/bench-crt-macos.sh --ppu seq.ppu
[--sizes ...] [--pace 60] [--profile] [--mode reference|fast|direct] [--compare reference:fast --png DIR
--png-frames ...]` and `replaynes-crt-test --bench WxH --ppu seq.ppu [--quality ...] [--pace 60]`
(`REPLAYNES_CRT_PROFILE=1` for per-pass times; Direct3D 11: `test_crt_d3d11 --bench WxH --quality ...`).

## Vulkan port (Linux / Steam Deck)

`apps/linux/shaders/crt/*.comp` (GLSL 450 compute, compiled to SPIR-V by glslc at build time) +
`apps/linux/src/render/` (`crt_renderer.cpp` = CRTRenderer.swift, `crt_display.cpp` = the live
view, `crt_export.cpp` = CRTExportRenderer) + the graphics-API-free parts shared with Direct3D 11 in
`apps/desktop/src/render/` (`crt_model.cpp` = CRTModel.swift, `crt_display_policy.cpp` = the
live-view policy, `crt_conformance.h` = the conformance checks). Each MSL kernel
has a GLSL twin with the same name, arithmetic and evaluation order; images are std430 `vec4`
buffers, push descriptors (`VK_KHR_push_descriptor`) bind them, parameters are push constants. The
AGC loop runs in the one-thread `rx_agc` kernel, receiver/supply/persistence state lives in GPU
buffers and is reset with `vkCmdUpdateBuffer` in command order (a non-increasing ordinal = a
discontinuity). Display-only: the input is `rn_video_indices` (RF path) or the flash-filtered RGB
picture when the filter altered it.

Differences from the Metal port, all value-preserving:

1. Arithmetic the bit-identity checks rely on is `precise` (no FMA contraction, like Metal's safe
   math mode). `gaussAt`'s `log`/`cos` are float32 cephes implementations (~1 ULP): Vulkan only
   guarantees ~2^-11 absolute error for `cos`, which would show in the RF noise.
2. The persistence ring is zeroed when allocated (WebGL textures start at 0; a Vulkan buffer does
   not, and 0 x garbage can be NaN).
3. Restructured kernels (numerically identical to the direct port, checked bit for bit at 256x192
   and 1144x858): shared-memory FFT with 512 threads and a coalesced inverse load (the bit-reversal
   as pair swaps in shared memory), the x scatter through a shared-memory row tile, `tube_h` with
   the column's taps in registers over 8 rows, `tube_lit` + `tube_persist` fused (the new slot is
   read as the half value it stores). The y scatter keeps the register-window kernel.
4. Live view: GPU time per new picture (timestamp queries) drives build-ahead (`rnf_build_ahead`,
   budget = what the maximum input lead leaves after the CPU work) and an adaptive tube scale
   (x0.85 steps when p90 > 80 % of a frame period, >= 512 wide, back above below 55 %); otherwise
   the tube maps 1:1 onto the destination (cap 1600x1200).

Conformance (`replaynes-crt-test`, the same fixture, inputs and tolerances as CRTConformance /
CRTTests; ctest `crt_conformance`): passes on RADV (Steam Deck), lavapipe (llvmpipe, Mesa 26.2 in
the Flatpak runtime; `REPLAYNES_VK_DEVICE=llvmpipe`) and MoltenVK (development). Maximum errors on
RADV: receiver 1.0e-6, with noise 1.4e-6, tube 1.8e-7 / growth 2.4e-7, raster 1.2e-7, supply 3.6e-5,
spot 1.8e-7, persistence 4.9e-4 (half); determinism and fast == direct port bit-identical.

GPU time on the Steam Deck (RADV VANGOGH, 1600 MHz, default effects, RF input):

| Tube | `--bench` (sync, per frame) | in the app, Gaming Mode 90 Hz (p50 / p90) |
|---|---|---|
| 1600x1200 | 18.4 ms (before restructuring) | - (larger than the screen) |
| 1144x858 (full screen 1280x800, fill) | 8.9 ms (11.0 before) | 11.7 / 11.8 ms |
| 960x720 (integer scale) | - | 9.3 / 9.5 ms |
| 640x480 | 6.3 ms (before) | - |

## Direct3D 11 port (Windows)

The Vulkan GLSL is also the source of the Windows kernels: `scripts/generate-crt-hlsl.sh` translates
it (glslc -> SPIR-V -> SPIRV-Cross, HLSL shader model 5.0) into the committed
`apps/windows/shaders/crt/*.hlsl`, compiled at run time by `D3DCompile` (IEEE strict; `precise`
kept). `apps/windows/src/crt_d3d11.cpp` is `crt_renderer.cpp` on raw buffers and the immediate
context (same passes, parameters, dispatch sizes, state resets in command order); the live view uses
the shared policy. One GLSL edit for FXC (no `continue` in `tube_v_growth`'s per-channel loop, same
arithmetic). Conformance (`test_crt_d3d11`, the shared checks): passes on WARP (bit-identical fast ==
direct) and on the Parallels adapter in the development VM (which fuses `precise` multiply-adds, so
fast == direct is checked within 2e-3 there; the test detects that). Details and numbers:
[WINDOWS.md](WINDOWS.md#crt-display-direct3d-11-compute-appswindowssrccrt_d3d11).

(Reference kernels; the fast path's Deck numbers have not been measured yet: the device was not
reachable when it was added. MoltenVK on the M1 Max, `--bench 1144x858 --ppu`: reference 23.9 ms,
fast 4.2 ms mean.) About 2.6 ms of it is the fixed receiver (FFT 1.6 ms); the tube passes are memory-bound
(persistence reads 7 half-float slots per pixel). 60 fps holds at full screen without lowering the
resolution; the adaptive step only engages if the GPU p90 passes 13.3 ms.

## Latency and GPU time (M1 Max, 32-core GPU, 120 Hz built-in display)

Existing in-app measurement (emulation done -> display, the `--snapshot` JSON, SMB auto-played for 12 seconds):

| Condition | Emulation done -> display | Display fps | Display GPU avg / max |
|---|---|---|---|
| Normal display, windowed (1x) | 34.2 ms | 60.0 | 0.15 / 0.17 ms |
| CRT, windowed (1x, tube face 1600x1200) | 37.2 ms | 61.0 | 7.9 / 11.1 ms |
| Normal display, full-screen FILL | 31.3 ms | 60.0 | 0.17 / 0.32 ms |
| CRT, full-screen FILL (tube face 1600x1200 -> enlarged) | 30.9 ms | 60.5 | 7.3 / 11.3 ms |

GPU alone (continuous run, maximum clocks): 4.0 ms/frame with the tube face at 1600x1200 and all effects on (receiver 0.75 ms).
At a 60 fps pace, GPU clock throttling makes the same work take 8-12 ms. These are the reference kernels; the live view now renders the
fast path (section "Fast path": 1.5-1.8 ms paced at 1144x858-1600x1200 in the headless benchmark). Before the restructuring (literal-translation kernels), the same
conditions took 6.6 ms, and 16.9 ms at 2400x1800.
