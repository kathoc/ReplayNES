// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui_logic.h"

#include <algorithm>

namespace rnl {

MenuTransition menuTransition(MenuPage current, MenuCommand cmd, bool hasSession, MenuPage previous) {
  MenuTransition t;
  if (cmd == MenuCommand::viewButton) {
    if (current != MenuPage::guide) {
      t.page = MenuPage::guide;
    } else {
      t.page = previous == MenuPage::guide ? (hasSession ? MenuPage::playback : MenuPage::library) : previous;
      if (!hasSession && t.page == MenuPage::playback) t.page = MenuPage::library;
      if (hasSession && t.page == MenuPage::library) t.page = MenuPage::playback;
    }
    return t;
  }
  if (!hasSession) {
    // Start screen: Library <-> Settings with Menu, B returns to the Library.
    if (cmd == MenuCommand::menuButton) t.page = current == MenuPage::settings ? MenuPage::library : MenuPage::settings;
    else t.page = MenuPage::library;
    return t;
  }
  // In a session: a page returns to the hub; the hub resumes play.
  if (current == MenuPage::playback || current == MenuPage::library) {
    t.page = MenuPage::playback;
    t.closeAndResume = current == MenuPage::playback;
    return t;
  }
  t.page = MenuPage::playback;
  return t;
}

MenuPage cyclePage(MenuPage current, int dir) {
  static const MenuPage order[] = {MenuPage::practice, MenuPage::takes, MenuPage::bookmarks, MenuPage::guide};
  const int n = int(sizeof order / sizeof order[0]);
  for (int i = 0; i < n; ++i)
    if (order[i] == current) return order[((i + (dir < 0 ? -1 : 1)) % n + n) % n];
  return current;
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

}  // namespace rnl
