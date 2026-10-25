// While a game is open and the menu is closed: the seek bar when paused (filmstrip + time + the
// A/B lane; the cancel button plays, L / R and the D-pad step, L2 / R2 rewind / fast-forward -
// all through the InputRouter; mouse / touch scrub, select and drag sections on it; the A/B
// markers of the selected slot from the controller: rnf_markers, docs/design/UI_REDESIGN.md) and
// read-only while rewind / fast-forward are held in play; the practice pill, the status label, the
// Undo / Make Video chips and the first-run coach toasts (docs/design/VTR_UX.md). All in the game's
// render pass; while playing only the status label (non-default states) and the Undo chip.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
std::string takeStr(char* p) {
  std::string s = p ? p : "";
  rnf_string_free(p);
  return s;
}
}  // namespace

// ------------------------------------------------------------------ markers (controller A / B)

bool UI::markersActive() const {
  if (!hasSession() || menuOpen_ || interactive()) return false;
  const EmuStatus& st = d_.emu->status();
  return st.paused && !st.practicing;
}

int UI::seekFocus() const {
  if (!markersActive()) return 0;
  return rnf_markers_editing(markers_) ? 2 : rnf_markers_focus(markers_) >= 0 ? 1 : 0;
}

void UI::applyMarkers(const rnf_markers_result& r) {
  EmulationController* emu = d_.emu;
  int slot = std::clamp(d_.settings->timelineSlot, 0, 7);
  if (r.seek) emu->scrub(r.seek_frame);
  switch (r.write) {
    case RNF_MARKERS_WRITE_A_ONLY: emu->practiceSetAAt(slot, r.a); break;
    case RNF_MARKERS_WRITE_RANGE: emu->practiceSetRange(slot, r.a, r.b, true); break;
    case RNF_MARKERS_WRITE_CLEAR: emu->practiceClear(slot); break;
    case RNF_MARKERS_WRITE_NONE: break;
  }
  double now = markerNow_;
  if (r.outcome == RNF_MARKERS_FULL) {
    markerHint_ = TR("Two markers already (X deletes one)");
    markerHintUntil_ = now + 2.5;
  } else if (r.outcome == RNF_MARKERS_OCCUPIED) {
    markerHint_ = TR("A marker is already here");
    markerHintUntil_ = now + 2.5;
  }
}

void UI::updateMarkers(double now) {
  markerNow_ = now;
  if (!markersActive()) {
    // Resumed, the menu, practice: an edit is dropped (the picture goes back while still paused).
    rnf_markers_result r = rnf_markers_leave(markers_);
    const EmuStatus& st = d_.emu->status();
    if (r.seek && hasSession() && st.paused && !st.practicing) d_.emu->scrub(r.seek_frame);
    laneHints_ = false;
    markerHoldDir_ = 0;
    rnf_step_repeater_reset(markerRepeat_);
    return;
  }
  int sel = std::clamp(d_.settings->timelineSlot, 0, 7);
  rnf_timeline_range range{};
  bool has = false;
  for (const rnf_timeline_range& r : d_.emu->visibleRanges())
    if (r.slot == sel) {
      range = r;
      has = true;
    }
  rnf_markers_sync(markers_, has ? &range : nullptr, d_.emu->status().takeLength);
  if (!rnf_markers_editing(markers_)) {
    markerHoldDir_ = 0;
    rnf_step_repeater_reset(markerRepeat_);
    return;
  }
  // Held D-pad (repeat) and L / R (the rewind / fast-forward curve) move the edited marker.
  if (int d = rnf_step_repeater_tick(markerRepeat_)) applyMarkers(rnf_markers_nudge(markers_, d));
  if (markerHoldDir_ != 0) {
    markerHoldTicks_ += 1;
    applyMarkers(rnf_markers_nudge(markers_, int64_t(markerHoldDir_) * rnf_hold_speed(markerHoldTicks_)));
  }
}

// The selected section's markers (its A, and B once set), left to right.
std::vector<uint64_t> UI::sectionMarkers(int slot) const {
  for (const rnf_timeline_range& r : d_.emu->visibleRanges())
    if (r.slot == slot) return r.has_b ? std::vector<uint64_t>{r.a, r.b} : std::vector<uint64_t>{r.a};
  return {};
}

void UI::markerNotice(const std::string& text) {
  markerHint_ = text;
  markerHintUntil_ = markerNow_ + 2.5;
}

// The paused seek bar (docs/design/VTR_UX.md, "Practice markers"): north places a marker at the
// cursor or deletes the one under it, west practises between the two markers, and while practising
// (paused) stops it.
void UI::seekInput(int input, bool down) {
  using SI = InputRouter::SeekInput;
  if (!down || !hasSession() || menuOpen_ || interactive()) return;
  const EmuStatus& st = d_.emu->status();
  if (!st.paused) return;
  if (st.practicing) {
    if (SI(input) == SI::x) d_.emu->stopPractice();
    return;
  }
  const int slot = std::clamp(d_.settings->timelineSlot, 0, 7);
  const std::vector<uint64_t> markers = sectionMarkers(slot);
  if (SI(input) == SI::y) {
    const MarkerEdit e = markerNorth(markers, st.frame);
    switch (e.kind) {
      case MarkerEdit::setA:
        if (!markers.empty()) d_.emu->practiceClear(slot);  // the other marker stays, as the only one
        d_.emu->practiceSetAAt(slot, e.a);
        break;
      case MarkerEdit::setRange: d_.emu->practiceSetRange(slot, e.a, e.b, true); break;
      case MarkerEdit::clear: d_.emu->practiceClear(slot); break;
      case MarkerEdit::full: markerNotice(TR("Two markers already: put the cursor on one to delete it")); break;
      default: break;
    }
  } else if (SI(input) == SI::x) {
    if (markerWest(markers).kind == MarkerEdit::practice) d_.emu->startPractice(slot);  // from A, after the countdown
    else markerNotice(TR("Place two markers to practice"));
  }
}

void UI::markerHold(int dir, bool down) {
  if (!down || !markersActive() || !rnf_markers_editing(markers_)) {
    if (!down || dir == markerHoldDir_) markerHoldDir_ = 0;
    return;
  }
  markerHoldDir_ = dir;
  markerHoldTicks_ = 1;
  applyMarkers(rnf_markers_nudge(markers_, int64_t(dir) * rnf_hold_speed(1)));
}

// L / R on the paused seek bar: the previous / next practice section (no frame steps there); while
// practising, the practice moves to that section when it has two markers.
void UI::pausedShoulder(int dir) {
  if (!hasSession() || menuOpen_ || interactive() || !d_.emu->paused()) return;
  int& slot = d_.settings->timelineSlot;
  slot = (std::clamp(slot, 0, 7) + dir + 8) % 8;
  changed();
  const bool complete = sectionMarkers(slot).size() >= 2;
  if (d_.emu->status().practicing && complete) d_.emu->startPractice(slot);
  markerNotice(TRF("Section %lld", {slot + 1}));
}

void UI::pauseGesture(int dir, bool repeat) {
  if (!hasSession() || menuOpen_ || interactive()) return;
  if (dir == 0) d_.emu->setPaused(true);
  else if (!d_.emu->paused()) d_.emu->followGesture(dir, repeat);  // no L / R frame steps on the seek bar
}

int UI::selectedSection() const { return std::clamp(d_.settings->timelineSlot, 0, 7) + 1; }

// L + START while practising: back to the first marker at once (a new run from A).
void UI::practiceToA() {
  if (!hasSession() || menuOpen_ || interactive()) return;
  const EmuStatus& st = d_.emu->status();
  if (st.practicing) d_.emu->startPractice(st.practiceSlot);
}

// ------------------------------------------------------------------ seek bar

bool UI::seekBarShown() const {
  if (!hasSession() || menuOpen_) return false;
  const EmuStatus& st = d_.emu->status();
  return !st.live && (st.paused || st.rewinding || st.fastForward);
}

void UI::buildSeekBar(double now) {
  ImGuiIO& io = ImGui::GetIO();
  const UiMetrics& m = metrics_;
  SeekGeometry g = layoutSeek(m);
  const EmuStatus& st = d_.emu->status();
  // Rewind / fast-forward held during play: the bar is read-only (the status label, no hints).
  const bool interact = st.paused;
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
  // Times: where we are | the tape's length (no record / playback badge: the status label says it).
  std::string left = timecode(st.practicing ? st.practiceFrame : st.frame, d_.emu->framePeriod());
  std::string right = st.practicing ? (st.practiceLength > 0 ? timecode(st.practiceLength, d_.emu->framePeriod()) : std::string("--:--.--"))
                                    : timecode(st.takeLength, d_.emu->framePeriod()) + (st.unsaved ? " \xE2\x80\xA2" : "");
  float cy = g.strip.y + g.strip.h / 2;
  ImVec2 ls = measure(m.label(), left.c_str());
  dl->AddText(ImGui::GetFont(), m.label(), ImVec2(g.timeLeft.x, cy - ls.y / 2), kText, left.c_str());
  ImVec2 rs = measure(m.label(), right.c_str());
  dl->AddText(ImGui::GetFont(), m.label(), ImVec2(g.timeRight.right() - rs.x, cy - rs.y / 2), kTextDim, right.c_str());
  buildTimeline(g.strip, g.lane, now, interact);
  // The hints row: the status label at the left, at most four hints, the chips at the right.
  rnf_controller_family f = promptFamily();
  float gh = m.hint() * 1.45f, x = g.hints.x, hy = g.hints.y + (g.hints.h - gh) / 2;
  // Practicing: the label is the "■ Stop Practice" button (click / tap), then the rest of it.
  if (st.practicing) x += stopPracticeChip(dl, x, hy, gh) + S(16);
  std::string label = statusLabelText(!st.practicing);
  if (!label.empty()) {
    dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(x, hy + (gh - m.hint()) / 2), kText, label.c_str());
    x += measure(m.hint(), label.c_str()).x + S(26);
  }
  if (!interact) {
    ImGui::End();
    return;
  }
  float chipsLeft = buildChips(dl, g.hints.right(), hy, gh);
  auto hint = [&](std::initializer_list<const char*> els, const std::string& text) {
    for (const char* e : els) x += glyph(dl, ImVec2(x, hy), e, f, gh) + S(4);
    x += S(4);
    textFit(dl, m.hint(), ImVec2(x, hy + (gh - m.hint()) / 2), std::max(0.0f, chipsLeft - S(12) - x), kTextDim, text);
    x += measure(m.hint(), text.c_str()).x + S(22);
  };
  const std::string play = st.atTapeEnd ? TR("Continue (tape)") : TR("Play");
  if (controllerConnected()) {
    // At most four (docs/design/VTR_UX.md, "Practice markers").
    const int slot = std::clamp(d_.settings->timelineSlot, 0, 7);
    const std::string section = TRF("Section %lld", {slot + 1});
    const bool markers = markersActive();
    hint({"ui.cancel"}, play);
    if (st.practicing) {
      hint({"face.west"}, TR("Stop Practicing"));
      hint({"leftShoulder", "rightShoulder"}, section);
      hint({"leftShoulder", "menu"}, TR("Back to A"));
    } else if (markers) {
      const std::vector<uint64_t> mk = sectionMarkers(slot);
      if (markerUnderCursor(mk, st.frame) >= 0) hint({"face.north"}, TR("Delete Marker"));
      else if (mk.size() < 2) hint({"face.north"}, TR("Marker"));
      if (mk.size() >= 2) hint({"face.west"}, TR("Practice"));
      hint({"leftShoulder", "rightShoulder"}, section);
    }
    if (markers && now < markerHintUntil_ && !markerHint_.empty()) {
      float w = measure(m.hint(), markerHint_.c_str()).x;
      dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(chipsLeft - S(12) - w, hy + (gh - m.hint()) / 2), kTextDim, markerHint_.c_str());
    }
  } else {
    x += keycap(dl, ImVec2(x, hy), gh, "Space", false) + S(8);
    dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(x, hy + (gh - m.hint()) / 2), kTextDim, play.c_str());
  }
  ImGui::End();
  (void)io;
}

void UI::buildTimeline(const LRect& strip, const LRect& lane, double now, bool interact) {
  ImGuiIO& io = ImGui::GetIO();
  const EmuStatus& st = d_.emu->status();
  EmulationController* emu = d_.emu;
  const float laneH = lane.h, stripH = strip.h;
  uint32_t thumbW = RNF_THUMB_WIDTH, thumbH = RNF_THUMB_HEIGHT;
  thumbnailSize(emu->videoFrame(), emu->media(), &thumbW, &thumbH, rn_session_core_kind(emu->session()) != RN_CORE_EXTERNAL);
  const double tw = double(stripH) * thumbW / thumbH;
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
  if (!interact) drag_ = Drag::none;  // read-only (rewind / fast-forward held in play)
  if (interact && ImGui::IsItemActivated() && io.MouseDown[0] && st.hasSession && len > 0) {
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
  if (interact && drag_ != Drag::none && ImGui::IsItemActive() && io.MouseDown[0]) {
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
  if (interact && drag_ != Drag::none && !ImGui::IsItemActive()) {
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
        if (d_.renderer->thumbTextureSized(img.get()->serial, img.get()->px.data(), img.get()->width, img.get()->height, &tex, uv)) {
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
    // The recorded part ahead of the playhead is dimmed (it is what playing from here replaces).
    if (!st.practicing) {
      uint64_t head = scrubFrame_ ? *scrubFrame_ : st.frame;
      float hx0 = X(std::min(head, len)), hx1 = left + float(std::min<double>(gw, w));
      if (hx1 > hx0) dl->AddRectFilled(ImVec2(hx0, s0.y), ImVec2(hx1, s1.y), IM_COL32(0, 0, 0, 120));
    }
  }
  std::vector<rnf_timeline_range> shown = ranges;
  if (hasPreview_) {
    shown.erase(std::remove_if(shown.begin(), shown.end(), [&](const rnf_timeline_range& r) { return r.slot == preview_.slot; }),
                shown.end());
    shown.push_back(preview_);
  }
  // The controller's markers stand for the selected slot (they may be mid-edit): its range from them.
  bool flags = markersActive() && !hasPreview_ && drag_ == Drag::none;
  size_t markerCount = flags ? rnf_markers_count(markers_) : 0;
  if (flags) {
    shown.erase(std::remove_if(shown.begin(), shown.end(), [&](const rnf_timeline_range& r) { return r.slot == sel; }), shown.end());
    if (markerCount == 2)
      shown.push_back(rnf_timeline_range{sel, rnf_markers_frame(markers_, 0), 1, rnf_markers_frame(markers_, 1)});
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
  // Marker flags: a pole through the filmstrip and a small flag in the lane (A to the left of its
  // pole, B to the right; letters once both exist), the one under the cursor lit (north deletes it).
  if (flags) {
    std::vector<uint64_t> mk;
    for (size_t i = 0; i < markerCount; ++i) mk.push_back(rnf_markers_frame(markers_, i));
    const int under = markerUnderCursor(mk, scrubFrame_ ? *scrubFrame_ : st.frame);  // north deletes it
    for (size_t i = 0; i < markerCount; ++i) {
      float x = X(rnf_markers_frame(markers_, i));
      bool f = false, editing = false, lit = int(i) == under;
      ImU32 c = lit ? kBrand : slotColor(sel, 1.0f);
      dl->AddRectFilled(ImVec2(x - (lit ? 1.5f : 1.0f), top), ImVec2(x + (lit ? 1.5f : 1.0f), s1.y), c);
      float fw = laneH * 1.15f, fh = laneH - S(2);
      bool left = markerCount == 2 && i == 0;
      float fx = left ? x - fw : x;
      if (fx < strip.x) fx = x;
      if (fx + fw > strip.right()) fx = x - fw;
      LRect fr{fx, top, fw, fh};
      dl->AddRectFilled(tl(fr), br(fr), c, S(3));
      if (markerCount == 2) {
        const char* t = i == 0 ? "A" : "B";
        ImVec2 ts = measure(fh * 0.85f, t);
        dl->AddText(ImGui::GetFont(), fh * 0.85f, ImVec2(fx + (fw - ts.x) / 2, top + (fh - ts.y) / 2), IM_COL32(0, 0, 0, 255), t);
      }
      if (f) {
        LRect ring{fx - S(3), top - S(3), fw + S(6), fh + S(6)};
        focusRing(dl, ring, S(5), metrics_.ring() * (editing ? 1.5f : 1.0f), S(2));
      }
    }
  }
  for (const BookmarkInfo& b : emu->structure().bookmarks)
    if (b.onActiveTake) dl->AddRectFilled(ImVec2(X(b.frame) - 1, s0.y), ImVec2(X(b.frame) + 1, s0.y + S(12)), IM_COL32(255, 214, 10, 255));
  // The playhead: the neutral accent (never a red "recording" one), orange inside the practiced section.
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
    hc = IM_COL32(255, 255, 255, 255);
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
  std::string progress = timecode(std::min(st.practiceFrame, st.practiceLength > 0 ? st.practiceLength : st.practiceFrame), d_.emu->framePeriod()) + " / " +
                         (st.practiceLength > 0 ? timecode(st.practiceLength, d_.emu->framePeriod()) : std::string("--:--.--"));
  if (st.practiceLoops > 0) progress += "  " + TRF("Loop %lld", {st.practiceLoops + 1});
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float fs = m.hint() * 1.1f;
  std::string text = std::string(icons::glyph("repeat")) + "  " + current + "   " + progress;
  ImVec2 ts = measure(fs, text.c_str());
  ImVec2 p(m.margin(), metrics_.H - m.margin() - ts.y - S(16));
  dl->AddRectFilled(p, ImVec2(p.x + ts.x + S(28), p.y + ts.y + S(16)), IM_COL32(20, 20, 22, 200), (ts.y + S(16)) / 2);
  dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + S(14), p.y + S(8)), IM_COL32(255, 170, 60, 255), text.c_str());
}

// The countdown (3, 2, 1) over the paused picture - at A (practice) or before play resumes from a
// pause (the resume countdown): a large numeral on a soft dark disc with a thin ring running down the second; the look (fade / scale) is the core's
// (rnf_practice_countdown_visual), the same on macOS.
void UI::buildCountdown() {
  const EmuStatus& st = d_.emu->status();
  rnf_countdown_visual v = rnf_practice_countdown_visual(st.countdown, st.countdownFraction);
  if (v.number <= 0 || v.alpha <= 0.01f) return;
  ImGuiIO& io = ImGui::GetIO();
  GameRect g = gameRect(int(io.DisplaySize.x), int(io.DisplaySize.y));
  if (!g.visible || g.w <= 0 || g.h <= 0) g = GameRect{true, 0, 0, io.DisplaySize.x, io.DisplaySize.y};
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 c(g.x + g.w / 2, g.y + g.h / 2);
  float r = std::min(g.w, g.h) * 0.17f;
  float a = v.alpha;
  dl->AddCircleFilled(c, r, alpha(IM_COL32(20, 20, 22, 255), 0.62f * a), 96);
  // Ring: a faint full circle and the bright part of the second still left (from 12 o'clock).
  float ringW = std::max(2.0f, r * 0.045f), rr = r - ringW * 1.6f;
  dl->AddCircle(c, rr, alpha(IM_COL32(255, 255, 255, 255), 0.14f * a), 96, ringW);
  if (v.ring > 0.002f) {
    const float top = -3.14159265f / 2;
    dl->PathArcTo(c, rr, top, top + 2 * 3.14159265f * v.ring, 96);
    dl->PathStroke(alpha(IM_COL32(255, 255, 255, 255), 0.85f * a), 0, ringW);
  }
  // A 1-second countdown is the ring alone (Settings > System > More > Countdown: 1).
  if (rnf_countdown_seconds() > 1) {
    std::string n = std::to_string(v.number);
    float fs = r * 1.25f * v.scale;
    ImVec2 ts = measure(fs, n.c_str());
    dl->AddText(ImGui::GetFont(), fs, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), alpha(IM_COL32(255, 255, 255, 255), a), n.c_str());
  }
  // Play resumes before the end of the tape: one line under the numeral, in the same pass.
  if (st.retryNotice) {
    const char* line = TR("Retrying from here");
    float ls = std::max(metrics_.label(), r * 0.3f);
    ImVec2 lt = measure(ls, line);
    ImVec2 p(c.x - lt.x / 2, c.y + r + S(14));
    ImVec2 pad(S(16), S(8));
    dl->AddRectFilled(ImVec2(p.x - pad.x, p.y - pad.y), ImVec2(p.x + lt.x + pad.x, p.y + lt.y + pad.y),
                      alpha(IM_COL32(20, 20, 22, 255), 0.62f * a), (lt.y + pad.y * 2) / 2);
    dl->AddText(ImGui::GetFont(), ls, p, alpha(IM_COL32(255, 255, 255, 255), a), line);
  }
}

// ------------------------------------------------------------------ badges, status label, chips

// Top left: only the flash reduction indicator (no record / playback / transport badges: the status
// label at the bottom left says those, docs/design/VTR_UX.md).
void UI::buildBadges() {
#if RN_PRODUCT_FLASH_FILTER
  const EmuStatus& st = d_.emu->status();
  if (!(st.flashActive && d_.settings->showFlashIndicator)) return;
  const UiMetrics& m = metrics_;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float fs = m.hint();
  ImVec2 p(m.margin(), pill_.y + (pill_.h - fs - S(8)) / 2);
  ui::Pill(dl, p, TR("Flash Reduction Active"), IM_COL32(30, 150, 150, 220), fs);
#endif
}

std::string UI::statusLabelText(bool practicePart) const {
  if (!hasSession()) return {};
  const EmuStatus& st = d_.emu->status();
  if (st.live) return TR("ReplayNEScore · Development · No recording");
  rnf_tape_status ts{};
  ts.mode = st.practicing ? (practicePart ? RN_MODE_PRACTICE : RN_MODE_RECORD) : st.recording ? RN_MODE_RECORD : RN_MODE_REPLAY;
  ts.rewinding = st.rewinding;
  ts.fast_forward = st.fastForward;
  ts.slow_rate = st.slow;
  ts.end_of_tape = st.endOfTake && !st.practicing && st.paused;
  std::string out = takeStr(rnf_tape_status_label(&ts));
  if (exportJob_.running()) {
    std::string v = TRF("Making video… %lld%%", {(long long)std::lround(exportJob_.progress() * 100)});
    out = out.empty() ? v : out + " · " + v;
  }
  return out;
}

std::string UI::boundPadElement(const char* action) const {
  for (const rnf_binding& b : d_.input->bindings()) {
    if (std::strcmp(b.action, action) != 0) continue;
    std::string in = b.input;
    if (in.rfind("gc0:", 0) == 0 && in.find('+') == std::string::npos) return in.substr(4);
  }
  return {};
}

std::string UI::boundKeyName(const char* action) const {
  for (const rnf_binding& b : d_.input->bindings()) {
    if (std::strcmp(b.action, action) != 0) continue;
    std::string in = b.input;
    if (in.rfind("kb:", 0) != 0 || in.find('+') != std::string::npos) continue;
    const char* name = SDL_GetScancodeName(SDL_Scancode(std::atoi(in.c_str() + 3)));
    if (name && *name) return name;
  }
  return {};
}

float UI::stopPracticeChip(ImDrawList* dl, float x, float y, float h) {
  const std::string text = TR("■ Stop Practice");
  float w = S(12) * 2 + measure(metrics_.hint(), text.c_str()).x;  // chip()'s width without a glyph
  bool clicked = false;
  chip(dl, x + w, y, h, std::string(), text, &clicked);
  if (clicked) d_.emu->stopPractice();
  return w;
}

void UI::stopPracticeFromPad() {
  if (!hasSession() || menuOpen_ || interactive() || !d_.emu->status().practicing) return;
  d_.emu->stopPractice();
}

LRect UI::chip(ImDrawList* dl, float right, float y, float h, const std::string& element, const std::string& text, bool* clicked) {
  const UiMetrics& m = metrics_;
  float fs = m.hint(), padX = S(12), gh = h * 0.78f;
  rnf_controller_family f = promptFamily();
  float glyphW = element.empty() ? 0 : glyph(dl, ImVec2(), element, f, gh, false) + S(6);
  float w = padX + glyphW + measure(fs, text.c_str()).x + padX;
  LRect r{right - w, y, w, h};
  ImGuiIO& io = ImGui::GetIO();
  auto inside = [&](ImVec2 p) { return p.x >= r.x && p.x < r.right() && p.y >= r.y && p.y < r.bottom(); };
  bool hovered = inside(io.MousePos) && !popupOpen();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (clicked) *clicked = hovered && ImGui::IsMouseReleased(0) && inside(io.MouseClickedPos[0]);
  dl->AddRectFilled(tl(r), br(r), hovered ? IM_COL32(64, 64, 72, 245) : IM_COL32(40, 40, 46, 235), h / 2);
  dl->AddRect(tl(r), br(r), IM_COL32(255, 255, 255, 50), h / 2, 0, 1.0f);
  float x = r.x + padX;
  if (!element.empty()) x += glyph(dl, ImVec2(x, y + (h - gh) / 2), element, f, gh) + S(6);
  dl->AddText(ImGui::GetFont(), fs, ImVec2(x, y + (h - fs) / 2), kText, text.c_str());
  layoutRecord(r);
  return r;
}

float UI::buildChips(ImDrawList* dl, float right, float y, float h) {
  const EmuStatus& st = d_.emu->status();
  // "↶ Undo" for 10 s after a retry (its button: pad 1's binding of hk.undo_take, default L3).
  if (st.canUndo) {
    bool clicked = false;
    std::string el = controllerConnected() ? boundPadElement("hk.undo_take") : std::string();
    LRect r = chip(dl, right, y, h, el, TR("↶ Undo"), &clicked);
    right = r.x - S(10);
    if (clicked) undoRetry();
  }
  // Paused at the end of the tape: "▶ Make Video" (pads reach it through the menu: Share).
  if (st.paused && st.atTapeEnd && RNL_HAVE_MP4_EXPORT && !exportJob_.running()) {
    bool clicked = false;
    LRect r = chip(dl, right, y, h, std::string(), TR("▶ Make Video"), &clicked);
    right = r.x - S(10);
    if (clicked) openExportDialog();
  }
  return right;
}

// While playing (no seek bar): the status label at the bottom left (non-default states only) and
// the Undo chip at the bottom right, where the seek bar's hints row would be.
void UI::buildPlayOverlay(double now) {
  (void)now;
  const UiMetrics& m = metrics_;
  SeekGeometry g = layoutSeek(m);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float gh = m.hint() * 1.45f, hy = g.hints.y + (g.hints.h - gh) / 2;
  const EmuStatus& st = d_.emu->status();
  std::string label = statusLabelText(!st.practicing);
  float lx = g.hints.x;
  // Practicing: the label is the "■ Stop Practice" button (click / tap; Y on the pad), above the
  // practice pill (same corner), then the rest of the label.
  if (st.practicing) lx += stopPracticeChip(dl, lx, hy - gh - S(24), gh) + S(12);
  if (!label.empty()) {
    float fs = m.hint();
    ImVec2 p(lx, hy + (gh - fs - S(8)) / 2);
    if (st.practicing) p.y -= gh + S(24);
    ui::Pill(dl, p, label.c_str(), IM_COL32(20, 20, 22, 200), fs);
  }
  buildChips(dl, g.hints.right(), hy, gh);
}

// ------------------------------------------------------------------ first-run coach

void UI::tickCoach(double now) {
  if (!coach_) return;
  rnf_coach_input in{};
  bool rewindStopped = false;
  if (hasSession()) {
    const EmuStatus& st = d_.emu->status();
    rewindStopped = st.rewindStops != lastRewindStops_;
    lastRewindStops_ = st.rewindStops;
    in.playing = !st.live && !st.paused && !menuOpen_ && !interactive() && st.countdown == 0 && !st.practicing && !st.rewinding &&
                 !st.fastForward;
    in.rewind_stopped = rewindStopped && !menuOpen_;
    in.paused_at_end = st.paused && st.atTapeEnd && !menuOpen_ && !interactive();
    in.take_length = st.takeLength;
  }
  rnf_coach_tip tip = rnf_coach_tick(coach_, now, &in);
  if (tip == RNF_COACH_NONE) return;
  // The rewind control of the device in use: the pad's glyph (L2 of its family), else the key.
  std::string control;
  if (controllerConnected()) {
    std::string el = boundPadElement("hk.rewind");
    control = padGlyph(promptFamily(), (el.empty() ? std::string("leftTrigger") : el).c_str());
  } else {
    control = boundKeyName("hk.rewind");
  }
  notice(takeStr(rnf_coach_text(tip, control.c_str())), 5.0);
  if (d_.settings->coachShown != rnf_coach_shown(coach_)) {
    d_.settings->coachShown = rnf_coach_shown(coach_);
    changed();
  }
}

}  // namespace rnl
