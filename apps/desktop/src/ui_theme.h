// The menus' visual language (docs/design/UI_REDESIGN.md): always dark (they sit over the game),
// translucent panels (#141416 at 92 %), radius 16 (tiles 12), a 2 px brand-red (#FF3B3B) focus
// ring, 120 ms ease-out animations, one font in three sizes (ui_layout.h), Tabler icons.
// Drawing helpers on ImDrawList; no state.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "imgui.h"
#include "ui_layout.h"

namespace rnl::theme {

constexpr ImU32 kPanel = IM_COL32(20, 20, 22, 235);       // #141416, 92 %
constexpr ImU32 kTile = IM_COL32(34, 34, 39, 235);
constexpr ImU32 kTileFocus = IM_COL32(52, 52, 60, 245);    // raised tint
constexpr ImU32 kRow = IM_COL32(255, 255, 255, 10);
constexpr ImU32 kRowFocus = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kBrand = IM_COL32(255, 59, 59, 255);       // #FF3B3B
constexpr ImU32 kText = IM_COL32(242, 242, 246, 255);
constexpr ImU32 kTextDim = IM_COL32(170, 172, 182, 255);
constexpr ImU32 kTextFaint = IM_COL32(118, 120, 130, 255);
constexpr ImU32 kDim = IM_COL32(0, 0, 0, 166);             // the game behind menus at ~35 %
constexpr ImU32 kHairline = IM_COL32(255, 255, 255, 24);
constexpr ImU32 kKeycap = IM_COL32(236, 238, 244, 255);
constexpr ImU32 kKeycapText = IM_COL32(24, 26, 32, 255);
constexpr double kAnim = 0.120;  // seconds

inline float easeOut(double t) {
  float x = float(std::clamp(t, 0.0, 1.0));
  return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
}
inline ImU32 alpha(ImU32 c, float a) {
  unsigned al = unsigned(float((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f) + 0.5f);
  return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}
inline ImU32 mix(ImU32 a, ImU32 b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  auto ch = [&](int shift) {
    float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
    return ImU32(x + (y - x) * t + 0.5f) << shift;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}
inline ImVec2 v(float x, float y) { return ImVec2(x, y); }
inline ImVec2 tl(const LRect& r) { return ImVec2(r.x, r.y); }
inline ImVec2 br(const LRect& r) { return ImVec2(r.right(), r.bottom()); }
inline LRect lerp(const LRect& a, const LRect& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}
inline LRect inset(const LRect& r, float d) { return {r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d}; }

/// The focus ring: 2 px brand red around r (outside it by `gap`).
inline void focusRing(ImDrawList* dl, const LRect& r, float radius, float thickness, float gap, float a = 1.0f) {
  dl->AddRect(ImVec2(r.x - gap, r.y - gap), ImVec2(r.right() + gap, r.bottom() + gap), alpha(kBrand, a), radius + gap, 0,
              thickness);
}

/// Text size of `text` at `size` (one line).
inline ImVec2 measure(float size, const char* text) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text); }

/// One line of text cut with "…" to maxW. Returns true when it had to be cut.
bool textFit(ImDrawList* dl, float size, ImVec2 pos, float maxW, ImU32 col, const std::string& text);
/// Centered in r horizontally (cut to r.w).
bool textCentered(ImDrawList* dl, float size, const LRect& r, float y, ImU32 col, const std::string& text);
/// Number of texts cut since the last call (layout checks).
int takeTruncations();

/// A key cap / button glyph: rounded rect with a label (dark text on light).
float keycap(ImDrawList* dl, ImVec2 p, float h, const std::string& label, bool round, bool draw = true);

}  // namespace rnl::theme
