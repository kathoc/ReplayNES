// Geometry of the desktop menus (docs/design/UI_REDESIGN.md), without ImGui: the frame every menu
// screen shares (breadcrumb / Menu pill at the top, the description line and the hint bar at the
// bottom) and the rectangles of each page kind (tiles, setting rows, cards, list rows, library,
// seek bar). Everything is fitted to the screen: nothing ever scrolls. The ImGui UI draws into
// these rectangles; test_desktop_frontend checks that every page of the menu model fits at
// 1280x800 (Steam Deck) and 1920x1080 and at the UI size limits.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <vector>

#include "replaynes/frontend.h"

namespace rnl {

struct LRect {
  float x = 0, y = 0, w = 0, h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  bool contains(const LRect& o, float eps = 0.5f) const {
    return o.x >= x - eps && o.y >= y - eps && o.right() <= right() + eps && o.bottom() <= bottom() + eps;
  }
  bool overlaps(const LRect& o) const { return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
};

/// Visual constants (px at scale 1 = 800 px high). Three text sizes only.
struct UiMetrics {
  float s = 1;  // window height / 800 * UI size
  // Text size factor of the font in use: a CJK font as the primary font (Japanese) has a taller line
  // box for its em, so its glyphs come out ~20 % smaller at the same pixel size.
  float text = 1;
  float W = 1280, H = 800;
  // Text: title 22 / label 15 / hint 12 (the spec's sizes) times kText for a TV / handheld distance.
  static constexpr float kText = 1.3f;
  float title() const { return 22 * kText * s * text; }
  float label() const { return 15 * kText * s * text; }
  float hint() const { return 12 * kText * s * text; }
  float margin() const { return 24 * s; }
  float gap() const { return 10 * s; }
  float panelRadius() const { return 16 * s; }
  float tileRadius() const { return 12 * s; }
  float ring() const { return 2 * s < 2 ? 2 : 2 * s; }
  float minTouch() const { return 40 * s; }
  /// Top bar (breadcrumb, Menu pill) and bottom bar (description, hints).
  LRect topBar() const { return {margin(), margin() * 0.75f, W - 2 * margin(), title() + 12 * s}; }
  LRect bottomBar() const { return {margin(), H - margin() * 0.75f - hint() - 18 * s, W - 2 * margin(), hint() + 18 * s}; }
  /// Where a page's content goes.
  LRect content() const {
    LRect t = topBar(), b = bottomBar();
    float top = t.bottom() + 16 * s, bottom = b.y - 12 * s;
    return {margin(), top, W - 2 * margin(), bottom - top};
  }
  static UiMetrics make(float width, float height, float uiScale, float textScale = 1);
};

struct PageGeometry {
  LRect content;              // the page's area (UiMetrics::content)
  LRect panel;                // background panel (settings / list / custom); w = 0 for tiles / cards
  LRect header;               // settings tabs / list sheet line (h = 0 if none)
  std::vector<LRect> items;   // tiles / rows / cards (a LIST: the rows of one sheet)
};

/// Rectangles of a page of the menu model. count: items (LIST: rows on the current sheet, at most
/// RNF_MENU_MAX_ITEMS; CARDS: cards). hasHeader: a settings group / list sheets line above the items.
/// reserveRows: SETTINGS / LIST panels keep room for that many rows (a list with several sheets keeps
/// its size from sheet to sheet).
PageGeometry layoutPage(const UiMetrics& m, rnf_menu_page_kind kind, size_t count, int columns, bool hasHeader,
                        size_t reserveRows = 0);

/// Height of one card's text strip (name + length) under its thumbnail.
float cardTextHeight(const UiMetrics& m);

struct LibraryGeometry {
  LRect content;
  LRect hero;                // "Continue" card (h = 0 without one)
  std::vector<LRect> cards;  // one page of game cards
  int columns = 4, rows = 2;
  LRect emptyCard;           // the empty-library card
};
LibraryGeometry layoutLibrary(const UiMetrics& m, bool hero);
float libraryCardTextHeight(const UiMetrics& m);

struct SeekGeometry {
  LRect panel;   // bottom panel
  LRect strip;   // the filmstrip (with the A/B lane above it)
  LRect lane;
  LRect timeLeft, timeRight;
  LRect hints;
};
SeekGeometry layoutSeek(const UiMetrics& m);

/// The Menu pill: top-right corner of the window; overPicture when it would cover the game
/// picture (FILL / narrow windows): drawn with low contrast and faded during play.
struct PillGeometry {
  LRect rect;
  bool overPicture = false;
};
PillGeometry layoutPill(const UiMetrics& m, float pillWidth, const LRect& picture);

/// Every rectangle of g inside the screen / content and not overlapping its siblings (layout checks).
bool pageFits(const UiMetrics& m, const PageGeometry& g, const char** why);

}  // namespace rnl
