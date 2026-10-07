// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio_out.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "host_clock.h"
#include <vector>

namespace rnl {

AudioOut::AudioOut() : drc_(rnf_audio_rate_new(1568)) {}
AudioOut::~AudioOut() {
  close();
  rnf_audio_rate_free(drc_);
}

void AudioOut::setVolume(float v) {
  volume_ = std::clamp(v, 0.0f, 1.0f);
  if (stream_) SDL_SetAudioStreamGain(stream_, volume_);
}

bool AudioOut::open(std::string* error) {
  SDL_AudioSpec spec{SDL_AUDIO_S16, 1, RN_SAMPLE_RATE};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) { *error = std::string("audio: ") + SDL_GetError(); return false; }
  SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream_);
  SDL_AudioSpec devSpec{};
  int frames = 0;
  if (SDL_GetAudioDeviceFormat(dev, &devSpec, &frames)) deviceFrames_ = frames;
  if (const char* n = SDL_GetAudioDeviceName(dev)) deviceName_ = n;
  deviceName_ += std::string(" (") + SDL_GetCurrentAudioDriver() + ", " + std::to_string(devSpec.freq) + " Hz, " +
                 std::to_string(deviceFrames_) + " frames)";
  // Level kept in the stream: the device's chunk plus one emulated frame and half a chunk of slack.
  int chunk = deviceFrames_ > 0 ? deviceFrames_ : 1024;
  baseFill_ = 1.5 * chunk + 800;
  rnf_audio_rate_free(drc_);
  drc_ = rnf_audio_rate_new(baseFill_);
  if (rate_ > 0) rnf_audio_rate_set_frame_rate(drc_, rate_, 0);
  SDL_SetAudioStreamGain(stream_, volume_);
  SDL_SetAudioStreamGetCallback(stream_, &AudioOut::onGet, this);
  SDL_ResumeAudioStreamDevice(stream_);
  return true;
}

void AudioOut::close() {
  if (stream_) SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
}

void SDLCALL AudioOut::onGet(void* user, SDL_AudioStream*, int additional, int total) {
  auto* self = static_cast<AudioOut*>(user);
  if (additional > 0 && self->counting_.load(std::memory_order_relaxed)) {
    self->underruns_.fetch_add(1);
    if (std::getenv("REPLAYNES_DEBUG_AUDIO"))
      std::fprintf(stderr, "underrun t=%.3f additional=%d total=%d\n", nowSeconds(), additional / 2, total / 2);
  }
}

void AudioOut::setEmulationRate(double fps) {
  rate_ = fps;
  rnf_audio_rate_set_frame_rate(drc_, fps, 0);
}

void AudioOut::setFramesPerPush(int frames) {
  double target = baseFill_ + 800.0 * std::max(0, frames - 1);
  if (target == rnf_audio_rate_target_fill(drc_)) return;
  rnf_audio_rate_free(drc_);  // the core's DRC has a fixed target: a new one (base ratio re-applied)
  drc_ = rnf_audio_rate_new(target);
  if (rate_ > 0) rnf_audio_rate_set_frame_rate(drc_, rate_, 0);
  if (stream_ && !muted_) {
    // Top up at once (silence) instead of waiting for the +-0.5 % rate control to get there.
    int queued = SDL_GetAudioStreamQueued(stream_) / 2;
    int need = int(target) - queued;
    if (need > 0) {
      std::vector<int16_t> silence(size_t(need), 0);
      SDL_PutAudioStreamData(stream_, silence.data(), need * 2);
    }
  }
}

void AudioOut::setMuted(bool muted) {
  if (muted == muted_) return;
  muted_ = muted;
  if (muted) {
    counting_ = false;
    if (stream_) SDL_ClearAudioStream(stream_);
  } else {
    primedFrames_ = 0;
  }
}

void AudioOut::push(const int16_t* pcm, size_t n) {
  if (!stream_ || muted_ || n == 0) return;
  int queued = SDL_GetAudioStreamQueued(stream_) / 2;
  if (primedFrames_ == 0) {
    // (Re)start: fill to the target with silence so the first device pull does not run dry.
    int need = int(rnf_audio_rate_target_fill(drc_)) - queued - int(n);
    if (need > 0) {
      std::vector<int16_t> silence(size_t(need), 0);
      SDL_PutAudioStreamData(stream_, silence.data(), need * 2);
      queued += need;
    }
    rnf_audio_rate_reset(drc_);
  }
  lastFill_ = queued;
  double r = rnf_audio_rate_update(drc_, queued);
  // SDL's ratio speeds playback up (> 1); ours is output samples per input sample.
  SDL_SetAudioStreamFrequencyRatio(stream_, float(1.0 / r));
  SDL_PutAudioStreamData(stream_, pcm, int(n * 2));
  if (primedFrames_ < 3 && ++primedFrames_ == 3) counting_ = true;  // a few frames in: pulls are steady
}

}  // namespace rnl
