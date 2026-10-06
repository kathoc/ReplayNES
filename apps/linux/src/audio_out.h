// Audio output through an SDL3 audio stream (48 kHz mono s16 in; SDL converts to the device).
// Dynamic rate control (rnf_audio_rate from the shared core, +-0.5 %) is applied with
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

#include "audio_sink.h"
#include "replaynes/frontend.h"

namespace rnl {

class AudioOut : public AudioSink {
 public:
  AudioOut();
  ~AudioOut() override;
  bool open(std::string* error);
  void close();
  /// Frames are emulated at `fps` (DRC base ratio = nominal / fps).
  void setEmulationRate(double fps);
  /// One emulated frame's samples (only call while audible).
  void push(const int16_t* pcm, size_t n) override;
  void setMuted(bool muted) override;
  /// 0...1 (settings).
  void setVolume(float v);
  bool muted() const { return muted_; }

  uint64_t underruns() const { return underruns_.load(); }
  double ratio() const { return rnf_audio_rate_ratio(drc_); }
  double fillMs() const { return lastFill_ * 1000.0 / RN_SAMPLE_RATE; }
  int deviceFrames() const { return deviceFrames_; }
  const std::string& deviceName() const { return deviceName_; }

 private:
  static void SDLCALL onGet(void* user, SDL_AudioStream* stream, int additional, int total);
  SDL_AudioStream* stream_ = nullptr;
  rnf_audio_rate* drc_ = nullptr;
  float volume_ = 1.0f;
  std::atomic<uint64_t> underruns_{0};
  std::atomic<bool> counting_{false};
  bool muted_ = true;
  int primedFrames_ = 0;
  int deviceFrames_ = 0;
  double lastFill_ = 0;
  std::string deviceName_;
};

}  // namespace rnl
