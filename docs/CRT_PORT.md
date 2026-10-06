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
- Carrier phase: the nesjs tap uses `8 x PPU dot count mod 12`. Nestopia's colour-burst phase `b` advances by +1 in a frame of 89342 dots and
  by +2 in a frame with the odd-frame dot skipped (89341), so `b = -dot count (mod 3)`,
  that is, `8 x dot count mod 12 = 4b` (the difference in origin falls within the scope of nesterm's `phaseOrigin: 'assumed-relative'`).
- The frame number (the key for persistence history and RF noise) is the core's frame number. When the number does not increase, as with a rewind or seek,
  the receiver, supply, and persistence states are reset (the same treatment as nesterm's `reset`).
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
4. **Display layout**: the tube face is always 4:3 (the same geometry as nesterm). "Pixel aspect 8:7" is not used with CRT Display.
   "Hide overscan" crops 8/240 from the top and bottom of the tube face. 1x/FILL fit 4:3 to the same height as the normal display.
5. **Internal resolution cap of 1600x1200**: the same as nesterm's OUTPUT-RESOLUTION-SPEC (256x192 to 1600x1200, the work cap for the detector).
   For larger destinations such as full screen, the display pass enlarges it with bilinear interpolation in linear light
   (in nesterm the browser enlarges the canvas).
6. **Not ported**: nesterm's "TV (legacy model)" (`web/tv-raster.mjs` / `tv-signal.mjs`) and the ASCII display are out of scope.
   Numerical precision is single precision, as in WebGL (the persistence ring is half float, as in nesterm).

## Verification

- `apps/macos/Tests/CRTTests.swift` + `CRTConformance.swift`: compare the Metal output against `tests/fixtures/crt/reference.json`, generated with nesterm's CPU reference model
  (`AGCReceiver`, `receiveRF`+`decodeLine` (with noise), `renderTube`, `evaluateGrowthPlan`+`mixTubeOutput`,
  `rasterRowWeights`, `SupplyState`+`applySupply` (3 frames), `applySpotH`, and persistence via `frameFractions`). Generate with:
  `NESTERM_CRT=<nesterm>/vendor/crt node tools/crt-reference/generate-fixtures.mjs`.
  Maximum errors on an M1 Max: receiver 1e-6, tube face 2e-7, supply 3e-5 (the 25 kV recurrence in single precision), persistence 2.4e-4 (half).
- A frame from a real ROM (Super Mario Bros.) was drawn at 512x384 with nesterm's CPU reference chain (receiver -> supply -> horizontal spot -> tube-face growth)
  and compared with the Metal version in 8-bit: maximum difference 1/255 (mean 0.13).
- `tests/test_render.cpp` "video indices": the code-to-RGB correspondence, an unchanged state hash, agreement between session and export, and
  the mock core returning "unsupported".

## Vulkan port (Linux / Steam Deck)

`apps/linux/shaders/crt/*.comp` (GLSL 450 compute, compiled to SPIR-V by glslc at build time) +
`apps/linux/src/render/` (`crt_model.cpp` = CRTModel.swift, `crt_renderer.cpp` = CRTRenderer.swift,
`crt_display.cpp` = the live-view policy, `crt_export.cpp` = CRTExportRenderer). Each MSL kernel
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

About 2.6 ms of it is the fixed receiver (FFT 1.6 ms); the tube passes are memory-bound
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
At a 60 fps pace, GPU clock throttling makes the same work take 8-12 ms. Before the restructuring (literal-translation kernels), the same
conditions took 6.6 ms, and 16.9 ms at 2400x1800.
