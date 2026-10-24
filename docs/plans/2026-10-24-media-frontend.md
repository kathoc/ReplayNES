# Media info in the shared frontend and the Linux app (2026-10-24)

Follows `2026-10-24-generic-core-boundary.md` section 1. The engine now reports, per session,
`rn_media_info` (maximum picture size, exact frame rate, audio channels, pixel aspect, display
rotation), the picture of the last frame with its size and stride (`rn_session_video_frame`,
`rn_renderer_video_frame`), sample positions (`rn_session_samples_before`) and a flash filter for any
size that is a multiple of 16 (`rn_flash_filter_new_sized`, `_set_size`, `_process_frame`). A product
can register its own core (`RN_CORE_EXTERNAL`). This step makes `frontend/` and `apps/desktop`,
`apps/linux` use them, so such a core plays, rewinds, shows thumbnails and exports correctly.

**ReplayNES must not change.** With the NES cores every value equals today's constants, and every
output (picture placement, pacing decisions, audio, exported files, thumbnails) must stay the same.

## Shared frontend (`frontend/`)

- Frame period: every user of `rnf_frame_period()` / `RN_FPS_*` (pacing, tape, cadence) takes the
  session's frame rate. Keep `rnf_frame_period()` as the NES default and add an explicit variant.
- Cadence lock tolerance (`RNF_CADENCE_LOCK_TOLERANCE`, 0.5 %): a parameter with today's value as the
  default, so a product can allow 59.63 Hz and 60.6 Hz games to lock to a 60 Hz display (1.5 %).
- Export (`export.cpp`): source width/height, frame rate (timescale and frame duration), audio
  channels and sample rate, pixel aspect and rotation come from the media info. The 8:7 option
  stays for the NES; for other cores the choice is "the game's pixel aspect" or 1:1. Crop limits are
  relative to the source size.
- Thumbnails, frame history, VTR reel (`timeline.cpp`, `playback.cpp`, `vtr.cpp`): frames of any size
  up to the maximum; thumbnails keep their width (128) and follow the picture's aspect after rotation.
- Stream output layout and anything else in the research map that hard-codes 256x240.

## Linux app (`apps/desktop`, `apps/linux`)

- Emulation keeps the picture with its size; the renderer receives width, height, stride and rotation
  (allocate the GPU image at the maximum size once; copy extent and UVs follow the current frame;
  rotation in the blit UVs). `GameRect` uses the pixel aspect (8:7 option = NES only) and swaps
  width/height for 90/270 degrees.
- Audio output: the device stays 48 kHz; the stream's input format is the core's rate (samples per
  frame x fps) and channel count, and the DRC targets follow from them. The audio sink takes a
  channel count.
- Pacing: the game's frame period everywhere `nesFramePeriod()` is used.
- Export: the game's size, frame rate, audio rate and channels (MP4 timescale from fps_num/fps_den).
- Flash reduction: the sized filter; CRT display only when the picture is the NES picture (256x240
  with PPU codes); otherwise the CRT setting is unavailable and the plain picture is shown.
- Library thumbnails, filmstrip tiles and library cards: the picture's aspect after rotation.

## Done when

- Unix Makefiles build (`build-make`, ninja is not installed): all ctest tests pass, including
  `test_golden`, `test_media`, `test_flash_filter_exact` and `test_desktop_frontend`.
- New frontend tests cover a non-NES media info (e.g. 320x224 stereo at 59.63 Hz, rotated 90 degrees):
  export geometry and timing, thumbnail size, frame history with changing frame sizes, cadence
  locking at 59.63 Hz on a 60 Hz display with the wider tolerance.
- The Linux app compiles in the Freedesktop SDK (as for the product profile step) and still prints
  `ReplayNES 0.6.4` for `--version`.
- A list of every place that still assumes 256x240 / NTSC / mono, with the reason it is NES-only
  (CRT signal model, PPU codes, test cartridge).
