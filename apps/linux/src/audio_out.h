// Audio output through an SDL3 audio stream (48 kHz mono s16 in; SDL converts to the device).
// Dynamic rate control (interim::AudioRateControl, +-0.5 %) is applied with
// SDL_SetAudioStreamFrequencyRatio, so emulation can follow the display (60.000 Hz) while the
// stream level stays at its target. Underruns are counted in the stream's get-callback (the
// device asked for more than was queued while audible).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "interim/pacing_logic.h"

namespace rnl {

class AudioOut {
 public:
  bool open(std::string* error);
  void close();
  /// Frames are emulated at `fps` (DRC base ratio = nominal / fps).
  void setEmulationRate(double fps);
  /// Emulated frames that may arrive together (2 on displays slower than the NES rate): the kept
  /// level grows by one frame of audio per extra frame, so the bursts do not drain it.
  void setFramesPerPush(int frames);
  /// One emulated frame's samples (only call while audible).
  void push(const int16_t* pcm, size_t n);
  void setMuted(bool muted);
  bool muted() const { return muted_; }

  uint64_t underruns() const { return underruns_.load(); }
  double ratio() const { return drc_.ratio; }
  double fillMs() const { return lastFill_ * 1000.0 / RN_SAMPLE_RATE; }
  int deviceFrames() const { return deviceFrames_; }
  const std::string& deviceName() const { return deviceName_; }

 private:
  static void SDLCALL onGet(void* user, SDL_AudioStream* stream, int additional, int total);
  SDL_AudioStream* stream_ = nullptr;
  interim::AudioRateControl drc_;
  double baseFill_ = 1568;
  std::atomic<uint64_t> underruns_{0};
  std::atomic<bool> counting_{false};
  bool muted_ = true;
  int primedFrames_ = 0;
  int deviceFrames_ = 0;
  double lastFill_ = 0;
  std::string deviceName_;
};

}  // namespace rnl
