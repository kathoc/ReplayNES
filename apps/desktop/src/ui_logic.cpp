// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui_logic.h"

#include <algorithm>

namespace rnl {

int scrubStepFrames(double heldSeconds) {
  if (heldSeconds < 0.6) return 1;
  if (heldSeconds < 1.5) return 4;
  if (heldSeconds < 3.0) return 15;
  return 60;
}

uint64_t timelineJumpTarget(uint64_t frame, uint64_t takeLength, const std::vector<uint64_t>& bookmarks, int dir) {
  const uint64_t fiveSeconds = 300;  // NES frames (60.0988 fps)
  frame = std::min(frame, takeLength);
  if (dir > 0) {
    uint64_t best = UINT64_MAX;
    for (uint64_t b : bookmarks)
      if (b > frame && b <= takeLength) best = std::min(best, b);
    if (best != UINT64_MAX) return best;
    return std::min(takeLength, frame + fiveSeconds);
  }
  bool found = false;
  uint64_t best = 0;
  for (uint64_t b : bookmarks)
    if (b < frame && (!found || b > best)) {
      best = b;
      found = true;
    }
  if (found) return best;
  return frame > fiveSeconds ? frame - fiveSeconds : 0;
}

std::string padGlyph(rnf_controller_family family, const char* element) {
  const char* l = rnf_controller_family_label(family, element);
  return l ? std::string(l) : std::string();
}

std::string menuChordGlyph(rnf_controller_family family, bool controllerConnected) {
  if (!controllerConnected) return "Esc";
  return family == RNF_FAMILY_PLAYSTATION ? "L1+R1" : "L+R";
}

}  // namespace rnl
