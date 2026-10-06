// Small ImGui helpers of the Linux UI: icon buttons drawn with shapes (no icon font needed),
// hold buttons (active while pressed, mouse / touch / gamepad), status pills.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>

#include "imgui.h"
#include "imgui_internal.h"

namespace rnl::ui {

enum class Icon { play, pause, stepBack, stepForward, rewind, fastForward, toStart, record, repeat, close };

inline void drawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float r, ImU32 col) {
  auto tri = [&](float x0, float dir) {  // dir +1 pointing right, -1 left
    dl->AddTriangleFilled(ImVec2(x0 - dir * r * 0.5f, c.y - r * 0.7f), ImVec2(x0 - dir * r * 0.5f, c.y + r * 0.7f),
                          ImVec2(x0 + dir * r * 0.6f, c.y), col);
  };
  float bw = std::fmax(2.0f, r * 0.28f);
  switch (icon) {
    case Icon::play: tri(c.x + r * 0.1f, 1); break;
    case Icon::pause:
      dl->AddRectFilled(ImVec2(c.x - r * 0.5f, c.y - r * 0.65f), ImVec2(c.x - r * 0.5f + bw * 1.2f, c.y + r * 0.65f), col);
      dl->AddRectFilled(ImVec2(c.x + r * 0.5f - bw * 1.2f, c.y - r * 0.65f), ImVec2(c.x + r * 0.5f, c.y + r * 0.65f), col);
      break;
    case Icon::stepForward:
      tri(c.x - r * 0.15f, 1);
      dl->AddRectFilled(ImVec2(c.x + r * 0.5f, c.y - r * 0.65f), ImVec2(c.x + r * 0.5f + bw, c.y + r * 0.65f), col);
      break;
    case Icon::stepBack:
      tri(c.x + r * 0.15f, -1);
      dl->AddRectFilled(ImVec2(c.x - r * 0.5f - bw, c.y - r * 0.65f), ImVec2(c.x - r * 0.5f, c.y + r * 0.65f), col);
      break;
    case Icon::fastForward:
      tri(c.x - r * 0.45f, 1);
      tri(c.x + r * 0.45f, 1);
      break;
    case Icon::rewind:
      tri(c.x + r * 0.45f, -1);
      tri(c.x - r * 0.45f, -1);
      break;
    case Icon::toStart:
      tri(c.x + r * 0.3f, -1);
      dl->AddRectFilled(ImVec2(c.x - r * 0.55f - bw, c.y - r * 0.65f), ImVec2(c.x - r * 0.55f, c.y + r * 0.65f), col);
      break;
    case Icon::record: dl->AddCircleFilled(c, r * 0.55f, col); break;
    case Icon::repeat:
      dl->AddCircle(c, r * 0.6f, col, 0, bw);
      dl->AddTriangleFilled(ImVec2(c.x + r * 0.35f, c.y - r * 0.95f), ImVec2(c.x + r * 0.35f, c.y - r * 0.25f),
                            ImVec2(c.x + r * 0.85f, c.y - r * 0.6f), col);
      break;
    case Icon::close:
      dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r * 0.5f), ImVec2(c.x + r * 0.5f, c.y + r * 0.5f), col, bw);
      dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r * 0.5f), ImVec2(c.x + r * 0.5f, c.y - r * 0.5f), col, bw);
      break;
  }
}

/// A button showing an icon. Returns true when clicked / activated.
inline bool IconButton(const char* id, Icon icon, ImVec2 size, bool enabled = true, ImU32 bg = 0) {
  if (bg) ImGui::PushStyleColor(ImGuiCol_Button, bg);
  ImGui::BeginDisabled(!enabled);
  bool r = ImGui::Button(id, size);
  ImGui::EndDisabled();
  if (bg) ImGui::PopStyleColor();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImU32 col = enabled ? IM_COL32(235, 238, 245, 255) : IM_COL32(140, 140, 150, 160);
  drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), (b.y - a.y) * 0.32f, col);
  return r;
}

/// Pressed-and-held button (rewind / fast-forward): *held follows the press (mouse, touch, A).
inline void HoldButton(const char* id, Icon icon, ImVec2 size, bool* held, bool enabled = true) {
  bool on = *held;
  if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  ImGui::BeginDisabled(!enabled);
  ImGui::Button(id, size);
  ImGui::EndDisabled();
  if (on) ImGui::PopStyleColor();
  *held = enabled && ImGui::IsItemActive();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), (b.y - a.y) * 0.32f,
           enabled ? IM_COL32(235, 238, 245, 255) : IM_COL32(140, 140, 150, 160));
}

/// Gives the controller / keyboard focus to the next item submitted, like SetKeyboardFocusHere(0)
/// but without activating it (that would open a slider or number field as a text field).
inline void FocusNextItem() {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  if (g.DragDropActive || g.MovingWindow != nullptr) return;
  ImGui::SetNavWindow(g.CurrentWindow);
  ImGui::NavMoveRequestSubmit(ImGuiDir_None, ImGuiDir_Down, ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi,
                              ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
  g.NavTabbingDir = 1;
  g.NavTabbingCounter = 1;
  ImGui::SetNavCursorVisible(true);
}

/// A coloured status pill on the foreground draw list; returns its width.
inline float Pill(ImDrawList* dl, ImVec2 p, const char* text, ImU32 bg, float fontSize, ImU32 fg = IM_COL32(255, 255, 255, 255)) {
  ImFont* f = ImGui::GetFont();
  ImVec2 ts = f->CalcTextSizeA(fontSize, 10000, 0, text);
  float padX = fontSize * 0.5f, padY = fontSize * 0.2f;
  dl->AddRectFilled(p, ImVec2(p.x + ts.x + padX * 2, p.y + ts.y + padY * 2), bg, fontSize * 0.3f);
  dl->AddText(f, fontSize, ImVec2(p.x + padX, p.y + padY), fg, text);
  return ts.x + padX * 2;
}

}  // namespace rnl::ui
