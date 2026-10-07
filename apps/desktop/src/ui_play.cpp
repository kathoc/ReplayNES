// The dock (transport + filmstrip timeline), the practice OSD and the status badges.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <vector>

#include "app_model.h"
#include "emulation.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "pad_nav.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui.h"
#include "ui_widgets.h"

namespace rnl {

namespace {
const ImU32 kSlotColors[8] = {IM_COL32(255, 149, 0, 255), IM_COL32(10, 132, 255, 255), IM_COL32(48, 209, 88, 255),
                              IM_COL32(255, 55, 95, 255),  IM_COL32(191, 90, 242, 255), IM_COL32(64, 200, 224, 255),
                              IM_COL32(255, 214, 10, 255), IM_COL32(255, 69, 58, 255)};
ImU32 slotColor(int slot, float alpha = 1.0f) {
  ImU32 c = kSlotColors[((slot % 8) + 8) % 8];
  return (c & 0x00FFFFFFu) | (ImU32(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << 24);
}
}  // namespace

// ------------------------------------------------------------------ dock

void UI::buildDock(bool inMenu, double now) {
  ImGuiIO& io = ImGui::GetIO();
  float dh = dockHeight(inMenu);
  if (!inMenu) {
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - dh));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, dh));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##dock", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
  } else {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + io.DisplaySize.x, p.y + dh), IM_COL32(20, 22, 27, 245));
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + io.DisplaySize.x, p.y), IM_COL32(255, 255, 255, 30));
    ImGui::SetCursorPos(ImVec2(S(14), ImGui::GetCursorPosY() + S(8)));
    ImGui::BeginGroup();
  }
  const EmuStatus& st = d_.emu->status();
  // Row 1: time | timeline | time
  std::string left = timecode(st.practicing ? st.practiceFrame : st.frame);
  std::string right = st.practicing ? (st.practiceLength > 0 ? timecode(st.practiceLength) : std::string("--:--.--"))
                                    : timecode(st.takeLength) + (st.unsaved ? " •" : "");
  float contentW = inMenu ? io.DisplaySize.x - S(28) : ImGui::GetContentRegionAvail().x;
  ImGui::PushFont(nullptr, S(18));
  float lw = ImGui::CalcTextSize("00:00.00").x + S(6), rw = ImGui::CalcTextSize("00:00.00 •").x + S(6);
  float textH = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  float rowH = S(18) + S(60);
  ImVec2 row = ImGui::GetCursorScreenPos();
  float y0 = ImGui::GetCursorPosY(), x0 = ImGui::GetCursorPosX();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float ty = row.y + S(18) + S(30) - textH * 0.5f;
  ImGui::PushFont(nullptr, S(18));
  dl->AddText(ImVec2(row.x, ty), IM_COL32(235, 238, 245, 255), left.c_str());
  dl->AddText(ImVec2(row.x + contentW - rw + S(6), ty), IM_COL32(160, 165, 175, 255), right.c_str());
  ImGui::PopFont();
  ImGui::SetCursorPos(ImVec2(x0 + lw, y0));
  // The timeline's focus rectangle spans the whole row (with the times): D-pad up / down from the
  // transport buttons below reach it from anywhere.
  buildTimeline(std::max(S(100), contentW - lw - rw), inMenu, now, row.x, row.x + contentW);
  ImGui::SetCursorPos(ImVec2(x0, y0 + rowH + S(6)));
  buildTransport(inMenu);
  if (inMenu) ImGui::EndGroup();
  else ImGui::End();
}

void UI::buildTransport(bool inMenu) {
  const EmuStatus& st = d_.emu->status();
  EmulationController* emu = d_.emu;
  ImVec2 bs(S(56), S(44));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (inMenu) ImGui::SetCursorPosX(S(14));
  // Record toggle: glowing red in record mode, gray in playback mode.
  {
    bool on = st.recording && !st.practicing;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz(S(132), S(44));
    if (on)
      for (int i = 3; i >= 1; --i)
        dl->AddRectFilled(ImVec2(p.x - i * S(2.5f), p.y - i * S(2.5f)), ImVec2(p.x + sz.x + i * S(2.5f), p.y + sz.y + i * S(2.5f)),
                          IM_COL32(255, 40, 40, 34), S(22) + i * S(2.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.86f, 0.12f, 0.12f, 1) : ImVec4(0.25f, 0.26f, 0.30f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.95f, 0.2f, 0.2f, 1) : ImVec4(0.32f, 0.33f, 0.38f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(22));
    ImGui::BeginDisabled(st.practicing);
    std::string label = std::string("     ") + TR("Record") + "##rec";
    if (ImGui::Button(label.c_str(), sz)) emu->toggleRecord();
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImVec2 a = ImGui::GetItemRectMin();
    dl->AddCircleFilled(ImVec2(a.x + S(22), a.y + sz.y * 0.5f), S(7), on ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 160, 255));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoNavOverride))
      ImGui::SetTooltip("%s", on ? TR("Record mode (click for playback mode: plays the recorded take)")
                                 : TR("Playback mode (click to return to record mode and continue recording from here)"));
  }
  ImGui::SameLine(0, S(18));
  if (ui::IconButton("##start", ui::Icon::toStart, bs)) emu->seek(0);
  ImGui::SameLine();
  bool rew = false;
  ui::HoldButton("##rew", ui::Icon::rewind, bs, &rew);
  if (rew) emu->setRewindHeld(true);
  ImGui::SameLine();
  if (ui::IconButton("##back", ui::Icon::stepBack, bs, st.paused)) emu->stepBack(1);
  ImGui::SameLine();
  if (ui::IconButton("##play", st.paused ? ui::Icon::play : ui::Icon::pause, ImVec2(S(72), bs.y), true,
                     IM_COL32(60, 66, 80, 255))) {
    if (inMenu) closeHubAndResume();  // playing: back to the game
    else emu->togglePause();
  }
  ImGui::SameLine();
  if (ui::IconButton("##fwd", ui::Icon::stepForward, bs, st.paused)) emu->frameAdvance(1);
  ImGui::SameLine();
  bool ff = false;
  ui::HoldButton("##ff", ui::Icon::fastForward, bs, &ff, !st.practicing);
  if (ff) emu->setFastForwardHeld(true);
  ImGui::SameLine();
  {
    bool slow = st.slow != 1;
    if (slow) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.25f, 0.6f, 1));
    std::string l = slow ? TRF("Slow %@", {std::string("1/2")}) : std::string(TR("Slow"));
    if (ImGui::Button((l + "##slow").c_str(), ImVec2(0, bs.y))) emu->toggleSlow();
    if (slow) ImGui::PopStyleColor();
  }
  if (!inMenu) {
    ImGui::SameLine(0, S(18));
    bool on = st.practicing || practicePanel_;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.42f, 0.08f, 1));
    std::string l = std::string(st.practicing ? TR("Stop Practicing") : TR("Practice")) + "##practice";
    if (ImGui::Button(l.c_str(), ImVec2(0, bs.y))) {
      if (st.practicing) emu->stopPractice();
      else practicePanel_ = !practicePanel_;
    }
    if (on) ImGui::PopStyleColor();
  }
  ImGui::SameLine();
  {
    std::string l = std::string(d_.settings->integerScale ? TR("Integer") : TR("FILL")) + "##scale";
    if (ImGui::Button(l.c_str(), ImVec2(0, bs.y))) {
      d_.settings->integerScale = !d_.settings->integerScale;
      changed();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoNavOverride))
      ImGui::SetTooltip("%s", TR("Integer: largest integer scale that fits (sharp) / FILL: fill the window, aspect ratio kept"));
  }
  ImGui::SameLine();
  if (ImGui::Button("···##more", ImVec2(S(56), bs.y))) ImGui::OpenPopup("##more");
  if (ImGui::BeginPopup("##more")) {
    if (ImGui::Selectable(TR("Add Bookmark"), false, st.practicing ? ImGuiSelectableFlags_Disabled : 0)) emu->addBookmark();
    if (ImGui::Selectable(TR("Back to Previous Take"), false, st.undoDepth == 0 || st.practicing ? ImGuiSelectableFlags_Disabled : 0))
      emu->undoTake();
    if (ImGui::Selectable(TR("Re-record from Here"), false, st.practicing ? ImGuiSelectableFlags_Disabled : 0)) emu->rerecordHere();
    ImGui::Separator();
    for (int n : {5, 10, 30, 60})
      if (ImGui::Selectable((TR("Advance by Frames") + std::string(": ") + TRF("%lld frames", {n})).c_str())) emu->frameAdvance(n);
    if (ImGui::Selectable(TR("Back 1 Second"))) emu->jumpSeconds(-1);
    if (ImGui::Selectable(TR("Forward 1 Second (Recorded Range)"))) emu->jumpSeconds(1);
    ImGui::Separator();
    bool resetOK = st.recording || st.practicing;
    if (ImGui::Selectable(TR("Soft Reset"), false, resetOK ? 0 : ImGuiSelectableFlags_Disabled)) emu->requestEvent(RN_EV_SOFT_RESET);
    if (ImGui::Selectable(TR("Power Cycle (Off and On)"), false, resetOK ? 0 : ImGuiSelectableFlags_Disabled))
      emu->requestEvent(RN_EV_POWER_CYCLE);
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ timeline

void UI::buildTimeline(float width, bool inMenu, double now, float navLeft, float navRight) {
  ImGuiIO& io = ImGui::GetIO();
  const EmuStatus& st = d_.emu->status();
  EmulationController* emu = d_.emu;
  const float laneH = S(18), stripH = S(60), inset = S(8);
  const double tw = double(stripH) * RN_VIDEO_WIDTH / RN_VIDEO_HEIGHT;
  const float w = std::max(0.0f, width - 2 * inset);
  const uint64_t len = st.takeLength;
  double step = d_.thumbs->layout(w, tw, len, now);
  double gw = rnf_thumb_extent(len, step, tw);
  ImVec2 origin = ImGui::GetCursorScreenPos();
  float left = origin.x + inset, top = origin.y;
  ImGui::PushID("timeline");
  bool pressed = false;
  {
    // InvisibleButton with a wider navigation rectangle.
    ImGuiID tid = ImGui::GetID("##tl");
    ImRect bb(origin, ImVec2(origin.x + width, origin.y + laneH + stripH));
    ImRect nav(ImVec2(std::min(navLeft, bb.Min.x), bb.Min.y), ImVec2(std::max(navRight, bb.Max.x), bb.Max.y));
    ImGui::ItemSize(bb.GetSize());
    if (ImGui::ItemAdd(bb, tid, &nav)) {
      bool hovered = false, held = false;
      pressed = ImGui::ButtonBehavior(bb, tid, &hovered, &held);
    }
  }
  bool focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto X = [&](uint64_t f) { return left + float(rnf_timeline_x_for_frame(gw, len, f)); };
  auto frameAt = [&](float x) { return rnf_timeline_frame_at_x(gw, len, double(x - left)); };

  std::vector<rnf_timeline_range> ranges = emu->visibleRanges();
  int& sel = d_.settings->timelineSlot;

  // ---- interaction: mouse / touch
  if (ImGui::IsItemActivated() && io.MouseDown[0] && st.hasSession && len > 0) {
    float px = io.MousePos.x - left, py = io.MousePos.y - top;
    dragStartX_ = px;
    bool inLane = py < laneH;
    bool selectMod = io.KeyShift || io.KeyAlt;
    drag_ = st.practicing ? Drag::ignore : Drag::scrub;
    if (inLane || selectMod) {
      rnf_timeline_hit hit = rnf_timeline_hit_test(px, ranges.data(), ranges.size(), gw, len, S(10), 1, sel);
      if (st.practicing) {
        if (hit.kind != RNF_HIT_NONE) {
          drag_ = Drag::body;
          dragSlot_ = hit.slot;
        } else {
          notice(EmulationController::practiceBlockedText());
          drag_ = Drag::ignore;
        }
      } else {
        drag_ = Drag::select;
        dragSlot_ = sel;
        if (!selectMod || inLane) {
          if (hit.kind == RNF_HIT_HANDLE) {
            for (const rnf_timeline_range& r : ranges)
              if (r.slot == hit.slot && r.has_b) {
                sel = hit.slot;
                drag_ = Drag::handle;
                dragSlot_ = hit.slot;
                dragHandle_ = hit.handle;
                dragA_ = r.a;
                dragB_ = r.b;
              }
          } else if (hit.kind == RNF_HIT_BODY && !selectMod) {
            drag_ = Drag::body;
            dragSlot_ = hit.slot;
          }
        }
      }
    }
  }
  if (drag_ != Drag::none && ImGui::IsItemActive() && io.MouseDown[0]) {
    float px = io.MousePos.x - left;
    switch (drag_) {
      case Drag::scrub: {
        uint64_t f = frameAt(io.MousePos.x);
        if (!scrubFrame_ || *scrubFrame_ != f) {
          scrubFrame_ = f;
          emu->scrub(f);
        }
        break;
      }
      case Drag::select: {
        uint64_t a = 0, b = 0;
        hasPreview_ = rnf_timeline_range_from_drag(gw, len, dragStartX_, px, &a, &b) != 0;
        preview_ = rnf_timeline_range{dragSlot_, a, 1, b};
        break;
      }
      case Drag::handle: {
        uint64_t a = 0, b = 0;
        rnf_timeline_drag(rnf_timeline_handle(dragHandle_), dragA_, dragB_, px, gw, len, &a, &b);
        hasPreview_ = true;
        preview_ = rnf_timeline_range{dragSlot_, a, 1, b};
        break;
      }
      case Drag::body:
        if (std::fabs(px - dragStartX_) > S(4) && !st.practicing) {
          drag_ = Drag::select;
          dragSlot_ = sel;
        }
        break;
      default: break;
    }
  }
  if (drag_ != Drag::none && !ImGui::IsItemActive()) {
    float px = io.MousePos.x - left;
    switch (drag_) {
      case Drag::scrub: scrubHoldUntil_ = now + 0.15; break;  // keep the drag position until the seek shows
      case Drag::select:
      case Drag::handle:
        if (hasPreview_ && preview_.has_b && preview_.b > preview_.a) {
          sel = dragSlot_;
          emu->practiceSetRange(dragSlot_, preview_.a, preview_.b);
          previewUntil_ = now + 0.4;
          changed();
        } else {
          hasPreview_ = false;
          if (drag_ == Drag::select && std::fabs(px - dragStartX_) < S(4) && !st.practicing) emu->seek(frameAt(io.MousePos.x));
        }
        break;
      case Drag::body:
        sel = dragSlot_;
        emu->startPractice(dragSlot_);
        practicePanel_ = true;
        changed();
        break;
      default: break;
    }
    drag_ = Drag::none;
  }
  if (scrubFrame_ && drag_ != Drag::scrub && now > scrubHoldUntil_) scrubFrame_.reset();
  if (hasPreview_ && drag_ == Drag::none && now > previewUntil_) hasPreview_ = false;

  // ---- interaction: controller (the focused timeline on the hub): D-pad <- / -> moves the playhead
  // (hold = faster; steps frames past the recorded end while recording), L1 / R1 jump to the
  // previous / next bookmark (or 5 s), X / Y set A / B of the selected section, A plays from here.
  if (inMenu && focused && st.hasSession) {
    timelineFocused_ = true;
    ImGuiContext& g = *GImGui;
    if (g.NavMoveDir == ImGuiDir_Left || g.NavMoveDir == ImGuiDir_Right) ImGui::NavMoveRequestCancel();  // <- / -> are ours
    int dir = 0;
    ImGuiKey key = ImGuiKey_None;
    for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_LeftArrow})
      if (ImGui::IsKeyPressed(k, true)) dir = -1, key = k;
    for (ImGuiKey k : {ImGuiKey_GamepadDpadRight, ImGuiKey_RightArrow})
      if (ImGui::IsKeyPressed(k, true)) dir = 1, key = k;
    if (dir != 0) {
      int n = scrubStepFrames(ImGui::GetKeyData(key)->DownDuration);
      uint64_t f = st.frame;
      if (st.practicing) {
        if (dir < 0) emu->stepBack(uint64_t(n));
        else emu->frameAdvance(n);
      } else if (dir < 0) {
        emu->scrub(f >= uint64_t(n) ? f - uint64_t(n) : 0);
      } else if (f + uint64_t(n) <= len) {
        emu->scrub(f + uint64_t(n));
      } else if (f < len) {
        emu->scrub(len);
      } else if (st.recording) {
        emu->frameAdvance(1);  // at the end of the recording: record one more frame, like paused stepping
      }
    }
    int jump = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? 1 : 0);
    if (jump != 0 && !st.practicing && len > 0) {
      std::vector<uint64_t> marks;
      for (const BookmarkInfo& b : emu->structure().bookmarks)
        if (b.onActiveTake) marks.push_back(b.frame);
      emu->seek(timelineJumpTarget(st.frame, len, marks, jump));
    }
    if (!st.practicing && ImGui::IsKeyPressed(kPadX, false)) emu->timelineMarkA(sel);
    if (!st.practicing && ImGui::IsKeyPressed(kPadY, false)) emu->timelineMarkB(sel);
    if (pressed && !io.MouseReleased[0]) closeHubAndResume();  // A: play from here
    prompt({"dpad.lr"}, TR("Move (hold = faster)"));
    prompt({"leftShoulder", "rightShoulder"}, TR("Previous / Next Bookmark (or 5 s)"));
    prompt({"face.west"}, TR("Set A"));
    prompt({"face.north"}, TR("Set B"));
    prompt({"face.south"}, TR("Play from Here"));
    prompt({"face.east"}, TR("Resume"));
  }

  // ---- drawing
  ImVec2 s0(left, top + laneH), s1(left + w, top + laneH + stripH);
  dl->AddRectFilled(s0, s1, IM_COL32(255, 255, 255, 18), S(4));
  if (gw > 0) dl->AddRectFilled(s0, ImVec2(left + float(std::min<double>(gw, w)), s1.y), IM_COL32(255, 255, 255, 26), S(4));
  dl->PushClipRect(s0, s1, true);
  auto drawTiles = [&](double stp, float alpha) {
    size_t n = rnf_thumb_tiles(len, stp, tw, nullptr, 0);
    std::vector<rnf_thumb_tile> tiles(n);
    rnf_thumb_tiles(len, stp, tw, tiles.data(), n);
    uint64_t window = rnf_thumb_fallback_window(stp);
    for (const rnf_thumb_tile& t : tiles) {
      float x = left + float(t.x);
      if (x > s1.x) break;
      ThumbRef img = d_.thumbs->imageAt(t.picture);
      bool own = bool(img);
      if (!img) img = d_.thumbs->imageBefore(t.picture, window);
      if (img) {
        uint64_t tex = 0;
        float uv[4];
        if (d_.renderer->thumbTexture(img.get()->serial, img.get()->px, &tex, uv)) {
          float vis = float(t.visible);
          float u1 = uv[0] + (uv[2] - uv[0]) * float(vis / tw);
          float a = alpha * (own ? 1.0f : float(RNF_THUMB_FALLBACK_OPACITY));
          dl->AddImage(ImTextureRef(ImTextureID(tex)), ImVec2(x, s0.y), ImVec2(x + vis, s1.y), ImVec2(uv[0], uv[1]),
                       ImVec2(u1, uv[3]), IM_COL32(255, 255, 255, int(255 * a)));
        }
      }
      if (t.index > 0) dl->AddLine(ImVec2(x, s0.y), ImVec2(x, s1.y), IM_COL32(0, 0, 0, int(90 * alpha)));
    }
  };
  if (len > 0) {
    drawTiles(step, 1.0f);
    double from = d_.thumbs->fadeFrom(now);
    if (from != 0 && from != step && (from == step * 2 || from * 2 == step)) drawTiles(from, 1.0f - float(d_.thumbs->fadeProgress(now)));
  }
  // A/B ranges over the filmstrip (tint) + lane bars.
  std::vector<rnf_timeline_range> shown = ranges;
  if (hasPreview_) {
    shown.erase(std::remove_if(shown.begin(), shown.end(), [&](const rnf_timeline_range& r) { return r.slot == preview_.slot; }),
                shown.end());
    shown.push_back(preview_);
  }
  std::stable_sort(shown.begin(), shown.end(), [&](const rnf_timeline_range& a, const rnf_timeline_range& b) {
    return std::make_pair(a.slot == sel ? 1 : 0, a.slot) < std::make_pair(b.slot == sel ? 1 : 0, b.slot);
  });
  for (const rnf_timeline_range& r : shown) {
    bool hl = r.slot == sel || (hasPreview_ && preview_.slot == r.slot);
    float xa = X(r.a);
    if (r.has_b) {
      float xb = std::max(X(r.b), xa + 2);
      dl->AddRectFilled(ImVec2(xa, s0.y), ImVec2(xb, s1.y), slotColor(r.slot, hl ? 0.28f : 0.16f));
    }
  }
  dl->PopClipRect();
  for (const rnf_timeline_range& r : shown) {
    bool hl = r.slot == sel || (hasPreview_ && preview_.slot == r.slot);
    float xa = X(r.a);
    if (r.has_b) {
      float xb = std::max(X(r.b), xa + 2);
      ImVec2 b0(xa, top + S(2)), b1(xb, top + laneH - S(2));
      dl->AddRectFilled(b0, b1, slotColor(r.slot, hl ? 0.95f : 0.6f), S(3));
      if (hl) dl->AddRect(b0, b1, IM_COL32(255, 255, 255, 230), S(3));
      for (float x : {xa, xb}) dl->AddRectFilled(ImVec2(x - 1, top), ImVec2(x + 1, s1.y), slotColor(r.slot, hl ? 1.0f : 0.7f));
      if (xb - xa >= S(14)) {
        std::string n = std::to_string(r.slot + 1);
        dl->AddText(nullptr, S(13), ImVec2(xa + S(3), top + S(2)), IM_COL32(0, 0, 0, 255), n.c_str());
      }
    } else {
      dl->AddRectFilled(ImVec2(xa - 1, top), ImVec2(xa + 1, s1.y), slotColor(r.slot, hl ? 1.0f : 0.7f));
      float fx = xa + S(26) > left + w + inset ? xa - S(26) : xa;
      dl->AddRectFilled(ImVec2(fx, top + S(2)), ImVec2(fx + S(26), top + laneH - S(2)), slotColor(r.slot, hl ? 0.95f : 0.6f), S(3));
      std::string n = std::to_string(r.slot + 1) + "A";
      dl->AddText(nullptr, S(12), ImVec2(fx + S(3), top + S(2)), IM_COL32(0, 0, 0, 255), n.c_str());
    }
  }
  // Bookmarks (yellow).
  for (const BookmarkInfo& b : emu->structure().bookmarks)
    if (b.onActiveTake) dl->AddRectFilled(ImVec2(X(b.frame) - 1, s0.y), ImVec2(X(b.frame) + 1, s0.y + S(12)), IM_COL32(255, 214, 10, 255));
  // Playhead: the take cursor (red recording, white playing), orange inside the practiced range.
  bool hasHead = false;
  float hx = 0;
  ImU32 hc = IM_COL32(255, 255, 255, 255);
  if (st.practicing) {
    for (const rnf_timeline_range& r : ranges)
      if (r.slot == st.practiceSlot) {
        uint64_t end = r.has_b ? r.b : r.a;
        hx = X(r.a + std::min(st.practiceFrame, end - r.a));
        hasHead = true;
        hc = IM_COL32(255, 149, 0, 255);
      }
    if (!hasHead && st.practiceLength > 0) {
      float p = std::min(1.0f, float(st.practiceFrame) / float(st.practiceLength));
      dl->AddRectFilled(ImVec2(left, s1.y - S(4)), ImVec2(left + w * p, s1.y), IM_COL32(255, 149, 0, 230));
    }
  } else if (len > 0) {
    hx = X(scrubFrame_ ? *scrubFrame_ : st.frame);
    hasHead = true;
    hc = st.recording ? IM_COL32(255, 59, 48, 255) : IM_COL32(255, 255, 255, 255);
  }
  if (hasHead) {
    // Whole pixels: while recording the playhead advances in clean 1-pixel steps.
    float x = float(rnf_thumb_snap_to_pixel(std::clamp(hx, left, left + float(std::min<double>(gw, w))), 1.0));
    dl->AddRectFilled(ImVec2(x, top + laneH - S(2)), ImVec2(x + 1, s1.y), hc);
    dl->AddTriangleFilled(ImVec2(x - S(6), top + laneH - S(9)), ImVec2(x + S(7), top + laneH - S(9)),
                          ImVec2(x + 0.5f, top + laneH - S(1)), hc);
  }
  dl->AddRect(s0, s1, IM_COL32(255, 255, 255, focused && inMenu ? 200 : 60), S(4), 0, 1.0f);
  if (focused && inMenu && !st.practicing) {
    // Which section X / Y edit (its color), next to the start time.
    std::string l = TRF("A/B %lld", {sel + 1});
    ImVec2 p(origin.x - S(4) - ImGui::GetFont()->CalcTextSizeA(S(15), FLT_MAX, 0, l.c_str()).x - S(10), top + S(1));
    dl->AddRectFilled(p, ImVec2(origin.x - S(4), top + laneH - S(1)), slotColor(sel, 0.9f), S(4));
    dl->AddText(nullptr, S(15), ImVec2(p.x + S(5), top + S(1)), IM_COL32(0, 0, 0, 255), l.c_str());
  }
}

// ------------------------------------------------------------------ practice OSD

int UI::buildPracticeRows(bool big) {
  const EmuStatus& st = d_.emu->status();
  const SessionStructure& ss = d_.emu->structure();
  EmulationController* emu = d_.emu;
  float rowH = big ? S(44) : S(32);
  int focused = -1;
  for (const SlotInfo& s : ss.slots) {
    auto noteFocus = [&] {
      if (ImGui::IsItemFocused()) focused = s.index;
    };
    ImGui::PushID(s.index);
    bool active = st.practicing && st.practiceSlot == s.index;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    if (active) ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width, p.y + rowH), IM_COL32(255, 149, 0, 56), S(5));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(slotColor(s.index)), "%d", s.index + 1);
    ImGui::SameLine(S(34));
    std::string name = s.hasA ? s.displayName() : std::string(TR("(not set)"));
    float nameW = std::max(S(80), width - (big ? S(560) : S(300)));
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + nameW, ImGui::GetCursorScreenPos().y + rowH), true);
    if (s.hasA) ImGui::TextUnformatted(name.c_str());
    else ImGui::TextDisabled("%s", name.c_str());
    ImGui::PopClipRect();
    ImGui::SameLine(S(34) + nameW + S(8));
    ImGui::TextDisabled("%s", s.hasA && s.hasB ? timecode(s.length).c_str() : "--:--.--");
    ImGui::SameLine();
    auto marker = [&](const char* l, bool set) {
      ImGui::PushStyleColor(ImGuiCol_Button, set ? ImVec4(1.0f, 0.58f, 0.0f, 1) : ImVec4(0.25f, 0.26f, 0.3f, 1));
      ImGui::PushStyleColor(ImGuiCol_Text, set ? ImVec4(0, 0, 0, 1) : ImVec4(1, 1, 1, 0.85f));
      bool r = ImGui::Button(l, ImVec2(rowH, rowH));
      ImGui::PopStyleColor(2);
      return r;
    };
    if (marker("A", s.hasA)) emu->practiceSetA(s.index);
    noteFocus();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoNavOverride)) ImGui::SetTooltip("%s", TR("Set A (make the current position the start of the section)"));
    ImGui::SameLine();
    if (marker("B", s.hasB)) emu->practiceSetB(s.index);
    noteFocus();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoNavOverride))
      ImGui::SetTooltip("%s", TR("Set B (make the current position, reached by playing on from A, the end)"));
    ImGui::SameLine();
    if (ui::IconButton("##go", active ? ui::Icon::repeat : ui::Icon::play, ImVec2(rowH * 1.3f, rowH), s.hasA)) {
      emu->startPractice(s.index);
      if (menuOpen_) setMenu(false);
    }
    noteFocus();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoNavOverride))
      ImGui::SetTooltip("%s", active ? TR("Restart from A") : TR("Practice This Section"));
    if (big) {
      ImGui::SameLine();
      ImGui::BeginDisabled(!s.hasA);
      if (ImGui::Button(TR("Rename…"), ImVec2(0, rowH))) {
        int idx = s.index;
        askRename(TR("Section Name"), s.name, [emu, idx](const std::string& n) { emu->practiceRename(idx, n); });
      }
      noteFocus();
      ImGui::SameLine();
      if (ImGui::Button(TR("Clear"), ImVec2(0, rowH))) emu->practiceClear(s.index);
      noteFocus();
      ImGui::EndDisabled();
    }
    ImGui::PopID();
  }
  return focused;
}

void UI::buildPracticeOverlay(bool) {
  const EmuStatus& st = d_.emu->status();
  ImGuiIO& io = ImGui::GetIO();
  bool dockShown = menuOpen_ || st.paused || drag_ != Drag::none;
  float bottom = io.DisplaySize.y - S(12) - (menuOpen_ ? hubPanelHeight() : dockShown ? dockHeight(false) : 0.0f);
  const SessionStructure& ss = d_.emu->structure();
  std::string current = TR("Practicing");
  if (st.practiceSlot >= 0 && st.practiceSlot < int(ss.slots.size()))
    current = std::to_string(st.practiceSlot + 1) + ". " + ss.slots[size_t(st.practiceSlot)].displayName();
  std::string progress = timecode(std::min(st.practiceFrame, st.practiceLength > 0 ? st.practiceLength : st.practiceFrame)) +
                         " / " + (st.practiceLength > 0 ? timecode(st.practiceLength) : std::string("--:--.--"));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
  if (!menuOpen_) flags |= ImGuiWindowFlags_NoNav;
  ImGui::SetNextWindowPos(ImVec2(S(14), bottom), ImGuiCond_Always, ImVec2(0, 1));
  ImGui::SetNextWindowBgAlpha(0.72f);
  if (st.practicing && !st.paused) {
    // Compact pill while a practice run plays (drawn in the same pass as the game).
    ImGui::Begin("##practicepill", nullptr, flags | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(ImVec4(1, 0.58f, 0, 1), "A/B");
    ImGui::SameLine();
    ImGui::TextUnformatted(current.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", progress.c_str());
    if (st.practiceLoops > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", TRF("Loop %lld", {st.practiceLoops + 1}).c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Stop Practicing"))) d_.emu->stopPractice();
    ImGui::End();
    return;
  }
  if (menuOpen_) return;  // the menu has the Practice tab
  ImGui::SetNextWindowSizeConstraints(ImVec2(S(460), 0), ImVec2(S(560), io.DisplaySize.y * 0.8f));
  ImGui::Begin("##practicepanel", nullptr, flags);
  ImGui::TextColored(ImVec4(1, 0.58f, 0, 1), "A/B");
  ImGui::SameLine();
  ImGui::TextUnformatted(st.practicing ? TRF("Practicing: %@", {current}).c_str() : TR("Practice (A/B Repeat)"));
  if (st.practicing) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", progress.c_str());
  }
  ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(140));
  if (st.practicing) {
    if (ImGui::SmallButton(TR("Stop Practicing"))) d_.emu->stopPractice();
  } else if (ImGui::SmallButton(TR("Close"))) {
    practicePanel_ = false;
  }
  ImGui::PushTextWrapPos(S(540));
  ImGui::TextDisabled("%s", st.practicing ? TR("At B it pauses briefly, returns to A and repeats. Nothing is recorded.")
                                          : TR("A = start of the section, B = the end you reach by playing on from A. Practice a "
                                               "section as often as you like (nothing is recorded)."));
  ImGui::PopTextWrapPos();
  ImGui::PushFont(nullptr, S(18));
  buildPracticeRows(false);
  ImGui::PopFont();
  ImGui::End();
}

// ------------------------------------------------------------------ badges

void UI::buildBadges() {
  const EmuStatus& st = d_.emu->status();
  bool show = st.paused || st.rewinding || st.fastForward || st.slow != 1 || st.practicing || st.endOfTake ||
              (st.flashActive && d_.settings->showFlashIndicator) || menuOpen_;
  if (!show) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float fs = S(16);
  float y = menuOpen_ ? S(68) : S(12);
  ImVec2 p(S(12), y);
  auto pill = [&](const char* t, ImU32 c) { p.x += ui::Pill(dl, p, t, c, fs) + S(6); };
  if (st.practicing) pill(TR("Practicing (not recording)"), IM_COL32(255, 149, 0, 200));
  else pill(st.recording ? TR("● REC") : TR("▶︎ PLAY"), st.recording ? IM_COL32(220, 30, 30, 200) : IM_COL32(40, 160, 70, 200));
  if (st.rewinding) pill(TR("◀◀ Rewinding"), IM_COL32(255, 149, 0, 200));
  else if (st.paused) pill(TR("❚❚ Paused"), IM_COL32(110, 110, 120, 200));
  else if (st.fastForward) pill(TR("▶▶ Fast-Forward"), IM_COL32(10, 110, 230, 200));
  else if (st.slow != 1) pill(TRF("Slow %@", {std::string("1/2")}).c_str(), IM_COL32(150, 70, 200, 200));
  if (st.endOfTake && !st.practicing) pill(TR("End of Take"), IM_COL32(200, 170, 0, 210));
  if (st.flashActive && d_.settings->showFlashIndicator) pill(TR("Flash Reduction Active"), IM_COL32(30, 150, 150, 210));
}

}  // namespace rnl
