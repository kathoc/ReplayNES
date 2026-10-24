# Generic core boundary: media info, audio positions, input layouts, per-button turbo (2026-10-24)

Follows `2026-10-24-product-profile.md`. Goal: the engine, the shared frontend and the Linux app run a
core whose picture size, frame rate, audio format and controls differ from the NES (an arcade core:
288x224 … 384x224, vertical screens, 59.63 / 60 / 60.6 Hz, stereo, 2 players with up to 3 buttons,
coin / start / service, analog stick and throttle on some games), **without changing ReplayNES**:
the golden test (per-frame digests and the frozen project) must keep passing at every step, and
existing projects stay byte-compatible.

## 1. Media info (step S2)

```cpp
// engine/src/core/ICore.h
struct MediaInfo {
  uint32_t maxWidth = 256, maxHeight = 240;         // capacity of video(); fixed per loaded content
  uint32_t fpsNum = 39375000, fpsDen = 655171;      // exact emulated frame rate
  uint32_t audioChannels = 1;                       // 1 or 2 (interleaved int16)
  uint32_t pixelAspectNum = 8, pixelAspectDen = 7;  // display width / height of one source pixel
  uint32_t rotation = 0;                            // display rotation, quarter turns clockwise
};
struct VideoFrame { const uint32_t* pixels; uint32_t width, height, stride; };  // stride in pixels
class ICore {
  virtual MediaInfo mediaInfo() const;              // default: the NES values above
  virtual VideoFrame videoFrame() const;            // default: {video(), 256, 240, 256}
  // Audio sample frames (per channel) emitted before logical frame `frame`: a pure function of the
  // frame index, as today. Default: the NTSC formula (frame * 48000 * 655171 / 39375000). A core
  // whose output is a constant number of samples per frame returns frame * that number.
  virtual uint64_t audioSamplesBefore(uint64_t frame) const;
  uint32_t audioSamplesForFrame(uint64_t f) const { return audioSamplesBefore(f + 1) - audioSamplesBefore(f); }
};
```

- The audio rate follows from the function and the frame rate (samples per frame × fps); the frontend
  resamples to the device as it already does (DRC). No core resamples internally.
- `audio(count)`: `count` is sample frames; the buffer holds `count * audioChannels` values.
- C API (additive; existing calls unchanged and still valid for NES):
  `rn_session_media_info(s, rn_media_info*)`, `rn_video_frame(s, rn_video_frame_info*)`,
  `rn_session_audio_samples_before(s, frame)`, and the same two for `rn_renderer`. `RN_VIDEO_*`,
  `RN_FPS_*` and `RN_SAMPLE_RATE` stay as the NES defaults.
- Frontend: every use of `RN_VIDEO_*`, `RN_FPS_*`, `rnf_frame_period()` and mono 48 kHz in pacing,
  export, thumbnails, frame history, VTR reel, stream layout and the flash filter takes the media info
  (or the current frame size) as a parameter. NES callers pass today's values, so their output is
  identical (golden test, `test_flash_filter_exact`, the frontend tests).
- The cadence lock tolerance (`RNF_CADENCE_LOCK_TOLERANCE`, 0.5 %) becomes a parameter so a 59.63 Hz
  or 60.6 Hz game can lock to a 60 Hz display.

## 2. Input layouts and records (step S3)

```cpp
constexpr int kMaxPorts = 4, kMaxAxes = 8;
struct InputRecord {                 // what is applied to the core and recorded
  uint32_t buttons[kMaxPorts] = {};  // bit i = button i of that port's layout
  int16_t axes[kMaxAxes] = {};       // signed, full range
  uint8_t events = 0;                // kEvSoftReset / kEvPowerCycle (unchanged)
};
struct InputLayout {                 // declared by the core for the loaded content
  struct Button { std::string id; std::string label; };   // id: "a", "b1", "coin", "start", "up" …
  struct Axis { std::string id; int port; std::string label; };  // "x", "y", "throttle"
  int ports;
  std::vector<std::vector<Button>> buttons;    // per port, bit order
  std::vector<std::pair<int, int>> socdPairs;  // per-port button indices of opposite directions
  std::vector<Axis> axes;
  // Storage: the record's bytes in this order; the NES layout encodes as p1, p2, events.
  size_t recordBytes() const;
  void encode(const InputRecord&, uint8_t* out) const;
  bool decode(const uint8_t* in, InputRecord&) const;
};
```

- **The NES layout encodes a record as exactly today's three bytes (p1, p2, events)**, so segments,
  journals and `manifest.json` of ReplayNES projects are unchanged (`inputFormatVersion` stays 1 for
  the NES layout). A non-NES layout writes `inputFormatVersion: 2` and its layout (ids, bit order,
  axes) into the manifest; the RLE becomes `{varint run, recordBytes bytes}`. Opening checks that the
  stored layout equals the core's.
- `ICore::stepFrame(const InputRecord&, bool renderVideo)`; the NES cores read `buttons[0..1]` as
  bytes. Session, Timeline, DeterminismHarness, OfflineRenderer carry `InputRecord` instead of
  `(p1, p2, events)`. C API: `rn_step_record(s, const rn_input_record*, info)` next to the existing
  `rn_step` (which stays, building the NES record).
- **Coins, start and service are ordinary buttons of the layout** (recorded like any button). DIP
  switch settings belong to the content's configuration, stored in the manifest, not to the record.

## 3. Actions, bindings and turbo (step S3)

- Action ids are built from the layout: `p<port>.<button id>` (NES: `p1.a`, `p1.up` … unchanged),
  analog `p<port>.axis.<axis id>`, hotkeys unchanged. The binding store keys actions by name, so the
  32-action limits (`latch_`, `latchSeq_[32]`) go away.
- **Turbo on any button**: `<action>.turbo` is bindable for every game button (NES `p1.turbo_a` stays
  as an alias of `p1.a.turbo`, so saved bindings load unchanged). Period and press length are global
  defaults (`rn_input_set_turbo`, unchanged) with optional per-button values
  (`rn_input_set_button_turbo(in, "p1.b1", period, duty)`, saved in the bindings JSON). Turbo is
  still applied before recording: the recorded input is the input after turbo.
- Analog: `rn_input_bind_axis(in, "gc0:lstick.x", "p1.axis.x", scale)`; the sampled record carries the
  axis value. Digital bindings to directions keep working (an arcade stick game can also be played
  with the d-pad).
- SOCD uses the layout's direction pairs instead of the NES bit positions.
- `rn_input_sample_record(in, frame, rn_input_record*)` next to `rn_input_sample_game` (unchanged).
- Frontend: the action catalog (`kActions`), groups (`RNF_GROUP_PLAYER1..4`, `RNF_GROUP_SYSTEM`),
  labels and default bindings come from the layout plus a product table; the resume countdown, the
  paused d-pad step and `rnf_transport_step` / `rnf_retry_step` take the record or the layout's
  direction ids instead of NES bits.

## 4. Content identity and loading (step S4)

- `ICore::loadContent(const ContentRef&)` with `{path, bytes}`; NES cores keep reading bytes. A
  content id replaces the ROM SHA-256 in sessions, projects, the library and the catalog: for NES it
  *is* the SHA-256 of the file (unchanged manifests); an arcade core reports
  `set name + sorted (file, CRC32) list` hashed.

## Order and checks

S2 then S3 then S4, each as several small commits. After every commit: full ctest (golden,
`test_flash_filter_exact`, `test_desktop_frontend`), and for S3 additionally a test that loads the
frozen project and a project written with a mock non-NES layout.
