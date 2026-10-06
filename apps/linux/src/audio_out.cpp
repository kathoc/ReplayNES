// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio_out.h"

#include <algorithm>
#include <vector>

namespace rnl {

AudioOut::AudioOut() : drc_(rnf_audio_rate_new(1600)) {}
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
  rnf_audio_rate_free(drc_);
  drc_ = rnf_audio_rate_new(1.5 * chunk + 800);
  SDL_SetAudioStreamGain(stream_, volume_);
  SDL_SetAudioStreamGetCallback(stream_, &AudioOut::onGet, this);
  SDL_ResumeAudioStreamDevice(stream_);
  return true;
}

void AudioOut::close() {
  if (stream_) SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
}

void SDLCALL AudioOut::onGet(void* user, SDL_AudioStream*, int additional, int) {
  auto* self = static_cast<AudioOut*>(user);
  if (additional > 0 && self->counting_.load(std::memory_order_relaxed)) self->underruns_.fetch_add(1);
}

void AudioOut::setEmulationRate(double fps) { rnf_audio_rate_set_frame_rate(drc_, fps, 0); }

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
