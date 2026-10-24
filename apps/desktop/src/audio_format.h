// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstddef>
#include "replaynes/frontend.h"

namespace rnl {

// SDL's input format and DRC use sample frames, independently of the channel count.
struct AudioStreamFormat {
  double sampleRate = RN_SAMPLE_RATE;
  uint32_t channels = 1;
  double frameRate = 1.0 / rnf_frame_period();
  double samplesPerPush = 800;

  static AudioStreamFormat forMedia(const rn_media_info& media, double rate) {
    AudioStreamFormat f;
    f.sampleRate = rate > 0 ? rate : RN_SAMPLE_RATE;
    f.channels = media.audio_channels;
    f.frameRate = 1.0 / rnf_frame_period_for_media(&media);
    // Preserve the established NES queue targets (800 samples, rounded to 60 Hz).
    const bool nesTiming = media.fps_num == RN_FPS_NUM && media.fps_den == RN_FPS_DEN &&
                           f.sampleRate == RN_SAMPLE_RATE;
    f.samplesPerPush = nesTiming ? 800 : f.sampleRate / f.frameRate;
    return f;
  }
  int streamRate() const { return int(std::lround(sampleRate)); }
  size_t bytesPerFrame() const { return sizeof(int16_t) * channels; }
  double targetFill(int deviceFrames) const {
    return 1.5 * deviceFrames * sampleRate / RN_SAMPLE_RATE + samplesPerPush;
  }
};

}  // namespace rnl
