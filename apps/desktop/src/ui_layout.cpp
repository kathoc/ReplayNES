// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui_layout.h"

#include <algorithm>
#include <cmath>

namespace rnl {

UiMetrics UiMetrics::make(float width, float height, float uiScale) {
  UiMetrics m;
  m.W = std::max(1.0f, width);
  m.H = std::max(1.0f, height);
  m.s = std::clamp(m.H / 800.0f, 0.6f, 3.0f) * uiScale;
  // A very wide UI size on a small window: keep the six tiles / four cards on screen.
  m.s = std::min(m.s, m.W / 800.0f);
  return m;
}

float cardTextHeight(const UiMetrics& m) { return m.label() + m.hint() + 22 * m.s; }
float libraryCardTextHeight(const UiMetrics& m) { return m.label() + m.hint() + 20 * m.s; }

PageGeometry layoutPage(const UiMetrics& m, rnf_menu_page_kind kind, size_t count, int columns, bool hasHeader) {
  PageGeometry g;
  g.content = m.content();
  const LRect& c = g.content;
  const float s = m.s, gap = m.gap();
  switch (kind) {
    case RNF_MENU_PAGE_TILES: {
      size_t n = std::max<size_t>(1, count);
      float w = std::min(184 * s, (c.w - gap * float(n - 1)) / float(n));
      float h = std::min(w * 1.08f, c.h);
      float total = w * float(n) + gap * float(n - 1);
      float x = c.x + (c.w - total) / 2, y = c.y + (c.h - h) * 0.46f;
      for (size_t i = 0; i < count; ++i) g.items.push_back({x + float(i) * (w + gap), y, w, h});
      break;
    }
    case RNF_MENU_PAGE_SETTINGS:
    case RNF_MENU_PAGE_LIST: {
      const float pad = 14 * s, rowGap = 8 * s;
      float headerH = hasHeader ? m.label() + 22 * s : 0;
      float pw = std::min(880 * s, c.w);
      size_t rows = std::max<size_t>(1, std::min<size_t>(count, RNF_MENU_MAX_ITEMS));
      // Room for a full sheet: the panel keeps its size when a page has fewer rows.
      size_t slots = kind == RNF_MENU_PAGE_LIST ? RNF_MENU_MAX_ITEMS : rows;
      float avail = c.h - 2 * pad - headerH - (headerH > 0 ? rowGap : 0);
      float rowH = std::min(64 * s, (avail - rowGap * float(slots - 1)) / float(slots));
      float ph = 2 * pad + headerH + (headerH > 0 ? rowGap : 0) + rowH * float(slots) + rowGap * float(slots - 1);
      g.panel = {c.x + (c.w - pw) / 2, c.y, pw, ph};
      if (hasHeader) g.header = {g.panel.x + pad, g.panel.y + pad, pw - 2 * pad, headerH};
      float y = g.panel.y + pad + headerH + (headerH > 0 ? rowGap : 0);
      for (size_t i = 0; i < std::min<size_t>(count, RNF_MENU_MAX_ITEMS); ++i)
        g.items.push_back({g.panel.x + pad, y + float(i) * (rowH + rowGap), pw - 2 * pad, rowH});
      break;
    }
    case RNF_MENU_PAGE_CARDS: {
      size_t cols = size_t(std::max(1, columns));
      size_t n = std::max<size_t>(1, count), rows = (n + cols - 1) / cols;
      float text = cardTextHeight(m);
      float w = std::min(268 * s, (c.w - gap * float(cols - 1)) / float(cols));
      float h = w * 15.0f / 16.0f + text;
      float maxH = (c.h - gap * float(rows - 1)) / float(rows);
      if (h > maxH) {
        h = maxH;
        w = (h - text) * 16.0f / 15.0f;
      }
      float gw = w * float(cols) + gap * float(cols - 1), gh = h * float(rows) + gap * float(rows - 1);
      float x0 = c.x + (c.w - gw) / 2, y0 = c.y + (c.h - gh) * 0.3f;
      for (size_t i = 0; i < count; ++i)
        g.items.push_back({x0 + float(i % cols) * (w + gap), y0 + float(i / cols) * (h + gap), w, h});
      break;
    }
    case RNF_MENU_PAGE_CUSTOM: {
      float pw = std::min(1000 * s, c.w);
      g.panel = {c.x + (c.w - pw) / 2, c.y, pw, c.h};
      break;
    }
  }
  return g;
}

LibraryGeometry layoutLibrary(const UiMetrics& m, bool hero) {
  LibraryGeometry g;
  g.content = m.content();
  const LRect& c = g.content;
  const float s = m.s, gap = m.gap();
  float y = c.y;
  if (hero) {
    g.hero = {c.x, y, c.w, std::min(150 * s, c.h * 0.3f)};
    y += g.hero.h + 18 * s;
  }
  g.columns = 4;
  g.rows = 2;
  float w = (c.w - gap * 3) / 4;
  float gridH = c.bottom() - y;
  float h = std::min((gridH - gap) / 2, w * 0.8f);
  for (int r = 0; r < g.rows; ++r)
    for (int col = 0; col < g.columns; ++col) g.cards.push_back({c.x + float(col) * (w + gap), y + float(r) * (h + gap), w, h});
  float ew = std::min(720 * s, c.w), eh = std::min(300 * s, c.h);
  g.emptyCard = {c.x + (c.w - ew) / 2, c.y + (c.h - eh) * 0.4f, ew, eh};
  return g;
}

SeekGeometry layoutSeek(const UiMetrics& m) {
  SeekGeometry g;
  const float s = m.s, pad = 16 * s;
  float laneH = 16 * s, stripH = 60 * s, hintsH = m.hint() + 10 * s;
  float h = pad * 2 + laneH + stripH + 8 * s + hintsH;
  g.panel = {m.margin(), m.H - m.margin() * 0.75f - h, m.W - 2 * m.margin(), h};
  float timeW = m.label() * 4.6f;
  float x0 = g.panel.x + pad, x1 = g.panel.right() - pad;
  g.timeLeft = {x0, g.panel.y + pad + laneH, timeW, stripH};
  g.timeRight = {x1 - timeW, g.timeLeft.y, timeW, stripH};
  g.lane = {x0 + timeW + 10 * s, g.panel.y + pad, (x1 - timeW - 10 * s) - (x0 + timeW + 10 * s), laneH};
  g.strip = {g.lane.x, g.lane.bottom(), g.lane.w, stripH};
  g.hints = {x0, g.strip.bottom() + 8 * s, x1 - x0, hintsH};
  return g;
}

PillGeometry layoutPill(const UiMetrics& m, float pillWidth, const LRect& picture) {
  PillGeometry p;
  float h = m.label() + 14 * m.s;
  LRect t = m.topBar();
  p.rect = {m.W - m.margin() - pillWidth, t.y + (t.h - h) / 2, pillWidth, h};
  p.overPicture = picture.w > 0 && p.rect.overlaps(picture);
  return p;
}

bool pageFits(const UiMetrics& m, const PageGeometry& g, const char** why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  LRect screen{0, 0, m.W, m.H};
  if (!screen.contains(g.content)) return fail("content outside the screen");
  LRect area = g.panel.w > 0 ? g.panel : g.content;
  if (g.panel.w > 0 && !g.content.contains(g.panel)) return fail("panel taller / wider than the content area");
  if (g.header.h > 0 && !area.contains(g.header)) return fail("header outside the panel");
  for (size_t i = 0; i < g.items.size(); ++i) {
    const LRect& r = g.items[i];
    if (!area.contains(r) || !g.content.contains(r)) return fail("item outside the content area");
    if (r.w < m.minTouch() || r.h < m.minTouch() * 0.9f) return fail("item smaller than a touch target");
    if (g.header.h > 0 && r.overlaps(g.header)) return fail("item overlaps the header");
    for (size_t j = i + 1; j < g.items.size(); ++j)
      if (r.overlaps(g.items[j])) return fail("items overlap");
  }
  return true;
}

}  // namespace rnl
