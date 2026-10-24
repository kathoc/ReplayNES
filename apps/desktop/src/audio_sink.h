// Where the emulation controller sends the sound of the frames it emulates (AudioOut on the
// device; a counter in tests). Interleaved s16 sample frames.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include "replaynes/replaynes.h"

namespace rnl {

class AudioSink {
 public:
  virtual ~AudioSink() = default;
  virtual void setMuted(bool muted) = 0;
  virtual void configureMedia(const rn_media_info&, double sampleRate) { (void)sampleRate; }
  /// Channel-aware sinks override this entry; legacy mono sinks keep their existing entry.
  virtual void push(const int16_t* pcm, size_t n, uint32_t channels) { (void)channels; push(pcm, n); }
  /// One emulated frame's samples (ignored while muted).
  virtual void push(const int16_t* pcm, size_t n) = 0;
};

}  // namespace rnl
