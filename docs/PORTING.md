# Writing a frontend (Windows / Linux / other) on the ReplayNES engine

The engine (`engine/`) is portable C++17 with no UI or OS media APIs. Frontends use only the C API
in `engine/include/replaynes/replaynes.h`. The macOS app (Swift) is one such frontend; an
SDL2/Qt/Win32 frontend follows the same pattern.

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

## Main loop (emulation thread)

```c
rn_session* s; rn_input* in = rn_input_new();
rn_session_open(dir, NULL, NULL, &s);            // or rn_session_new(rom, dir, NULL, &s)
if (rn_session_recovered(s)) tell_user("recovered unsaved work");
rn_input_load_json(in, settings_json);           // remap / turbo / SOCD / hotkeys

for (;;) {                                       // woken by vsync / timer at ~60.0988 Hz
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
  audio drift); drop/duplicate *presentation* frames instead.
* A controller disconnect: `rn_input_release_prefix(in, "<device prefix>")` and pause.

## Threads

* `rn_session` / `rn_renderer`: not thread-safe; one owner thread each (serialize calls).
* `rn_input`: internally locked; device callbacks may call `rn_input_set_*` from any thread.
* `rn_last_error()` is thread-local.
* Export: create `rn_renderer_new(s, 0, 0, &r)` on the emulation thread, then run
  `rn_renderer_next` on a worker thread while the user keeps playing.

## Input ids

Physical ids are opaque, stable strings chosen by the frontend, e.g. `kb:<scancode>`,
`pad0:a`, `pad0:dpad.up`, analog sticks via `rn_input_set_axis(in, "pad0:lstick", x, y)` which
derives `pad0:lstick.left/right/up/down` with the configured threshold. Bind with
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

## Errors to handle in UI

| Code | Meaning / action |
|---|---|
| `RN_ERR_ROM_NOT_FOUND` | ask the user for the ROM, reopen with `rom_override` |
| `RN_ERR_ROM_MISMATCH` | the chosen file is a different ROM (SHA-256 shown in message) |
| `RN_ERR_CORE_MISMATCH` | project from another core build — see COMPATIBILITY.md; do not open |
| `RN_ERR_CORRUPT` | name of the damaged file in the message; for damaged states offer reopen with `RN_OPEN_DROP_CORRUPT_STATES` |
| `RN_ERR_UNSUPPORTED_FORMAT` | project written by a newer app |
| `RN_ERR_DISK_FULL` / `RN_ERR_IO` | keep running, keep data in memory, retry save later |

## Tools

`replaynes-cli make-test-rom | sha256 | info | verify | determinism | record-random | render-hash`
(see `tools/replaynes-cli/main.cpp`). `verify` replays a project's active take on a fresh core and
checks every stored checkpoint — useful in bug reports from any platform.
