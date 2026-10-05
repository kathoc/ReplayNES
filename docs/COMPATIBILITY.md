# Core compatibility and reproducibility policy

A take is reproducible only on the exact same emulation core behaviour. ReplayNES therefore pins
the core and refuses — never silently converts — projects made with a different one.

## Core compatibility ID

```
nestopia-ue@<first 12 hex of the pinned commit>+p<patch level>+adapter<adapter version>+ntsc
e.g. nestopia-ue@7b5c87d8dc3c+p2+adapter1+ntsc
```

| Part | Source | Changes when |
|---|---|---|
| commit | `REPLAYNES_NESTOPIA_COMMIT` in `cmake/NestopiaCore.cmake` | the submodule is bumped. Configure **fails** if the checked-out submodule differs from the pin or has local modifications (override only for source tarballs: `REPLAYNES_SKIP_CORE_PIN_CHECK=ON`). |
| patch level | `REPLAYNES_NESTOPIA_PATCHLEVEL` in `cmake/NestopiaPatches.cmake` | any build-time core patch is added/changed. Each patch must match exactly once or configure fails. |
| adapter version | `NestopiaCore::kAdapterVersion` | any pinned setting below changes. |
| region | `ntsc` | (v1 is NTSC only) |

`coreBuild` (manifest, informational) additionally records the full commit and compiler.

### Pinned adapter settings (adapter 1)

NTSC NES (RP2C02 timing, 39375000/655171 Hz); RAM power-on state 0x00 (`SetRamPowerState(0)`,
re-applied before power cycles); no image database (header-only detection); video 256×240 32-bit
BGRA, YUV palette, canonical decoder, all picture controls 0, no NTSC filter; audio 48 000 Hz mono
16-bit, default volumes, no speed adaption/transpose, optional output filter **off**; exactly
`floor((f+1)·48000·655171/39375000) − floor(f·48000·655171/39375000)` samples for frame f;
controllers: standard pads on ports 1/2, simultaneous opposite directions allowed (SOCD is decided
upstream and recorded); soft reset = `Machine::Reset(false)`, power cycle = `Machine::Reset(true)`;
each ROM load uses a new `Emulator` instance.

### Build-time core patches (patch level 2)

See `cmake/NestopiaPatches.cmake` (rationale per patch) and ARCHITECTURE_DECISION.md §5:
1 frame-IRQ clock kept across `LoadState`; 2/3 APU output stage saved/restored (`RNA` chunk);
4 `Triangle::linearCtrl` initialised; 5 (patch level 2) the VRC IRQ timer phase (`Timer::M2::count`)
saved in the `Vrc4::Irq` chunk (VRC4/VRC6/VRC7 and two boards reusing it; found with Gradius II,
mapper 25). Projects recorded with `+p1` are refused by `+p2` builds (core mismatch). These belong upstream (jgemu/nestopia); once merged, the pin
moves to the upstream commit and the patch level resets — which is a new compat ID.

## Other version numbers

| Field | Meaning |
|---|---|
| `formatVersion` | package layout/JSON schema. Older versions are read (upgrade on next save); newer → `RN_ERR_UNSUPPORTED_FORMAT`. |
| `inputFormatVersion` | button bit order + event bits. 1 = A,B,Select,Start,Up,Down,Left,Right; events bit0 soft reset, bit1 power cycle. |
| `stateFormatVersion` | engine envelope around core states. States are a cache: a mismatch only discards checkpoints, never inputs. |

## Mismatch policy

* Different `coreCompatId` → `RN_ERR_CORE_MISMATCH` with both IDs in the message. The UI must
  explain it and offer: open with a matching ReplayNES build / legacy runner (below), or an explicit
  migration. There is no "open anyway".
* A state file whose compat ID differs from the manifest → also `RN_ERR_CORE_MISMATCH`.
* ROM SHA-256 mismatch → `RN_ERR_ROM_MISMATCH` (no override); missing ROM → `RN_ERR_ROM_NOT_FOUND`
  (user re-locates it; verified by SHA-256).

## Legacy cores and migration (plan)

The input log is core-independent data; only its *interpretation* is core-specific.

1. **Keep old releases runnable**: every release documents its compat ID; release artifacts are
   archived. `replaynes-cli info` prints a project's compat ID so users can pick the right build.
2. **Legacy runner (preferred for in-app support)**: build older pinned cores as separate
   `replaynes-legacy-<compatId>` helper executables (same engine sources at that tag). The app talks
   to them out-of-process (replay/export only), avoiding C++ symbol clashes between two Nestopia
   versions in one binary and keeping each legacy build bit-identical to its release.
3. **Explicit migration** (user-initiated, never automatic): replay the active take on the old core
   and on the new core side by side, compare per-frame video+audio hashes (harness). Only if the
   whole take is identical is the project rewritten with the new compat ID (states regenerated,
   original kept as a backup copy). Otherwise migration is refused with the first divergent
   frame, and the project stays on its original core.
4. When the pin moves, CI must run the harness and the corpus of recorded test projects with the
   new core; any change to recorded results is a compat-ID change by definition.
