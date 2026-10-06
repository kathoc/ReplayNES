# ReplayNES — Architecture Decision Record

Status: accepted (2026-10-05). Covers the scope of `docs/spec/00_RESEARCH.md` and records what the
engine implementation (step 2) actually found while integrating the core.

## 1. Requirements that drive the core choice

| Requirement | Why |
|---|---|
| Embeddable in a macOS arm64 app, portable C/C++ | Engine is shared with future Windows/Linux frontends |
| Exactly 1 frame per call, caller-driven | Logical time = frame index; host clock only paces |
| Input injected per frame (P1/P2 bitfields) | The recorded final bitfield is the source of truth |
| Complete savestates, bit-exact round trip | Rewind/seek/branch = checkpoint + fast replay |
| Determinism (no host time/RNG/thread influence) | Takes must replay identically, years later |
| Soft reset and power cycle as events | Some games are played with resets |
| Raw video (RGB) + raw PCM per frame | Metal display, AVAudioEngine, offline MP4 export |
| Several independent instances per process | Live play + export + determinism checks at once |
| License compatible with a distributable app | GPL-2.0-or-later app is acceptable |
| Pinnable core build, old projects replayable | Projects are refused, never silently "converted" |

## 2. Candidates

| | Mesen 2 | Nestopia UE | FCEUX | QuickNES | libretro cores (nestopia/fceumm/mesen via libretro API) |
|---|---|---|---|---|---|
| Accuracy | Excellent | Very good (cycle-based CPU/PPU/APU) | Good (TAS-proven, some inaccuracies) | Moderate (fast, fewer mappers) | as the wrapped core |
| Embedding | Large multi-system framework; single `Emulator` with its own threads; interop layer assumes one global instance | Library-style `Nes::Api::Emulator` object, no internal threads | Pervasive global state, single instance | Class-based `Nes_Emu`, small | `retro_*` C API is process-global: one instance per loaded dylib |
| 1-frame step / input | Possible but goes through its run loop & notification system | `Emulator::Execute(video, sound, input)` = exactly one frame, input struct passed in | `FCEUI_Emulate` one frame, input via globals | `emulate_frame(joypad1, joypad2)` | `retro_run`, input via callbacks |
| Savestate | Complete | Complete (chunked, versioned) + found gaps, see §5 | Complete | Complete | `retro_serialize` |
| Reset / power | Yes | `Machine::Reset(false/true)` | Yes | Yes | `retro_reset` (soft only, power via reload) |
| Raw A/V | Yes | 32-bit RGB with masks, 16-bit PCM at any rate | Yes | Yes | Yes (rate/format negotiated) |
| Multi-instance | No (global emulator, threads) | Yes (verified, see §5) | No | Yes | Only by loading duplicate dylibs |
| License | GPL-3.0 | GPL-2.0-or-later | GPL-2.0-or-later | LGPL-2.1+ | per core |
| macOS arm64 build | Yes (with .NET UI; core builds) | Yes (plain C++03/11 code) | Yes | Yes | Yes |
| Bridge to Swift | C++ → C shim | C++ → C shim (our `replaynes.h`) | C++ → C shim | C++ → C shim | C (direct) |
| Build pinning | Submodule | Submodule (pinned commit + verified build-time patches) | Submodule | Submodule | Separate dylib per core version |

Other options considered and rejected: ares/higan (ISC, accurate, but a monolithic multi-system
framework with global scheduler state), puNES (GPL-2, accurate, UI-coupled code base).

## 3. Decision

**Nestopia UE core** (`third_party/nestopia`, pinned commit, built from source with `NST_NO_ZLIB`),
used through its instance-based C++ API, wrapped by `NestopiaCore` behind our `ICore` interface.
Not the libretro build (process-global API). Reasons: accuracy is high enough for the target use,
the API is natively one-frame/caller-driven with explicit input structs, instances are independent,
and GPL-2.0-or-later is compatible with the app license. Mesen 2 would be more accurate but is
GPL-3.0, multi-threaded internally and designed around one global emulator instance.

## 4. Module boundaries (engine/, portable C++17, no platform UI APIs)

```
             Swift app / future SDL-Qt-Win32 frontends
                          | replaynes.h (C API, opaque handles, error codes)
  capi/ -----------------------------------------------------------------
  session/   Session: owns core + timeline + checkpoints + bookmarks, RECORD/REPLAY, seek, takes, undo
  timeline/  InputRecord {p1,p2,events}; append-only Segment DAG; active path; RLE codec
  checkpoint/ CheckpointStore: dense ring / sparse / bookmark / head states + validity rules
  persist/   ProjectStore: .nesrec package, atomic writes, journal, crash recovery (FILE_FORMAT.md)
  input/     InputPipeline: physical ids -> actions -> turbo/SOCD -> final bitfields; hotkeys separate
  render/    OfflineRenderer: snapshot of active take on a fresh core (export path)
  video/     FlashFilter: display-side photosensitive flash reduction (pure function of shown frames;
             never touches sessions or hashes; see FLASH_REDUCTION.md)
  harness/   DeterminismHarness: multi-run + mid-savestate comparison, first divergent frame/component
  core/      ICore; NestopiaCore (pinned settings); MockCore (fast deterministic fake for tests)
  util/      SHA-256, CRC32, Hasher64, JSON, byte codecs, Fs (the ONLY OS-specific file: fsync/rename)
  testrom/   generator of our own NROM test ROM (hand-assembled 6502)
```

Rules: only `core/` knows Nestopia; only `util/Fs.cpp` has OS calls; only `capi/` is exported.

## 5. Determinism audit (what was found and done)

* `rand()/time()` in the core: `NstCpu.cpp` RAM power-on option 2 (`std::rand()`) — never used:
  `SetRamPowerState(0)` is set before load and before every power cycle. `NstApiBarcodeReader.cpp`
  (`srand(time())`) — barcode reader is never connected. `NstBoardEvent.cpp` uses only a DIP value,
  no host time. No other host clock / RNG use in `source/core`.
* Mutable statics: `Input::Pad::mic` (written, always 0 for our pads), `Cpu::logged`
  (unofficial-opcode log mask, only feeds a user callback we never install), static callback holders
  (never set). None can influence emulation results; concurrent instances verified by test
  (`two instances run concurrently`, 4 threads + interleaved). They are formally data races on
  benign values — tolerated, documented here. ThreadSanitizer on the concurrency tests reports
  exactly these two sites (`Pad::Reset/BeginFrame/Poll` → `Pad::mic`, `Cpu::Reset` → `Cpu::logged`)
  and nothing in engine code. ASan + UBSan (minus legacy signed shifts in the core) run clean.
* Core defects found by the harness and fixed with verified build-time patches
  (`cmake/NestopiaPatches.cmake`, submodule stays pristine, each patch must match exactly once):
  1. `Apu::LoadState` dropped the 4-step frame-sequencer clock when the frame IRQ is inhibited
     (`$4017=$40`) → loaded machine ≠ uninterrupted machine (`$4015` bit 6 reads differ).
  2./3. APU output stage (resampler accumulators + produced-but-undelivered samples) not saved →
     PCM after a load was shifted by ~1 sample and channel timer state diverged. Persisted in an
     extra `RNA` chunk.
  4. `Triangle::linearCtrl` uninitialised at construction → power-on state depended on heap garbage.
  5. `Timer::M2::count` (next IRQ-counter clock cycle) not saved for the Konami VRC IRQ → after a
     load the VRC4/6/7 IRQ counter ran at another cycle phase (Gradius II diverged ~40 frames after
     loading). Saved in the `Vrc4::Irq` chunk; accessed without patching the header.
  Also: reloading a ROM in a used `Emulator` keeps some APU registers, so `loadROM` always creates
  a new `Emulator`; the optional float output filter is disabled (its state is not in states).
* Result: machine state, video and PCM are bit-identical between power-on runs and runs resumed
  from any savestate (harness compares every frame; 10 000+ frames in CI).

## 6. Thread model (macOS app; same for other frontends)

| Thread | Owns | Notes |
|---|---|---|
| Emulation thread | `rn_session`, calls `rn_input_sample_game`, `rn_step`, seek/rewind, autosave | Paced by display link / host clock: decides only *when* to step. Copies video (245 KB) into a triple buffer, pushes PCM into an SPSC ring. |
| UI / main thread | windows, menus, device callbacks → `rn_input_set_pressed` (internally locked) | Sends transport commands (pause, rewind, take switch…) to the emulation thread via a queue. Never touches `rn_session`. |
| Render thread (Metal) | latest video buffer | nearest-neighbour upscale, overscan crop, aspect. |
| Audio callback | SPSC ring consumer | Mute on pause/rewind/scrub/slow; underrun counter. Never feeds back into emulation. |
| Export thread | `rn_renderer` (created on the emulation thread) | Independent core instance; AVAssetWriter; timestamps from frame/sample counts. |

## 7. Top 5 risks

1. **Unexercised core paths still hide determinism defects** (other mappers, expansion audio,
   uninitialised members like patch 4). Mitigation: harness + `replaynes-cli determinism/verify`
   on the user's own ROMs/projects, `verify` before export, add sanitizer / Valgrind CI on Linux,
   extend the generated test ROM set (MMC1/MMC3 variants) later.
2. **Core upgrades break old projects.** Mitigation: compat ID (commit + patch level + adapter
   settings), refusal with a distinct error, legacy-core plan (COMPATIBILITY.md).
3. **Rewind/scrub latency**: a seek loads the nearest checkpoint and replays ≤ dense interval
   frames (~2 500–3 000 fps on M-series → ≤ ~12 ms). Hold-to-rewind at 60 Hz is fine, very fast
   scrubbing may stutter. Mitigation: smaller dense interval near the cursor or a per-frame state
   ring for the last seconds (states are ~5.5 KB for NROM).
4. **I/O on the emulation thread**: `autosave` fsyncs the journal (~ms), `save` writes files.
   Mitigation: call at frame boundaries with budget / while paused; later move persistence to a
   worker with a snapshot of pending deltas (engine API already separates cheap autosave).
5. **Live-play latency and pacing** (60.0988 Hz content on 60/120 Hz displays, audio buffer
   size vs underruns). Mitigation: measure (input sample → step → present) and tune; never let
   wall clock alter emulation; MP4 export is independent of live pacing.

## 8. PoCs done first

1. Core integration probe: one-frame step, state round trip, two instances, fps (≈ 2 500 fps
   with video+audio+hashing on Apple Silicon).
2. Determinism harness before any UI: found the four core defects above.
