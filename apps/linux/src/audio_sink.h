// Where the emulation controller sends the sound of the frames it emulates (AudioOut on the
// device; a counter in tests). 48 kHz mono s16.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace rnl {

class AudioSink {
 public:
  virtual ~AudioSink() = default;
  virtual void setMuted(bool muted) = 0;
  /// One emulated frame's samples (ignored while muted).
  virtual void push(const int16_t* pcm, size_t n) = 0;
};

}  // namespace rnl
