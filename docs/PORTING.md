# Writing a frontend (Windows / Linux / other) on the ReplayNES engine

The engine (`engine/`) is portable C++17 with no UI or OS media APIs. Frontends use the C API
in `engine/include/replaynes/replaynes.h` plus the shared frontend core (`frontend/`, C API
`frontend/include/replaynes/frontend.h`) for everything a frontend decides that is not
platform-specific. The macOS app (Swift) is one such frontend; the desktop frontend for Linux
and Windows is another, and future iOS frontends follow the same pattern.

The desktop frontend is one C++ code base with small platform seams:

| | Shared (`apps/desktop`) | Linux (`apps/linux`) | Windows (`apps/windows`) |
|---|---|---|---|
| UI, frame loop, input, audio | Dear ImGui UI, `runApp` frame loop, `DisplayScheduler`, input routing, OSK, SDL3 audio + DRC | - | - |
| `Renderer` (`renderer.h`) | `GameRect`, present-timing interface | Vulkan FIFO + `VK_KHR_present_wait`, Vulkan CRT | Direct3D 11 flip model + DXGI frame statistics (CRT: not yet) |
| Platform services (`platform/platform.h`, `paths.h`, `host_clock.h`, `fonts.h`) | | XDG folders, freedesktop Trash, `SDL_OpenURL`, CLOCK_MONOTONIC, fontconfig | Known Folders, Recycle Bin (`IFileOperation`), Explorer, QPC + high-resolution timers, `%WINDIR%\Fonts` |
| Optional features | `update_service.h`, `export/mp4_export.h`, `render/crt_export.h`, `ui_features.h` | Flatpak portal updates, FFmpeg export, Add to Steam | not yet (stubs) |

New desktop platforms implement those seams; `rnl_logic` (no SDL) and its tests
(`test_desktop_frontend`) build everywhere, also on macOS.

## Build

```sh
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # MSVC: -G "Visual Studio 17 2022"
cmake --build build
ctest --test-dir build -j8
```

Link `replaynes_engine` + `nestopia_core` (both static) and the platform thread library
(`Threads::Threads`). In CMake: `add_subdirectory(<ReplayNES>)` then
`target_link_libraries(myfrontend PRIVATE replaynes_engine)`. Options: `REPLAYNES_BUILD_TESTS`,
`REPLAYNES_BUILD_CLI`. OS-specific code lives only in `engine/src/util/Fs.cpp`
(`fsync`/`_commit`, directory sync, `std::filesystem` rename); paths are UTF-8 everywhere.

## Shared frontend core (`frontend/`)

`replaynes_frontend` is a static C++17 library (no windowing, graphics, audio, controller or
media APIs) built by the top-level CMake on every platform (`-DREPLAYNES_BUILD_FRONTEND=OFF` to
skip it). It links `replaynes_engine`; `target_link_libraries(myfrontend PRIVATE
replaynes_frontend)` is enough. Building it needs `python3` (the localization table is
generated). Its C API (`rnf_*`) follows the engine's conventions: `rn_status` codes, no C++
exception crosses the boundary, `rnf_last_error()` is thread-local, opaque handles,
`char*` results freed with `rnf_string_free`, sized outputs with the two-call pattern
(`n = f(..., NULL, 0)`, then `f(..., buf, n)`). Handles are not thread-safe (one owner thread)
except `rnf_thumb_cache` and `rnf_rom_hash_cache`; free functions are pure.

What lives where:

| Area | Core (`frontend/src`) | Stays in each frontend |
|---|---|---|
| Display pacing | cadence (`rnf_cadence`: which refresh starts a frame, locked k-th refresh / free), just-in-time input deadline, direct-vs-composited present path, backlog-drain policy, CRT build-ahead decision, audio DRC ratio, Hermite resampler, judder / callback counters | display link / vsync callback, clocks, present / GPU timing, audio device |
| Input | action catalog + labels, default bindings (`RNF_KEYBOARD_MACOS` kVK codes, `RNF_KEYBOARD_SDL` scancodes; controllers identical), saved-layout migrations (as unbind/bind plans, `rnf_input_apply_plan`), paused D-pad step map, display names, controller families + printed labels (incl. Steam Deck), face positions by family + glyph (Apple GameController), SDL3 button / axis / type / label and XInput mappings, controller diagram geometry + badge texts | device enumeration and events (`rn_input_set_pressed` / `set_axis`), drawing the diagram |
| Session | resume record (`resume.json`), launch decision, single-instance lock, `rnf_resume_apply`, "has content" | the folder (`~/Library/Application Support/...`, `$XDG_DATA_HOME/ReplayNES/Session`), prompts |
| Timeline | frame <-> x mapping, A/B drag / hit test / "Set A/B Here", take lineage, visible A/B ranges, filmstrip grid (fixed 5 s * 2^k scale, tiles, targets), thumbnail cache policy (thread-safe, opaque retained payloads), 2x2 box downscaler | thumbnail images / textures, rendering threads |
| Library | `ROM/` + `Projects/` folders, ROM scan (frontend may pass its collation; default natural order), project scan + SHA-256 matching, ROM hash cache, project / backup names | the root folder, file watching, Trash |
| Transport | record toggle plan, slow toggle, step repeater, practice A/B loop (hold -> rewind animation -> restart), frame history, audio fade tail, fast-forward over the take | wall clock, presenting the animation |
| Export / streaming | export presets, validation, geometry + nearest-neighbour column/row maps, bit rate; streaming canvas sizes | encoders (AVFoundation, FFmpeg), Syphon / PipeWire |
| Text | localization table + `rnf_format` (Apple-style `%@`, `%lld`, positional `%1$@`), language rule (`rnf_ui_language_choose`: Japanese if the first preferred language is Japanese), flash level texts | widgets |

Localization: `apps/macos/Resources/Localizable.xcstrings` stays the single source of truth.
`frontend/tools/gen_l10n.py` turns it into the compiled table (and
`build/.../frontend/generated/l10n.json` for tooling) at build time. Display strings returned by
the core use the language set with `rnf_l10n_set_language("ja" | "en")` (call once at startup);
a frontend's own UI strings use `rnf_l10n_lookup(key)` / `rnf_l10n_format(key, args, n)` with
the English source text as key. Strings used by the core are marked `RNF_L("...")` in
`frontend/src`; `scripts/check-l10n.py` counts them as used catalog keys. New UI strings of any
frontend are added to the String Catalog with a Japanese translation.

Determinism of the math: the core is compiled with `-ffp-contract=off` (no FMA contraction), so
the pacing and geometry decisions are the same on every platform and identical to the original
Swift code. Tests: `tests/test_frontend_*.cpp` (ctest).

A new frontend typically: sets the language, loads bindings (`rn_input_load_json`, or
`rnf_input_default_config_json(scheme)` on first run) and applies the migrations, maps device
buttons to positional ids (`gc<slot>:face.south` ... via `rnf_sdl_button_element` /
`rnf_gc_face_positions`), drives `rnf_cadence_refresh` from its vsync callback and
`rnf_input_deadline_*` around input sampling, feeds audio through `rnf_audio_rate_update` +
`rnf_resampler_process`, keeps a `rnf_thumb_cache` for the filmstrip, and uses the resume record,
lock and library functions with its own folders.

## Main loop (emulation thread)

```c
rn_session* s; rn_input* in = rn_input_new();
rn_session_open(dir, NULL, NULL, &s);            // or rn_session_new(rom, dir, NULL, &s)
if (rn_session_recovered(s)) tell_user("recovered unsaved work");
rn_input_load_json(in, settings_json);           // remap / turbo / SOCD / hotkeys

for (;;) {                                       // woken by the display (see FRAME_PACING.md)
    drain_ui_commands();                         // seek, rewind, take switch, bookmark, mode...
    uint32_t edges, held; rn_input_poll_hotkeys(in, &edges, &held);   // also while paused
    handle_hotkeys(edges, held);
    if (paused && !frame_advance_requested) continue;
    uint8_t p1, p2; rn_input_sample_game(in, rn_frame(s), &p1, &p2);
    rn_step_info info;
    if (rn_step(s, p1, p2, pending_events, &info) != RN_OK) show_error(rn_last_error());
    pending_events = 0;                          // RN_EV_SOFT_RESET / RN_EV_POWER_CYCLE
    publish_video(rn_video(s));                  // copy 256*240*4 bytes to the renderer
    size_t n; const int16_t* pcm = rn_audio(s, &n);
    if (!muted) audio_ring_push(pcm, n);         // 48 kHz mono s16
    if (time_for_autosave()) rn_session_autosave(s);   // every few seconds
}
rn_session_save(s); rn_session_close(s);         // close never saves implicitly
```

Rules:
* Pause, slow motion (1/2, 1/4) and N-frame advance only change **when** `rn_step` is called.
  They never change recorded data; final replay runs at normal speed.
* Recording at a frame before the take end creates a new take automatically (`info.branched`);
  the old future stays available (`rn_take_*`, `rn_undo_take_switch`).
* `rn_audio` returns 0 samples after seek/rewind/take switches; mute during scrub/rewind/slow.
* Never feed wall-clock corrections back into emulation (no "catch-up" steps that depend on
  audio drift). Pace frames from the display (one frame per refresh, or per 2 refreshes at
  120 Hz) and resample the audio stream slightly instead; docs/FRAME_PACING.md describes the
  macOS implementation and the equivalents on iOS, Windows and Linux.
* A controller disconnect: `rn_input_release_prefix(in, "<device prefix>")` and pause.

## Practice mode and A/B repeat

`RN_MODE_PRACTICE` plays with live input but records nothing: `rn_frame`, the take, checkpoints
and `rn_session_has_unsaved_changes` stay as they were. Leaving it (`rn_set_mode(s,
RN_MODE_RECORD/REPLAY)`) restores the take state at the frame practice was entered from.
Per project there are 8 A/B slots (`RN_PRACTICE_SLOTS`), saved with the project (full save and
autosave journal); slot edits do mark the project unsaved.

```c
rn_practice_set_a(s, slot);          // any mode: A = current machine state, counter = 0, B cleared
/* ...play (record, replay or practice)... */
if (rn_practice_set_b(s, slot) == RN_ERR_DISCONTINUITY) tell_user("seeked since A: set A again");
rn_practice_slot_rename(s, slot, "boss");
/* Or straight from take frames (timeline selection, no continuous play needed; not in practice).
   The cursor, state and picture are restored exactly; rn_audio reports 0 samples afterwards. */
rn_practice_set_range(s, slot, a_frame, b_frame);   // A = state at a_frame, length = b - a

rn_practice_goto_a(s, slot);         // enters practice (remembers take position), loads A
for (;;) {                           // A/B loop, per emulation tick
    rn_step(s, p1, p2, events, NULL);            // nothing recorded
    rn_practice_slot_info si; rn_practice_slot_get(s, slot, &si);
    if (si.has_b && rn_practice_frame(s) >= si.length_frames) {
        pause_ms(500); play_rewind_animation(); rn_practice_goto_a(s, slot);  // autoplay again
    }
}
rn_rewind(s, n);                     // in practice: rewinds the practice run (R2 hold), not the take
rn_set_mode(s, RN_MODE_RECORD);      // leave practice: take exactly as before
```

* `rn_practice_frame` = frames since the current anchor (last set A / goto A / practice start);
  `rn_practice_get_status` also gives the anchor slot, return frame and how far practice rewind can go.
* Practice rewind keeps a bounded snapshot ring (dense-checkpoint policy: every `dense_interval`
  frames, `dense_capacity` entries) plus the practice inputs; it is exact and stops at the anchor.
* In practice `rn_seek`, `rn_bookmark_add/goto`, `rn_take_activate`, `rn_undo_take_switch` return
  `RN_ERR_WRONG_MODE`; leave practice first. Saving while practicing saves the take position.
* B needs unbroken emulation since that slot's A anchor: seek/take rewind, bookmark goto, take
  switch/undo, leaving practice, another goto A or reopening the project break it
  (`RN_ERR_DISCONTINUITY`); `rn_practice_goto_a(slot)` re-anchors so B can be re-set.
* Practice is never persisted: a reopened project starts in its take mode at the take cursor.

## Threads

* `rn_session` / `rn_renderer`: not thread-safe; one owner thread each (serialize calls).
* `rn_input`: internally locked; device callbacks may call `rn_input_set_*` from any thread.
* `rn_last_error()` is thread-local.
* Export: create `rn_renderer_new(s, 0, 0, &r)` on the emulation thread, then run
  `rn_renderer_next` on a worker thread while the user keeps playing.

## Input ids

Physical ids are opaque, stable strings chosen by the frontend. The shared core's catalog and
defaults use `kb:<code>` (macOS virtual key code or SDL scancode, see `rnf_keyboard_scheme`) and
`gc<slot>:<element>` with POSITIONAL elements (`face.south/east/west/north`, `dpad.up`,
`leftShoulder`, `menu`, `options`, ...: NES A = east, NES B = south on every pad). Analog sticks via
`rn_input_set_axis(in, "gc0:lstick", x, y)` derive `gc0:lstick.left/right/up/down` with the
configured threshold. Bind with
`rn_input_bind(in, id, "p1.a")`; action names are listed in `replaynes.h`. Hotkeys (`hk.*`) never
reach the game bitfields.

## Export (MP4 or anything else)

```c
rn_renderer* r; rn_renderer_new(s, start, end /*0=all*/, &r);
const uint32_t* px; const int16_t* pcm; size_t n; uint64_t f;
while (rn_renderer_next(r, &px, &pcm, &n, &f) == RN_OK) {
    // video PTS = (f - start) * RN_FPS_DEN / RN_FPS_NUM seconds  (rational, exact)
    // audio: n samples, starting at sample rn_audio_samples_before(f) - rn_audio_samples_before(start)
    encode_video(px /*BGRA 256x240*/, f - start);  // scale nearest-neighbour, crop overscan, 8:7 PAR
    encode_audio(pcm, n);
    report_progress(rn_renderer_frames_done(r), rn_renderer_total_frames(r));
}
rn_renderer_free(r);                                // cancel = stop calling + free
```
On Windows use Media Foundation (H.264/AAC) or FFmpeg (libx264 + aac); on Linux FFmpeg/GStreamer.
Timestamps must come from frame/sample counts, never from the wall clock.

## Photosensitive flash reduction (display only)

Run the frames you **show** (and, optionally, export) through the engine's flash filter; it never
touches the session, so recorded input and every hash stay the same. Default to "standard".

```c
rn_flash_filter* ff = rn_flash_filter_new(RN_FLASH_STANDARD);   // OFF / LOW / STANDARD / HIGH
rn_flash_info info;
rn_flash_filter_process(ff, rn_video(s), display_copy, &info);  // 256x240 BGRA in -> out
present(display_copy); show_indicator(info.altered);
rn_flash_filter_reset(ff);   // on seek, scrub, bookmark/take jump, load, new session, rewind start
```
Feed one processed frame per *displayed* frame (fast-forward: only the shown one), and while
paused re-process the current frame each display tick while `info.altered` so a held picture
settles. For export use a fresh filter per export and process every rendered frame in order.
Algorithm and limits: [FLASH_REDUCTION.md](FLASH_REDUCTION.md).

## Streaming output (optional)

The macOS app publishes the displayed frame (after the flash filter, before any UI) as a Syphon
server named "ReplayNES" for OBS (`apps/macos/Sources/App/SyphonOutput.swift`): poll the same
display frame copy from a separate thread, scale nearest-neighbour, and never let the consumer
block the emulation thread. Equivalents for future frontends:

- Windows: [Spout](https://spout.zeal.co) (OBS via the Spout2 plugin), sharing a D3D11 texture.
- Linux: v4l2loopback (appears as a camera) or a PipeWire video stream (OBS PipeWire source).

Audio needs no special output: OBS captures application audio on every OS.

## Errors to handle in UI

| Code | Meaning / action |
|---|---|
| `RN_ERR_ROM_NOT_FOUND` | ask the user for the ROM, reopen with `rom_override` |
| `RN_ERR_ROM_MISMATCH` | the chosen file is a different ROM (SHA-256 shown in message) |
| `RN_ERR_CORE_MISMATCH` | project from another core build — see COMPATIBILITY.md; do not open |
| `RN_ERR_CORRUPT` | name of the damaged file in the message; for damaged states offer reopen with `RN_OPEN_DROP_CORRUPT_STATES`; for a damaged practice slot ("practice slot N" in the message) offer `RN_OPEN_DROP_CORRUPT_PRACTICE` (that slot is lost) |
| `RN_ERR_DISCONTINUITY` | B could not be set: emulation was interrupted since A (set A again / goto A) |
| `RN_ERR_WRONG_MODE` | take navigation attempted while practicing: leave practice first |
| `RN_ERR_UNSUPPORTED_FORMAT` | project written by a newer app |
| `RN_ERR_DISK_FULL` / `RN_ERR_IO` | keep running, keep data in memory, retry save later |

## Tools

`replaynes-cli make-test-rom | sha256 | info | verify | determinism | record-random | render-hash`
(see `tools/replaynes-cli/main.cpp`). `verify` replays a project's active take on a fresh core and
checks every stored checkpoint — useful in bug reports from any platform.
