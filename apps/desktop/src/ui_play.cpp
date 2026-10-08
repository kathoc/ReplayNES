// While a game is open and the menu is closed: the seek bar when paused (filmstrip + time + the
// A/B lane; A / B resume, L2 / R2 rewind / fast-forward, the D-pad steps - all through the
// InputRouter; mouse / touch scrub, select and drag sections on it), the practice pill and the
// status badges. All in the game's render pass; nothing at all while playing but the badges.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <vector>

#include "app_model.h"
#include "emulation.h"
#include "icons.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

namespace rnl {

using namespace theme;

namespace {
const ImU32 kSlotColors[8] = {IM_COL32(255, 149, 0, 255), IM_COL32(10, 132, 255, 255), IM_COL32(48, 209, 88, 255),
                              IM_COL32(255, 55, 95, 255),  IM_COL32(191, 90, 242, 255), IM_COL32(64, 200, 224, 255),
                              IM_COL32(255, 214, 10, 255), IM_COL32(255, 69, 58, 255)};
ImU32 slotColor(int slot, float a = 1.0f) { return alpha(kSlotColors[((slot % 8) + 8) % 8], a); }
}  // namespace

// ------------------------------------------------------------------ seek bar

void UI::buildSeekBar(double now) {
  ImGuiIO& io = ImGui::GetIO();
  const UiMetrics& m = metrics_;
  SeekGeometry g = layoutSeek(m);
  const EmuStatus& st = d_.emu->status();
  ImGui::SetNextWindowPos(tl(g.panel));
  ImGui::SetNextWindowSize(ImVec2(g.panel.w, g.panel.h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##seekbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(tl(g.panel), br(g.panel), kPanel, m.panelRadius());
  layoutRecord(g.panel);
  // Times: where we are (with the record / playback state) | the take's length.
  std::string left = timecode(st.practicing ? st.practiceFrame : st.frame);
  std::string right = st.practicing ? (st.practiceLength > 0 ? timecode(st.practiceLength) : std::string("--:--.--"))
                                    : timecode(st.takeLength) + (st.unsaved ? " \xE2\x80\xA2" : "");
  float cy = g.strip.y + g.strip.h / 2;
  ImU32 stateCol = st.practicing ? IM_COL32(255, 149, 0, 255) : st.recording ? kBrand : IM_COL32(48, 209, 88, 255);
  const char* stateIcon = st.recording && !st.practicing ? "player-record-filled" : "player-play-filled";
  float is = m.label();
  dl->AddText(ImGui::GetFont(), is, ImVec2(g.timeLeft.x, cy - is * 1.1f), stateCol, icons::glyph(stateIcon));
  const char* stateText = st.practicing ? TR("Practicing") : st.recording ? TR("REC") : TR("PLAY");
  dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(g.timeLeft.x + is * 1.3f, cy - is * 1.0f), stateCol, stateText);
  dl->AddText(ImGui::GetFont(), m.label(), ImVec2(g.timeLeft.x, cy + S(2)), kText, left.c_str());
  ImVec2 rs = measure(m.label(), right.c_str());
  dl->AddText(ImGui::GetFont(), m.label(), ImVec2(g.timeRight.right() - rs.x, cy - rs.y / 2), kTextDim, right.c_str());
  buildTimeline(g.strip, g.lane, now);
  // Hints inside the panel: A resume, L2 / R2 rewind / fast-forward, D-pad step.
  rnf_controller_family f = promptFamily();
  float gh = m.hint() * 1.45f, x = g.hints.x, hy = g.hints.y + (g.hints.h - gh) / 2;
  auto hint = [&](std::initializer_list<const char*> els, const char* text) {
    for (const char* e : els) x += glyph(dl, ImVec2(x, hy), e, f, gh) + S(4);
    x += S(4);
    dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(x, hy + (gh - m.hint()) / 2), kTextDim, text);
    x += measure(m.hint(), text).x + S(22);
  };
  if (controllerConnected()) {
    hint({"face.south"}, TR("Resume"));
    std::string l = triggerAction(true), r = triggerAction(false);
    if (l == "hk.rewind" && r == "hk.fast_forward") hint({"leftTrigger", "rightTrigger"}, TR("Rewind / Fast-forward"));
    if (d_.settings->dpadStepWhenPaused) hint({"dpad.lr"}, TR("Step"));
  } else {
    float kh = gh;
    x += keycap(dl, ImVec2(x, hy), kh, "Space", false) + S(8);
    dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(x, hy + (gh - m.hint()) / 2), kTextDim, TR("Resume"));
  }
  ImGui::End();
  (void)io;
}

void UI::buildTimeline(const LRect& strip, const LRect& lane, double now) {
  ImGuiIO& io = ImGui::GetIO();
  const EmuStatus& st = d_.emu->status();
  EmulationController* emu = d_.emu;
  const float laneH = lane.h, stripH = strip.h;
  const double tw = double(stripH) * RN_VIDEO_WIDTH / RN_VIDEO_HEIGHT;
  const float w = strip.w;
  const uint64_t len = st.takeLength;
  double step = d_.thumbs->layout(w, tw, len, now);
  double gw = rnf_thumb_extent(len, step, tw);
  float left = strip.x, top = lane.y;
  ImGui::SetCursorScreenPos(ImVec2(lane.x, lane.y));
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  ImGui::InvisibleButton("##timeline", ImVec2(strip.w, laneH + stripH));
  ImGui::PopItemFlag();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto X = [&](uint64_t f) { return left + float(rnf_timeline_x_for_frame(gw, len, f)); };
  auto frameAt = [&](float x) { return rnf_timeline_frame_at_x(gw, len, double(x - left)); };
  std::vector<rnf_timeline_range> ranges = emu->visibleRanges();
  int& sel = d_.settings->timelineSlot;

  // ---- mouse / touch: scrub on the filmstrip, select / move sections on the lane (Shift / Alt: anywhere)
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
      case Drag::scrub: scrubHoldUntil_ = now + 0.15; break;
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
        changed();
        break;
      default: break;
    }
    drag_ = Drag::none;
  }
  if (scrubFrame_ && drag_ != Drag::scrub && now > scrubHoldUntil_) scrubFrame_.reset();
  if (hasPreview_ && drag_ == Drag::none && now > previewUntil_) hasPreview_ = false;

  // ---- drawing
  float radius = S(6);
  ImVec2 s0(left, top + laneH), s1(left + w, top + laneH + stripH);
  dl->AddRectFilled(s0, s1, IM_COL32(255, 255, 255, 14), radius);
  dl->PushClipRect(s0, s1, true);
  auto drawTiles = [&](double stp, float a) {
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
          float al = a * (own ? 1.0f : float(RNF_THUMB_FALLBACK_OPACITY));
          dl->AddImage(ImTextureRef(ImTextureID(tex)), ImVec2(x, s0.y), ImVec2(x + vis, s1.y), ImVec2(uv[0], uv[1]), ImVec2(u1, uv[3]),
                       IM_COL32(255, 255, 255, int(255 * al)));
        }
      }
      if (t.index > 0) dl->AddLine(ImVec2(x, s0.y), ImVec2(x, s1.y), IM_COL32(0, 0, 0, int(90 * a)));
    }
  };
  if (len > 0) {
    drawTiles(step, 1.0f);
    double from = d_.thumbs->fadeFrom(now);
    if (from != 0 && from != step && (from == step * 2 || from * 2 == step)) drawTiles(from, 1.0f - float(d_.thumbs->fadeProgress(now)));
  }
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
    if (r.has_b) dl->AddRectFilled(ImVec2(X(r.a), s0.y), ImVec2(std::max(X(r.b), X(r.a) + 2), s1.y), slotColor(r.slot, hl ? 0.28f : 0.16f));
  }
  dl->PopClipRect();
  // The A/B lane above the filmstrip.
  for (const rnf_timeline_range& r : shown) {
    bool hl = r.slot == sel || (hasPreview_ && preview_.slot == r.slot);
    float xa = X(r.a);
    if (r.has_b) {
      float xb = std::max(X(r.b), xa + 2);
      ImVec2 b0(xa, top + S(2)), b1(xb, top + laneH - S(2));
      dl->AddRectFilled(b0, b1, slotColor(r.slot, hl ? 0.95f : 0.6f), S(3));
      for (float x : {xa, xb}) dl->AddRectFilled(ImVec2(x - 1, top), ImVec2(x + 1, s1.y), slotColor(r.slot, hl ? 1.0f : 0.7f));
      if (xb - xa >= S(14)) {
        std::string n = std::to_string(r.slot + 1);
        dl->AddText(ImGui::GetFont(), laneH * 0.8f, ImVec2(xa + S(3), top + laneH * 0.08f), IM_COL32(0, 0, 0, 255), n.c_str());
      }
    } else {
      dl->AddRectFilled(ImVec2(xa - 1, top), ImVec2(xa + 1, s1.y), slotColor(r.slot, hl ? 1.0f : 0.7f));
      float fx = xa + S(26) > left + w ? xa - S(26) : xa;
      dl->AddRectFilled(ImVec2(fx, top + S(2)), ImVec2(fx + S(26), top + laneH - S(2)), slotColor(r.slot, hl ? 0.95f : 0.6f), S(3));
      std::string n = std::to_string(r.slot + 1) + "A";
      dl->AddText(ImGui::GetFont(), laneH * 0.75f, ImVec2(fx + S(3), top + laneH * 0.1f), IM_COL32(0, 0, 0, 255), n.c_str());
    }
  }
  for (const BookmarkInfo& b : emu->structure().bookmarks)
    if (b.onActiveTake) dl->AddRectFilled(ImVec2(X(b.frame) - 1, s0.y), ImVec2(X(b.frame) + 1, s0.y + S(12)), IM_COL32(255, 214, 10, 255));
  // The playhead: red recording, white playing, orange inside the practiced section.
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
  } else if (len > 0) {
    hx = X(scrubFrame_ ? *scrubFrame_ : st.frame);
    hasHead = true;
    hc = st.recording ? kBrand : IM_COL32(255, 255, 255, 255);
  }
  if (hasHead) {
    float x = float(rnf_thumb_snap_to_pixel(std::clamp(hx, left, left + float(std::min<double>(gw, w))), 1.0));
    dl->AddRectFilled(ImVec2(x - 1, top + laneH - S(2)), ImVec2(x + 1, s1.y), hc);
    dl->AddTriangleFilled(ImVec2(x - S(7), top + laneH - S(9)), ImVec2(x + S(8), top + laneH - S(9)), ImVec2(x + 0.5f, top + laneH - S(1)), hc);
  }
  dl->AddRect(s0, s1, IM_COL32(255, 255, 255, 40), radius, 0, 1.0f);
}

// ------------------------------------------------------------------ practice pill

void UI::buildPracticePill() {
  const EmuStatus& st = d_.emu->status();
  const UiMetrics& m = metrics_;
  const SessionStructure& ss = d_.emu->structure();
  std::string current = TR("Practicing");
  if (st.practiceSlot >= 0 && st.practiceSlot < int(ss.slots.size()))
    current = std::to_string(st.practiceSlot + 1) + ". " + ss.slots[size_t(st.practiceSlot)].displayName();
  std::string progress = timecode(std::min(st.practiceFrame, st.practiceLength > 0 ? st.practiceLength : st.practiceFrame)) + " / " +
                         (st.practiceLength > 0 ? timecode(st.practiceLength) : std::string("--:--.--"));
  if (st.practiceLoops > 0) progress += "  " + TRF("Loop %lld", {st.practiceLoops + 1});
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float fs = m.hint() * 1.1f;
  std::string text = std::string(icons::glyph("repeat")) + "  " + current + "   " + progress;
  ImVec2 ts = measure(fs, text.c_str());
  ImVec2 p(m.margin(), metrics_.H - m.margin() - ts.y - S(16));
  dl->AddRectFilled(p, ImVec2(p.x + ts.x + S(28), p.y + ts.y + S(16)), IM_COL32(20, 20, 22, 200), (ts.y + S(16)) / 2);
  dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + S(14), p.y + S(8)), IM_COL32(255, 170, 60, 255), text.c_str());
}

// ------------------------------------------------------------------ badges

void UI::buildBadges() {
  const EmuStatus& st = d_.emu->status();
  bool show = st.rewinding || st.fastForward || st.slow != 1 || st.endOfTake || (st.flashActive && d_.settings->showFlashIndicator) ||
              (st.paused && st.practicing);
  if (!show) return;
  const UiMetrics& m = metrics_;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float fs = m.hint();
  ImVec2 p(m.margin(), pill_.y + (pill_.h - fs - S(8)) / 2);
  auto pill = [&](const std::string& t, ImU32 c) { p.x += ui::Pill(dl, p, t.c_str(), c, fs) + S(6); };
  if (st.rewinding) pill(std::string(icons::glyph("player-track-prev")) + " " + TR("Rewinding"), IM_COL32(255, 149, 0, 210));
  else if (st.fastForward) pill(std::string(icons::glyph("player-track-next")) + " " + TR("Fast-Forward"), IM_COL32(10, 110, 230, 210));
  else if (st.slow != 1) pill(TRF("Slow %@", {std::string("1/2")}), IM_COL32(150, 70, 200, 210));
  if (st.endOfTake && !st.practicing) pill(TR("End of Take"), IM_COL32(200, 160, 0, 220));
  if (st.flashActive && d_.settings->showFlashIndicator) pill(TR("Flash Reduction Active"), IM_COL32(30, 150, 150, 220));
}

}  // namespace rnl
