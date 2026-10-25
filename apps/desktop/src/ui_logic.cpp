// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui_logic.h"

#include <algorithm>

namespace rnl {

int markerUnderCursor(const std::vector<uint64_t>& markers, uint64_t cursor) {
  for (size_t i = 0; i < markers.size() && i < 2; ++i)
    if (markers[i] == cursor) return int(i);
  return -1;
}

MarkerEdit markerNorth(const std::vector<uint64_t>& markers, uint64_t cursor) {
  MarkerEdit e;
  const int on = markerUnderCursor(markers, cursor);
  if (on >= 0) {  // delete it: the other one (if any) stays as the section's only marker
    if (markers.size() < 2) {
      e.kind = MarkerEdit::clear;
    } else {
      e.kind = MarkerEdit::setA;
      e.a = markers[size_t(1 - on)];
    }
    return e;
  }
  if (markers.empty()) {
    e.kind = MarkerEdit::setA;
    e.a = cursor;
  } else if (markers.size() == 1) {
    e.kind = MarkerEdit::setRange;
    e.a = std::min(markers[0], cursor);
    e.b = std::max(markers[0], cursor);
  } else {
    e.kind = MarkerEdit::full;
  }
  return e;
}

MarkerEdit markerWest(const std::vector<uint64_t>& markers) {
  MarkerEdit e;
  e.kind = markers.size() >= 2 ? MarkerEdit::practice : MarkerEdit::needTwo;
  return e;
}

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
