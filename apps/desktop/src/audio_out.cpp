// SPDX-License-Identifier: GPL-2.0-or-later
#include "replaynes/product.h"
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
  baseFill_ = format_.targetFill(chunk);
  rnf_audio_rate_free(drc_);
  drc_ = rnf_audio_rate_new(baseFill_);
  if (rate_ > 0) rnf_audio_rate_set_frame_rate(drc_, rate_, format_.frameRate);
  SDL_SetAudioStreamGain(stream_, volume_);
  SDL_SetAudioStreamGetCallback(stream_, &AudioOut::onGet, this);
  SDL_ResumeAudioStreamDevice(stream_);
  return true;
}

void AudioOut::configureMedia(const rn_media_info& media, double sampleRate) {
  AudioStreamFormat next = AudioStreamFormat::forMedia(media, sampleRate);
  if (next.sampleRate == format_.sampleRate && next.channels == format_.channels &&
      next.frameRate == format_.frameRate) return;
  format_ = next;
  if (stream_) {
    SDL_ClearAudioStream(stream_);
    SDL_AudioSpec input{SDL_AUDIO_S16, int(format_.channels), format_.streamRate()};
    SDL_SetAudioStreamFormat(stream_, &input, nullptr);
  }
  baseFill_ = format_.targetFill(deviceFrames_ > 0 ? deviceFrames_ : 1024);
  rnf_audio_rate_free(drc_);
  drc_ = rnf_audio_rate_new(baseFill_);
  rate_ = format_.frameRate;
  rnf_audio_rate_set_frame_rate(drc_, rate_, format_.frameRate);
  lastFill_ = 0;
  primedFrames_ = 0;
  counting_ = false;
}

void AudioOut::close() {
  if (stream_) SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
}

void SDLCALL AudioOut::onGet(void* user, SDL_AudioStream*, int additional, int total) {
  auto* self = static_cast<AudioOut*>(user);
  if (additional > 0 && self->counting_.load(std::memory_order_relaxed)) {
    self->underruns_.fetch_add(1);
    if (std::getenv(productEnvName("DEBUG_AUDIO").c_str()))
      std::fprintf(stderr, "underrun t=%.3f additional=%d total=%d\n", nowSeconds(), additional / 2, total / 2);
  }
}

void AudioOut::setEmulationRate(double fps) {
  rate_ = fps;
  rnf_audio_rate_set_frame_rate(drc_, fps, format_.frameRate);
}

void AudioOut::setFramesPerPush(int frames) {
  double target = baseFill_ + format_.samplesPerPush * std::max(0, frames - 1);
  if (target == rnf_audio_rate_target_fill(drc_)) return;
  rnf_audio_rate_free(drc_);  // the core's DRC has a fixed target: a new one (base ratio re-applied)
  drc_ = rnf_audio_rate_new(target);
  if (rate_ > 0) rnf_audio_rate_set_frame_rate(drc_, rate_, format_.frameRate);
  if (stream_ && !muted_) {
    // Top up at once (silence) instead of waiting for the +-0.5 % rate control to get there.
    int queued = SDL_GetAudioStreamQueued(stream_) / int(format_.bytesPerFrame());
    int need = int(target) - queued;
    if (need > 0) {
      std::vector<int16_t> silence(size_t(need) * format_.channels, 0);
      SDL_PutAudioStreamData(stream_, silence.data(), int(size_t(need) * format_.bytesPerFrame()));
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

void AudioOut::push(const int16_t* pcm, size_t n, uint32_t channels) {
  if (channels != format_.channels) return;
  if (!stream_ || muted_ || n == 0) return;
  int queued = SDL_GetAudioStreamQueued(stream_) / int(format_.bytesPerFrame());
  if (primedFrames_ == 0) {
    // (Re)start: fill to the target with silence so the first device pull does not run dry.
    int need = int(rnf_audio_rate_target_fill(drc_)) - queued - int(n);
    if (need > 0) {
      std::vector<int16_t> silence(size_t(need) * format_.channels, 0);
      SDL_PutAudioStreamData(stream_, silence.data(), int(size_t(need) * format_.bytesPerFrame()));
      queued += need;
    }
    rnf_audio_rate_reset(drc_);
  }
  lastFill_ = queued;
  double r = rnf_audio_rate_update(drc_, queued);
  // SDL's ratio speeds playback up (> 1); ours is output samples per input sample.
  SDL_SetAudioStreamFrequencyRatio(stream_, float(format_.sampleRate / format_.streamRate() / r));
  SDL_PutAudioStreamData(stream_, pcm, int(n * format_.bytesPerFrame()));
  if (primedFrames_ < 3 && ++primedFrames_ == 3) counting_ = true;  // a few frames in: pulls are steady
}

}  // namespace rnl
