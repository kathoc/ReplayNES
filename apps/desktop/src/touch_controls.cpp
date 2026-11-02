// The on-screen controller on touch screens (docs/design/TOUCH_ANDROID.md).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "touch_controls.h"

#include <algorithm>
#include <cmath>

#include "settings.h"

namespace rnl {

using touch::BBButton;
using touch::Point;
using touch::Rect;
using touch::TouchButton;
using touch::TouchExtra;
using touch::TouchLayout;

namespace {

// The iPhone's colours (TouchControlsView BBColor, NESTERM's :root).
constexpr ImU32 rgb(uint32_t c, float a = 1) {
  return IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, int(a * 255));
}
constexpr uint32_t kLine = 0x262d38, kText = 0xc7cedb, kTextDim = 0x7f8a9b, kAccent = 0x6ee7b7, kBtn = 0x232b36,
                   kBtnPress = 0x2f3a49;

}  // namespace

TouchControls::TouchControls(Host host) : host_(std::move(host)) {
  pad_ = std::make_unique<touch::TouchGamepad>([this](BBButton b, bool down) {
    if (auto id = touch::inputID(touch::touchButton(b))) host_.press(*id, down);
  });
  pad_->onHaptic = [this] {
    if (vibration_ && host_.haptic) host_.haptic();
  };
}

TouchControls::~TouchControls() = default;

void TouchControls::update(const Frame& f, const Settings& s) {
  const bool show = f.show && f.widthPx > 0 && f.heightPx > 0;
  if (!show) {
    if (visible_) releaseAll();
    visible_ = false;
    return;
  }
  visible_ = true;
  vibration_ = s.touchVibration;
  opacity_ = std::clamp(s.touchOpacity, 15, 100) / 100.0f;
  density_ = f.density > 0 ? f.density : 1;
  windowToPx_ = f.windowToPx > 0 ? f.windowToPx : 1;
  touch::Size size{f.widthPx / density_, f.heightPx / density_};
  TouchLayout::Options o;
  o.showAABB = s.touchTurbo;
  o.dpadSize = std::clamp(double(s.touchDpadSize), TouchLayout::dpadRangeLo, TouchLayout::dpadRangeHi);
  o.ways = s.touchWays;
  o.fill = f.fill;
  o.pictureAspect = f.pictureAspect;
  o.buttons = std::clamp(f.fireButtons, 2, 4);
  if (!haveLayout_ || !(size == size_) || !(o == options_) || !(f.safe == safe_)) {
    // A new layout (the window turned or resized, a setting, another game): everything released, the
    // D-pad's home back at its place (TouchControlsView on rotation).
    releaseAll();
    layout_ = TouchLayout::make(size, f.safe, o);
    pad_->geometry = layout_.pad;
    pad_->relayout();
    pad_->resetHome();
    size_ = size;
    safe_ = f.safe;
    options_ = o;
    haveLayout_ = true;
  }
  if (ways_ != s.touchWays) {
    ways_ = s.touchWays;
    pad_->setWays(ways_);
  }
  pad_->setHomeGain(s.touchFollowX, s.touchFollowY);
  if (pad_->hasPendingAnimationFrame()) pad_->animationFrame(f.nowMs);
}

std::optional<ImVec4> TouchControls::pictureArea() const {
  if (!visible_ || !haveLayout_) return std::nullopt;
  const Rect& g = layout_.gameArea;
  return ImVec4(float(g.x * density_), float(g.y * density_), float(g.w * density_), float(g.h * density_));
}

std::optional<ImVec4> TouchControls::overlayArea() const {
  if (!visible_ || !haveLayout_) return std::nullopt;
  const Rect& o = layout_.overlayArea;
  return ImVec4(float(o.x * density_), float(o.y * density_), float(o.w * density_), float(o.h * density_));
}

Point TouchControls::toPoints(float nx, float ny) const { return {nx * size_.width, ny * size_.height}; }

bool TouchControls::takes(Point p) const {
  for (const TouchExtra& x : layout_.extras)
    if (x.hit(p)) return true;
  if (pad_->regionAt(p)) return true;
  const float px = float(p.x * density_), py = float(p.y * density_);
  if (picture_.z > 0 && px >= picture_.x && px < picture_.x + picture_.z && py >= picture_.y && py < picture_.y + picture_.w) {
    const float q = (px - picture_.x) / picture_.z;
    return q < 0.25f || q >= 0.75f;
  }
  return false;
}

void TouchControls::onExtra(const TouchExtra& e, bool down) {
  if (e.kind.type == TouchExtra::Kind::ways) {
    if (down) {
      ways_ = ways_ == 8 ? 4 : 8;
      pad_->setWays(ways_);
    }
    return;
  }
  const TouchButton b = e.kind.btn;
  if (auto id = touch::inputID(b)) {  // AA / BB / C / D: held until the finger leaves the screen
    host_.press(*id, down);
    if (down && vibration_ && host_.haptic) host_.haptic();
    return;
  }
  if (!down) return;
  if (b == TouchButton::menu && host_.toggleMenu) host_.toggleMenu();
  else if (b == TouchButton::pause && host_.togglePause) host_.togglePause();
  else if (b == TouchButton::slow && host_.toggleSlow) host_.toggleSlow();
}

bool TouchControls::handleFinger(const SDL_Event& e, double nowMs) {
  if (!visible_) return false;
  const SDL_FingerID id = e.tfinger.fingerID;
  const Point p = toPoints(e.tfinger.x, e.tfinger.y);
  touch::PointerEvent pe;
  pe.id = int(id & 0x7fffffff);
  pe.x = p.x;
  pe.y = p.y;
  pe.t = nowMs;
  if (e.type == SDL_EVENT_FINGER_DOWN) {
    // The extras are on top (TouchExtrasView), then the pad, then the picture's quarters.
    for (const TouchExtra& x : layout_.extras) {
      if (!x.hit(p)) continue;
      fingers_[id] = Finger{Owner::extra, x};
      onExtra(x, true);
      return true;
    }
    if (pad_->pointerDown(pe)) {
      fingers_[id] = Finger{Owner::pad, std::nullopt};
      return true;
    }
    const float px = float(p.x * density_), py = float(p.y * density_);
    if (picture_.z > 0 && px >= picture_.x && px < picture_.x + picture_.z && py >= picture_.y && py < picture_.y + picture_.w) {
      const float q = (px - picture_.x) / picture_.z;
      if (q < 0.25f && !rewindHeld_) {
        rewindHeld_ = true;
        fingers_[id] = Finger{Owner::rewind, std::nullopt};
        host_.press("touch:l2", true);
        return true;
      }
      if (q >= 0.75f && !fastForwardHeld_) {
        fastForwardHeld_ = true;
        fingers_[id] = Finger{Owner::fastForward, std::nullopt};
        host_.press("touch:r2", true);
        return true;
      }
    }
    return false;  // the UI's (the seek bar while paused ...)
  }
  auto it = fingers_.find(id);
  if (it == fingers_.end()) return false;
  if (e.type == SDL_EVENT_FINGER_MOTION) {
    if (it->second.owner == Owner::pad) pad_->pointerMove(pe);
    return true;
  }
  // Up or cancelled.
  switch (it->second.owner) {
    case Owner::pad: pad_->pointerUp(pe); break;
    case Owner::extra: onExtra(*it->second.extra, false); break;
    case Owner::rewind:
      rewindHeld_ = false;
      host_.press("touch:l2", false);
      break;
    case Owner::fastForward:
      fastForwardHeld_ = false;
      host_.press("touch:r2", false);
      break;
  }
  fingers_.erase(it);
  return true;
}

bool TouchControls::takesTouchMouse(const SDL_Event& e) {
  if (!visible_) return false;
  bool touchMouse = false;
  float x = 0, y = 0;
  switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION: touchMouse = e.motion.which == SDL_TOUCH_MOUSEID; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      touchMouse = e.button.which == SDL_TOUCH_MOUSEID;
      x = e.button.x;
      y = e.button.y;
      break;
    case SDL_EVENT_MOUSE_WHEEL: touchMouse = e.wheel.which == SDL_TOUCH_MOUSEID; break;
    default: return false;
  }
  if (!touchMouse) return false;
  // SDL sends the mouse events it makes from a touch before the finger event: the press is judged by
  // where it lands, the motion and release follow it.
  if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
    mouseTaken_ = takes(Point{x * windowToPx_ / density_, y * windowToPx_ / density_});
  const bool taken = mouseTaken_;
  if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) mouseTaken_ = false;
  return taken;
}

void TouchControls::releaseAll() {
  for (auto& [id, f] : fingers_) {
    if (f.owner == Owner::extra && f.extra) {
      if (auto in = touch::inputID(f.extra->kind.btn); in && f.extra->kind.type == TouchExtra::Kind::button) host_.press(*in, false);
    }
  }
  if (rewindHeld_) host_.press("touch:l2", false);
  if (fastForwardHeld_) host_.press("touch:r2", false);
  rewindHeld_ = fastForwardHeld_ = false;
  fingers_.clear();
  mouseTaken_ = false;
  if (pad_) pad_->releaseAll();
}

void TouchControls::draw(ImDrawList* dl, bool paused, bool slow) const {
  if (!visible_ || !haveLayout_ || !dl) return;
  const float k = float(density_);
  const float a = opacity_;
  auto P = [&](double x, double y) { return ImVec2(float(x) * k, float(y) * k); };
  ImFont* font = ImGui::GetFont();
  auto label = [&](const std::string& t, ImVec2 c, float size, ImU32 col) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, t.c_str());
    dl->AddText(font, size, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col, t.c_str());
  };

  // D-pad: a cross at its place moved by the home offset (ox, oy); pressed arms in the accent.
  {
    const Rect& d = layout_.pad.dpad;
    const double cx = d.midX() + pad_->ox, cy = d.midY() + pad_->oy, half = d.w / 2, arm = d.w * 0.34 / 2;
    const ImU32 fill = rgb(kBtn, a), edge = rgb(kLine, a), on = rgb(kAccent, a * 0.85f);
    const float r = float(arm * 0.35) * k;
    dl->AddRectFilled(P(cx - half, cy - arm), P(cx + half, cy + arm), fill, r);
    dl->AddRectFilled(P(cx - arm, cy - half), P(cx + arm, cy + half), fill, r);
    dl->AddRect(P(cx - half, cy - arm), P(cx + half, cy + arm), edge, r, 0, 1.0f * k);
    dl->AddRect(P(cx - arm, cy - half), P(cx + arm, cy + half), edge, r, 0, 1.0f * k);
    if (pad_->isActive(BBButton::left)) dl->AddRectFilled(P(cx - half, cy - arm), P(cx - arm, cy + arm), on, r);
    if (pad_->isActive(BBButton::right)) dl->AddRectFilled(P(cx + arm, cy - arm), P(cx + half, cy + arm), on, r);
    if (pad_->isActive(BBButton::up)) dl->AddRectFilled(P(cx - arm, cy - half), P(cx + arm, cy - arm), on, r);
    if (pad_->isActive(BBButton::down)) dl->AddRectFilled(P(cx - arm, cy + arm), P(cx + arm, cy + half), on, r);
    // The arrows.
    const double t = arm * 0.55, o = half - arm * 0.9;
    const ImU32 arrow = rgb(kTextDim, a);
    dl->AddTriangleFilled(P(cx + o + t * 0.6, cy), P(cx + o - t * 0.4, cy - t * 0.7), P(cx + o - t * 0.4, cy + t * 0.7), arrow);
    dl->AddTriangleFilled(P(cx - o - t * 0.6, cy), P(cx - o + t * 0.4, cy - t * 0.7), P(cx - o + t * 0.4, cy + t * 0.7), arrow);
    dl->AddTriangleFilled(P(cx, cy - o - t * 0.6), P(cx - t * 0.7, cy - o + t * 0.4), P(cx + t * 0.7, cy - o + t * 0.4), arrow);
    dl->AddTriangleFilled(P(cx, cy + o + t * 0.6), P(cx - t * 0.7, cy + o - t * 0.4), P(cx + t * 0.7, cy + o - t * 0.4), arrow);
  }
  // A / B.
  const double fb = layout_.faceButton;
  for (auto [btn, c, name] : {std::tuple{BBButton::a, layout_.pad.a, "A"}, std::tuple{BBButton::b, layout_.pad.b, "B"}}) {
    const bool on = pad_->isActive(btn);
    const double y = c.y + (on ? layout_.pad.pressOffset : 0);
    dl->AddCircleFilled(P(c.x, y), float(fb / 2) * k, rgb(on ? kBtnPress : kBtn, a), 48);
    dl->AddCircle(P(c.x, y), float(fb / 2) * k, rgb(on ? kAccent : kLine, a), 48, (on ? 2.0f : 1.0f) * k);
    label(name, P(c.x, y), float(fb * 0.36) * k, rgb(on ? kAccent : kText, a));
  }
  // SELECT / START.
  for (auto [btn, r, name] : {std::tuple{BBButton::select, layout_.select, "SELECT"}, std::tuple{BBButton::start, layout_.start, "START"}}) {
    const bool on = pad_->isActive(btn);
    dl->AddRectFilled(P(r.x, r.y), P(r.maxX(), r.maxY()), rgb(on ? kBtnPress : kBtn, a), float(r.h / 2) * k);
    dl->AddRect(P(r.x, r.y), P(r.maxX(), r.maxY()), rgb(on ? kAccent : kLine, a), float(r.h / 2) * k, 0, 1.0f * k);
    label(name, P(r.midX(), r.midY()), float(TouchLayout::pillFont) * k, rgb(on ? kAccent : kTextDim, a));
  }
  // The extras.
  for (const TouchExtra& x : layout_.extras) {
    const Rect& r = x.frame;
    bool held = false;
    for (const auto& [id, f] : fingers_)
      held = held || (f.owner == Owner::extra && f.extra && f.extra->kind == x.kind);
    const bool lit = held || (x.kind.type == TouchExtra::Kind::button && ((x.kind.btn == TouchButton::pause && paused) ||
                                                                          (x.kind.btn == TouchButton::slow && slow)));
    const ImU32 fill = rgb(held ? kBtnPress : kBtn, a), edge = rgb(lit ? kAccent : kLine, a), fg = rgb(lit ? kAccent : kText, a);
    if (x.style.type == TouchExtra::Style::round) {
      dl->AddCircleFilled(P(r.midX(), r.midY()), float(r.w / 2) * k, fill, 40);
      dl->AddCircle(P(r.midX(), r.midY()), float(r.w / 2) * k, edge, 40, 1.0f * k);
      label(TouchLayout::label(x.kind, ways_), P(r.midX(), r.midY()), float(TouchLayout::turboFont) * k, fg);
      continue;
    }
    dl->AddRectFilled(P(r.x, r.y), P(r.maxX(), r.maxY()), fill, float(std::min(r.h, r.w) / 2) * k);
    dl->AddRect(P(r.x, r.y), P(r.maxX(), r.maxY()), edge, float(std::min(r.h, r.w) / 2) * k, 0, 1.0f * k);
    if (x.style.type == TouchExtra::Style::icon && x.kind.btn == TouchButton::pause) {
      const double w = r.h * 0.12, h = r.h * 0.42, gap = r.h * 0.12;
      if (paused) {  // a play triangle: tapping resumes
        dl->AddTriangleFilled(P(r.midX() - h * 0.4, r.midY() - h / 2), P(r.midX() - h * 0.4, r.midY() + h / 2),
                              P(r.midX() + h * 0.5, r.midY()), fg);
      } else {
        dl->AddRectFilled(P(r.midX() - gap / 2 - w, r.midY() - h / 2), P(r.midX() - gap / 2, r.midY() + h / 2), fg);
        dl->AddRectFilled(P(r.midX() + gap / 2, r.midY() - h / 2), P(r.midX() + gap / 2 + w, r.midY() + h / 2), fg);
      }
    } else if (x.style.type == TouchExtra::Style::icon && x.kind.btn == TouchButton::slow) {
      label("1/2", P(r.midX(), r.midY()), float(TouchLayout::pillFont + 1) * k, fg);
    } else {
      label(TouchLayout::label(x.kind, ways_), P(r.midX(), r.midY()), float(TouchLayout::pillFont) * k, fg);
    }
  }
}

}  // namespace rnl
