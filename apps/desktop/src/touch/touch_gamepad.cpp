// The on-screen touch gamepad (NESTERM's Brickboy controller): see touch_gamepad.h.
// Port of apps/ios/Sources/Play/Brickboy/TouchGamepad.swift (ios-generalize), function by function.
// Original: NESTERM web/brickboy/input.mjs.
//
// Copyright 2026 Katsuya Hokamura. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "touch/touch_gamepad.h"

#include <cmath>

// Swift does not fuse a * b + c; keep the same rounding as the iPhone's reference data.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace rnl::touch {

namespace {

constexpr double kPi = 3.14159265358979323846;

double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
double lerp(double a, double b, double t) { return a + (b - a) * t; }

const std::array<std::vector<BBButton>, 8> kBtns = {{
    {BBButton::right},
    {BBButton::down, BBButton::right},
    {BBButton::down},
    {BBButton::down, BBButton::left},
    {BBButton::left},
    {BBButton::up, BBButton::left},
    {BBButton::up},
    {BBButton::up, BBButton::right},
}};

std::array<std::set<BBButton>, 8> makeDir8Sets() {
  std::array<std::set<BBButton>, 8> out;
  for (size_t i = 0; i < 8; i++) out[i] = std::set<BBButton>(kBtns[i].begin(), kBtns[i].end());
  return out;
}

bool contains(const std::set<BBButton>& s, BBButton b) { return s.count(b) != 0; }

}  // namespace

double c(Dir8 d) {
  switch (d) {
    case Dir8::R: return 0;
    case Dir8::DR: return 45;
    case Dir8::D: return 90;
    case Dir8::DL: return 135;
    case Dir8::L: return 180;
    case Dir8::UL: return 225;
    case Dir8::U: return 270;
    case Dir8::UR: return 315;
  }
  return 0;
}

const std::vector<BBButton>& btns(Dir8 d) { return kBtns[static_cast<size_t>(d)]; }

double halfOf(Dir8 name, double cardHalf) { return btns(name).size() == 2 ? 45 - cardHalf : cardHalf; }

const std::array<Dir8, 8> DIR8_ORDER = {Dir8::R, Dir8::DR, Dir8::D, Dir8::DL, Dir8::L, Dir8::UL, Dir8::U, Dir8::UR};
const std::array<std::set<BBButton>, 8> DIR8_SETS = makeDir8Sets();
const std::set<BBButton> NO_DIRS;

namespace {
const std::set<BBButton>& setOf(Dir8 d) { return DIR8_SETS[static_cast<size_t>(d)]; }
}  // namespace

// JS % keeps the dividend's sign, as fmod does.
double norm360(double deg) { return std::fmod(std::fmod(deg, 360) + 360, 360); }

double angularDist(double a, double b) {
  double d = std::abs(norm360(a) - norm360(b));
  return d > 180 ? 360 - d : d;
}

Dir8 sectorRaw(double angle, double cardHalf) {
  for (Dir8 name : DIR8_ORDER)
    if (angularDist(angle, c(name)) <= halfOf(name, cardHalf)) return name;
  return Dir8::R;  // unreachable (bands tile 360 degrees), keeps the function total
}

TouchGamepad::TouchGamepad(std::function<void(BBButton, bool)> press_, std::function<void(BBButton)> feedback_)
    : press(std::move(press_)), feedback(std::move(feedback_)) {}

// MARK: requestAnimationFrame

int TouchGamepad::requestAnimationFrame(std::function<void(double)> cb) {
  int id = rafNext;
  rafNext += 1;
  rafCallbacks.push_back({id, std::move(cb)});
  if (onAnimationFrameRequest) onAnimationFrameRequest();
  return id;
}

void TouchGamepad::cancelAnimationFrame(int id) {
  rafCallbacks.erase(std::remove_if(rafCallbacks.begin(), rafCallbacks.end(), [id](const RafCallback& r) { return r.id == id; }),
                     rafCallbacks.end());
}

bool TouchGamepad::hasPendingAnimationFrame() const { return !rafCallbacks.empty(); }

void TouchGamepad::animationFrame(double now_) {
  now = now_;
  std::vector<RafCallback> run = rafCallbacks;
  rafCallbacks.clear();
  for (auto& r : run) r.cb(now_);
}

// MARK: public surface

bool TouchGamepad::isActive(BBButton btn) const {
  auto it = counts.find(btn);
  return (it == counts.end() ? 0 : it->second) > 0;
}

int TouchGamepad::wayCount() const { return ways; }

void TouchGamepad::relayout() { setHome(ox, oy); }

std::optional<TouchGamepad::Region> TouchGamepad::regionAt(Point p) const {
  const Geometry& g = geometry;
  if (g.face.contains(p)) return Region::face;
  if (g.system) {
    if (g.system->contains(p)) return Region::system;
  } else if (g.select.contains(p) || g.start.contains(p)) {
    return Region::system;
  }
  Rect pad = g.dpad.offsetBy(ox, oy);
  double ext = pad.height() * g.dpadExtFraction;
  Rect hit{pad.minX(), pad.minY() - ext, pad.width(), pad.height() + 2 * ext};
  if (hit.contains(p) && (g.dpadClip ? g.dpadClip->contains(p) : true)) return Region::dpad;
  return std::nullopt;
}

std::optional<TouchGamepad::Region> TouchGamepad::pointerDown(const PointerEvent& e) {
  now = e.t;
  auto r = regionAt(Point{e.x, e.y});
  if (!r) return std::nullopt;
  captured[e.id] = *r;
  switch (*r) {
    case Region::dpad: onDpadDown(e); break;
    case Region::face: onFaceDown(e); break;
    case Region::system: onSysDown(e); break;
  }
  return r;
}

void TouchGamepad::pointerMove(const PointerEvent& e) {
  now = e.t;
  auto it = captured.find(e.id);
  if (it == captured.end()) return;
  switch (it->second) {
    case Region::dpad: onDpadMove(e); break;
    case Region::face: onFaceMove(e); break;
    case Region::system: onSysMove(e); break;
  }
}

void TouchGamepad::pointerUp(const PointerEvent& e) {
  now = e.t;
  auto it = captured.find(e.id);
  if (it != captured.end()) {
    Region r = it->second;
    captured.erase(it);
    switch (r) {
      case Region::dpad: onDpadUp(e); break;
      case Region::face: onFaceUp(e); break;
      case Region::system: onSysUp(e); break;
    }
  }
  // Backstop: if an up/cancel never reaches the element that captured the pointer, nothing would
  // ever release its buttons (a no-op after a normal release).
  releasePointer(e.id);
}

void TouchGamepad::releasePointer(int id) {
  auto dp = dpadPtrs.find(id);
  if (dp != dpadPtrs.end()) {
    std::shared_ptr<DpadPtr> st = dp->second;
    applyDpad(*st, NO_DIRS);
    removeDpadPtr(id);
  }
  if (facePointers.count(id) != 0) {
    applyFace(id, {});
    removeFacePointer(id);
    faceLatch.erase(id);
    faceRoll.erase(id);
  }
  if (sysPointers.count(id) != 0) {
    applySystem(id, {});
    sysPointers.erase(id);
  }
}

void TouchGamepad::setWays(int ways_) {
  if (ways == ways_) return;
  ways = ways_;
  // Anything held under the old geometry may not be legal under the new one.
  std::vector<int> order = dpadOrder;
  for (int id : order) {
    auto it = dpadPtrs.find(id);
    if (it != dpadPtrs.end()) {
      std::shared_ptr<DpadPtr> st = it->second;
      readDirs(*st);
    }
  }
}

void TouchGamepad::setHomeGain(double x, double y) {
  gainX = x;
  gainY = y;
}

void TouchGamepad::releaseAll() {
  for (BBButton btn : BBButton_allCases) {
    if (isActive(btn)) {
      press(btn, false);
      pressAnim[btn] = PressAnim{pressShift(btn), now};
    }
  }
  counts.clear();
  dpadPtrs.clear();
  dpadOrder.clear();
  facePointers.clear();
  facePointerOrder.clear();
  faceLatch.clear();
  faceRoll.clear();
  sysPointers.clear();
  captured.clear();
  resetHome();
  if (onVisual) onVisual();
}

// ---- reference-counted edge dispatch ----

void TouchGamepad::setBtn(BBButton btn, bool down) {
  auto it = counts.find(btn);
  int c0 = it == counts.end() ? 0 : it->second;
  int nc = down ? c0 + 1 : std::max(0, c0 - 1);
  // The visual edge restarts the round's transform transition from where it is now.
  if ((c0 == 0) != (nc == 0)) pressAnim[btn] = PressAnim{pressShift(btn), now};
  counts[btn] = nc;
  if (c0 == 0 && nc > 0) {
    press(btn, true);
    if (onVisual) onVisual();
    // One tick per gesture batch: a diagonal engages two buttons but buzzes once.
    if (hapticArmed) {
      hapticArmed = false;
      haptic();
      if (feedback) feedback(btn);
    }
  } else if (c0 > 0 && nc == 0) {
    press(btn, false);
    if (onVisual) onVisual();
  }
}

void TouchGamepad::haptic() {
  if (onHaptic) onHaptic();
}

// ---- D-pad ----

void TouchGamepad::setHome(double ox_, double oy_) {
  Rect rect = elemRect();
  double natLeft = rect.minX() - ox;
  double natTop = rect.minY() - oy;
  double radius = rect.width() / 2;
  double vw = geometry.viewport.width, vh = geometry.viewport.height;
  double capX = HOME_MAX_X * radius;
  double capY = HOME_MAX_Y * radius;
  // The frame the container actually leaves free (frameRect), in place of the raw insets.
  Rect frame = geometry.frame;
  double fl = frame.minX();
  double fr = vw - frame.maxX();
  double ft = frame.minY();
  double fb = vh - frame.maxY();
  // Only the left edge is matched against what is painted, not the hit box.
  double visLeft = natLeft + visibleLeftOffset();
  double minOx = std::max(fl - visLeft, -capX);
  double maxOx = std::min(vw - EDGE_MARGIN - fr - (natLeft + rect.width()), capX);
  double minOy = std::max(EDGE_MARGIN + ft - natTop, -capY);
  double maxOy = std::min(vh - EDGE_MARGIN - fb - (natTop + rect.height()), capY);
  // Degenerate viewport (pad wider than the room): keep at least one edge in.
  if (maxOx < minOx) maxOx = minOx;
  if (maxOy < minOy) maxOy = minOy;
  ox = clamp(ox_, minOx, maxOx);
  oy = clamp(oy_, minOy, maxOy);
  if (onVisual) onVisual();  // dpad.style.transform
}

void TouchGamepad::resetHome() {
  stopHomeReturn();
  ox = 0;
  oy = 0;
  if (onVisual) onVisual();
}

void TouchGamepad::stopHomeReturn() {
  if (homingRaf != 0) {
    cancelAnimationFrame(homingRaf);
    homingRaf = 0;
  }
}

void TouchGamepad::startHomeReturn() {
  if (homingRaf != 0) return;
  homeTickLast = 0;
  homeStartT = 0;
  auto est = restEstimate();
  homeRest = est ? *est : Point{};
  homingRaf = requestAnimationFrame([this](double t) { homeReturnTick(t); });
}

void TouchGamepad::homeReturnTick(double now_) {
  if (!dpadPtrs.empty()) {
    homingRaf = 0;  // a finger arrived - leave the home where it is
    return;
  }
  if (homeStartT == 0) homeStartT = now_;
  if (homeTickLast == 0) homeTickLast = now_;
  double dt = std::min(64.0, now_ - homeTickLast);  // clamp after a stall
  homeTickLast = now_;
  // Hold still first: during rapid tapping the gaps never reach this.
  if (now_ - homeStartT < HOME_RETURN_DELAY) {
    homingRaf = requestAnimationFrame([this](double t) { homeReturnTick(t); });
    return;
  }
  Point rest = homeRest;
  double decay = std::exp(-dt / HOME_RETURN_TAU);  // frame-rate independent
  double nx = rest.x + (ox - rest.x) * decay;
  double ny = rest.y + (oy - rest.y) * decay;
  if (std::abs(nx - rest.x) < 0.5 && std::abs(ny - rest.y) < 0.5) {
    homingRaf = 0;
    setHome(rest.x, rest.y);
    return;
  }
  setHome(nx, ny);
  homingRaf = requestAnimationFrame([this](double t) { homeReturnTick(t); });
}

Point TouchGamepad::toElem(double dx, double dy) const { return Point{dx, dy}; }

TouchGamepad::PadBox TouchGamepad::padBox() const {
  Rect r = elemRect();
  return PadBox{r.width(), r.height(), r.midX(), r.midY()};
}

Rect TouchGamepad::elemRect() const { return geometry.dpad.offsetBy(ox, oy); }

double TouchGamepad::visibleLeftOffset() const {
  double minV = 0;
  if (isActive(BBButton::left)) minV = std::min(minV, geometry.edgeLeftOutset);
  return minV;
}

std::set<BBButton> TouchGamepad::resolveDirs(DpadPtr& st, double vx, double vy, double r, double rCommit, bool sticky,
                                             double cardHalf, double engageDz) {
  // A direction already engaged is LATCHED until the thumb has travelled far enough out.
  if (st.dir8 && r < rCommit) return setOf(*st.dir8);
  // Nothing engaged yet and too close to the origin for the angle to mean anything.
  if (!st.dir8 && r < engageDz) return NO_DIRS;
  double angle = norm360(std::atan2(vy, vx) * 180 / kPi);
  Dir8 dir = sectorRaw(angle, cardHalf);
  // Hysteresis holds the current sector only while nearly still (anti-chatter).
  if (sticky && st.dir8 && *st.dir8 != dir) {
    Dir8 cur = *st.dir8;
    if (angularDist(angle, c(cur)) <= halfOf(cur, cardHalf) + ANGLE_HYST) dir = cur;
  }
  st.dir8 = dir;
  return setOf(dir);
}

void TouchGamepad::applyDpad(DpadPtr& st, const std::set<BBButton>& nextIn) {
  std::set<BBButton> next = nextIn;
  std::set<BBButton> prev = st.dirs;
  for (BBButton b : prev)
    if (!contains(next, b)) setBtn(b, false);
  for (BBButton b : next)
    if (!contains(prev, b)) setBtn(b, true);
  st.dirs = next;
}

// ---- thumb rest-point estimator (touchdowns) ----

void TouchGamepad::foldTouchdown(double px, double py, double hx, double hy) {
  double a = std::hypot(hx, hy);
  // A tap essentially on the origin has no meaningful direction.
  double ux = a > 1 ? hx / a : 0;
  double uy = a > 1 ? hy / a : 0;
  tdW = tdW * TD_DECAY + 1;
  tdX = tdX * TD_DECAY + px;
  tdY = tdY * TD_DECAY + py;
  tdA = tdA * TD_DECAY + a;
  tdUX = tdUX * TD_DECAY + ux;
  tdUY = tdUY * TD_DECAY + uy;
}

double TouchGamepad::engageDeadzone(double radius) const {
  double reach = tdW >= TD_MIN_W ? tdA / tdW : radius * REACH_DEFAULT;
  double k = ways == 4 ? R_COMMIT_4WAY : 1;
  return clamp(reach * DZ_OF_REACH * k, radius * DZ_MIN * k, radius * DZ_MAX * k);
}

double TouchGamepad::commitRadius(double radius) const {
  double reach = tdW >= TD_MIN_W ? tdA / tdW : radius * REACH_DEFAULT;
  double k = ways == 4 ? R_COMMIT_4WAY : 1;
  return clamp(reach * R_COMMIT_OF_REACH * k, radius * R_COMMIT_MIN * k, radius * R_COMMIT_MAX * k);
}

std::optional<Point> TouchGamepad::restEstimate() const {
  if (tdW < TD_MIN_W) return std::nullopt;
  double a = tdA / tdW;
  return Point{tdX / tdW - (a * tdUX) / tdW, tdY / tdW - (a * tdUY) / tdW};
}

void TouchGamepad::calibrateFromTaps() {
  auto est = restEstimate();
  if (!est) return;
  setHome(ox + (est->x - ox) * TD_GAIN * gainX - ox * TD_LEAK,
          oy + (est->y - oy) * TD_GAIN * gainY - oy * TD_LEAK);
}

void TouchGamepad::onDpadDown(const PointerEvent& e) {
  hapticArmed = true;
  // This id may still be holding buttons from a touch whose up never arrived.
  releasePointer(e.id);
  // The D-pad is owned by the LAST finger that touched it (so a fast re-tap passes through zero and
  // produces both edges).
  {
    std::vector<int> order = dpadOrder;
    for (int id : order)
      if (id != e.id) releasePointer(id);
  }
  // A fresh touch keeps whatever home the release-return has eased to so far.
  stopHomeReturn();
  PadBox box = padBox();
  double radius = box.w / 2;
  Point v = toElem(e.x - box.cx, e.y - box.cy);
  auto st = std::make_shared<DpadPtr>(e.x, e.y, e.t);
  // Touchdown is read absolutely from the current home, with no radius floor.
  (void)resolveDirs(*st, v.x, v.y, std::hypot(v.x, v.y), commitRadius(radius), true,
                    ways == 4 ? CARD_HALF_4WAY : CARD_HALF_ENGAGE, engageDeadzone(radius));
  // Learn from the touchdown, then re-read it from the corrected home. Skipped while another finger
  // already owns the pad.
  dpadPtrs[e.id] = st;
  if (std::find(dpadOrder.begin(), dpadOrder.end(), e.id) == dpadOrder.end()) dpadOrder.push_back(e.id);
  if (dpadPtrs.size() == 1) {
    foldTouchdown(v.x + ox, v.y + oy, v.x, v.y);
    calibrateFromTaps();
    // Clear the provisional read so the re-read is absolute.
    st->dir8 = std::nullopt;
  }
  readDirs(*st);
  st->calibDir = st->dir8;
  startCalib();
}

void TouchGamepad::onDpadMove(const PointerEvent& e) {
  auto it = dpadPtrs.find(e.id);
  if (it == dpadPtrs.end()) return;
  std::shared_ptr<DpadPtr> st = it->second;
  hapticArmed = true;
  double dt = std::max(1.0, e.t - st->lastT);
  double dxStep = e.x - st->lastX;
  double dyStep = e.y - st->lastY;
  double step = std::hypot(dxStep, dyStep);
  st->speed = lerp(st->speed, step / dt, 0.6);  // EMA of px/ms (fairly responsive)
  st->lastX = e.x;
  st->lastY = e.y;
  st->lastT = e.t;
  st->accDX += dxStep;
  st->accDY += dyStep;
  readDirs(*st);
}

void TouchGamepad::readDirs(DpadPtr& st) {
  PadBox box = padBox();
  double radius = box.w / 2;
  Point v = toElem(st.lastX - box.cx, st.lastY - box.cy);
  double r = std::hypot(v.x, v.y);
  // Hysteresis only while nearly still; an active sweep tracks the raw angle.
  bool sticky = st.speed < SPEED_STILL * 2;
  // Cardinal-heavy on the first read, easing to an even split as the press is held.
  double cardHalf = ways == 4
                        ? CARD_HALF_4WAY
                        : lerp(CARD_HALF_ENGAGE, CARD_HALF_HELD, clamp(double(st.heldTicks) / SHARE_RAMP_TICKS, 0, 1));
  applyDpad(st, resolveDirs(st, v.x, v.y, r, commitRadius(radius), sticky, cardHalf, engageDeadzone(radius)));
}

void TouchGamepad::startCalib() {
  if (calibRaf != 0) return;
  calibLast = 0;
  calibRaf = requestAnimationFrame([this](double t) { calibTick(t); });
}

void TouchGamepad::calibTick(double now_) {
  if (dpadPtrs.empty()) {
    calibRaf = 0;
    return;
  }
  if (calibLast == 0) calibLast = now_;
  double dt = std::min(64.0, now_ - calibLast);  // clamp after a stall
  calibLast = now_;
  if (dt >= 1 && dpadPtrs.size() == 1 && !dpadOrder.empty()) {
    auto it = dpadPtrs.find(dpadOrder.front());
    if (it != dpadPtrs.end()) {
      std::shared_ptr<DpadPtr> st = it->second;
      if (st->lastT == st->seenT) {
        // No pointer event since the last tick: the finger is genuinely still.
        st->speed = lerp(st->speed, 0, 0.35);
        st->accDX = 0;
        st->accDY = 0;
      }
      st->seenT = st->lastT;
      st->heldTicks += 1;
      readDirs(*st);
      calibrate(*st, dt);
      st->accDX = 0;
      st->accDY = 0;
    }
  }
  calibRaf = requestAnimationFrame([this](double t) { calibTick(t); });
}

double TouchGamepad::padRadiusForCalib() const { return padBox().w / 2; }

void TouchGamepad::calibrate(DpadPtr& st, double dt) {
  PadBox box = padBox();
  Point v = toElem(st.lastX - box.cx, st.lastY - box.cy);
  double vx = v.x, vy = v.y;
  double k = clamp((HOME_LEAK * dt) / 1000, 0, 0.5);

  if (st.calibDir && st.dir8 != st.calibDir) {
    // The direction just changed: relax the home partway toward the estimated rest point.
    auto est = restEstimate();
    Point rest = est ? *est : Point{};
    setHome(lerp(rest.x, ox, RELAX_ON_TURN), lerp(rest.y, oy, RELAX_ON_TURN));
  } else if (st.dir8 && st.speed < SPEED_STILL * 2) {
    Dir8 d = *st.dir8;
    // Any held direction: cancel the PERPENDICULAR part of the finger vector, and pull the origin
    // along the axis only by what exceeds the learned reach.
    double cRad = c(d) * kPi / 180;
    double dxu = std::cos(cRad);
    double dyu = std::sin(cRad);
    double dot = vx * dxu + vy * dyu;
    double perpX = vx - dot * dxu;
    double perpY = vy - dot * dyu;
    double radiusNow = padRadiusForCalib();
    double learned = tdW >= TD_MIN_W ? tdA / tdW : radiusNow * REACH_DEFAULT;
    double target = clamp(learned, radiusNow * R_TARGET, radiusNow * R_MAX_TARGET);
    double excess = std::max(0.0, dot - target);
    setHome(ox + (perpX + excess * dxu) * k * gainX,
            oy + (perpY + excess * dyu) * k * gainY);
  }
  st.calibDir = st.dir8;

  // Horizontal play must stay horizontal even mid-slide: with clearly horizontal motion and nothing
  // vertical intended, ease the VERTICAL home toward the thumb at any speed.
  const std::set<BBButton>& dirs = st.dirs;
  bool noVertical = !contains(dirs, BBButton::up) && !contains(dirs, BBButton::down) &&
                    (dirs.empty() || contains(dirs, BBButton::left) || contains(dirs, BBButton::right));
  Point acc = toElem(st.accDX, st.accDY);
  if (noVertical && std::abs(acc.x) > std::abs(acc.y) * 1.4 + 0.5) {
    double kv = clamp((HOME_LEAK * gainY * dt) / 1000, 0, 0.5);
    setHome(ox, oy + vy * kv);
  }
}

void TouchGamepad::onDpadUp(const PointerEvent& e) {
  auto it = dpadPtrs.find(e.id);
  if (it == dpadPtrs.end()) return;
  std::shared_ptr<DpadPtr> st = it->second;
  applyDpad(*st, NO_DIRS);
  removeDpadPtr(e.id);
  // Last finger up -> ease (don't snap) the pad back to its natural home.
  if (dpadPtrs.empty()) startHomeReturn();
}

void TouchGamepad::removeDpadPtr(int id) {
  dpadPtrs.erase(id);
  dpadOrder.erase(std::remove(dpadOrder.begin(), dpadOrder.end(), id), dpadOrder.end());
}

// ---- face / system buttons ----

std::set<BBButton> TouchGamepad::systemButtonsAt(double x, double y) const {
  const Rect& s = geometry.select;
  const Rect& t = geometry.start;
  double sx = s.midX(), sy = s.midY();
  double tx = t.midX(), ty = t.midY();
  double dSel = std::hypot(x - sx, y - sy);
  double dSta = std::hypot(x - tx, y - ty);
  double thr = std::hypot(sx - tx, sy - ty) * 0.66;
  std::set<BBButton> set;
  if (dSel < thr) set.insert(BBButton::select);
  if (dSta < thr) set.insert(BBButton::start);
  if (set.empty()) set.insert(dSel < dSta ? BBButton::select : BBButton::start);
  return set;
}

void TouchGamepad::applySystem(int id, const std::set<BBButton>& nextIn) {
  std::set<BBButton> next = nextIn;
  auto it = sysPointers.find(id);
  std::set<BBButton> prev = it == sysPointers.end() ? std::set<BBButton>{} : it->second;
  for (BBButton b : prev)
    if (!contains(next, b)) setBtn(b, false);
  for (BBButton b : next)
    if (!contains(prev, b)) setBtn(b, true);
  sysPointers[id] = next;
}

void TouchGamepad::onSysDown(const PointerEvent& e) {
  hapticArmed = true;
  releasePointer(e.id);
  applySystem(e.id, systemButtonsAt(e.x, e.y));
}

void TouchGamepad::onSysMove(const PointerEvent& e) {
  if (sysPointers.count(e.id) == 0) return;
  hapticArmed = true;
  applySystem(e.id, systemButtonsAt(e.x, e.y));
}

void TouchGamepad::onSysUp(const PointerEvent& e) {
  if (sysPointers.count(e.id) == 0) return;
  applySystem(e.id, {});
  sysPointers.erase(e.id);
}

// ---- A/B shared pad ----

TouchGamepad::FaceGeom TouchGamepad::faceGeometry() const {
  double ax = geometry.a.x, bx = geometry.b.x;
  double ay = geometry.a.y + pressShift(BBButton::a);
  double by = geometry.b.y + pressShift(BBButton::b);
  return FaceGeom{ax, ay, bx, by, std::hypot(ax - bx, ay - by)};
}

double TouchGamepad::pressShift(BBButton btn) const {
  double target = isActive(btn) ? geometry.pressOffset : 0;
  double dur = geometry.pressTransitionMs;
  auto it = pressAnim.find(btn);
  if (!(dur > 0) || it == pressAnim.end()) return target;
  const PressAnim& anim = it->second;
  double p = (now - anim.t0) / dur;
  if (p >= 1) return target;
  if (p <= 0) return anim.from;
  return anim.from + (target - anim.from) * cssEase(p);
}

double TouchGamepad::cssEase(double x) {
  const double p1x = 0.25, p1y = 0.1, p2x = 0.25, p2y = 1;
  auto bx = [&](double t) { return 3 * (1 - t) * (1 - t) * t * p1x + 3 * (1 - t) * t * t * p2x + t * t * t; };
  auto by = [&](double t) { return 3 * (1 - t) * (1 - t) * t * p1y + 3 * (1 - t) * t * t * p2y + t * t * t; };
  double lo = 0, hi = 1, t = x;
  for (int i = 0; i < 30; i++) {
    t = (lo + hi) / 2;
    if (bx(t) < x) lo = t; else hi = t;
  }
  return by(t);
}

std::optional<BBButton> TouchGamepad::faceSoloHeldByOther(int id) const {
  std::optional<BBButton> found;
  for (int pid : facePointerOrder) {
    if (pid == id) continue;
    auto it = facePointers.find(pid);
    if (it == facePointers.end() || it->second.size() != 1) continue;
    if (found) return std::nullopt;  // more than one committed hold: no inference
    found = *it->second.begin();
  }
  return found;
}

std::set<BBButton> TouchGamepad::faceButtonsAt(double x, double y, int id) {
  FaceGeom g = faceGeometry();

  // 1. Latched tap. Breaking on travel is what lets the latch ignore position at touchdown.
  auto lt = faceLatch.find(id);
  if (lt != faceLatch.end()) {
    FaceLatch latch = lt->second;
    if (std::hypot(x - latch.x, y - latch.y) < g.sep * FACE_LATCH_TRAVEL) return {latch.btn};
    faceLatch.erase(id);
  }

  // 2. Geometry.
  double dA = std::hypot(x - g.ax, y - g.ay);
  double dB = std::hypot(x - g.bx, y - g.by);
  double thr = g.sep * FACE_THR;
  std::optional<std::set<BBButton>> mine;
  auto fp = facePointers.find(id);
  if (fp != facePointers.end()) mine = fp->second;
  auto hyst = [&](BBButton own) -> double { return (mine && contains(*mine, own)) ? FACE_HYST : 1; };

  std::set<BBButton> set;
  if (dA < thr * hyst(BBButton::a)) set.insert(BBButton::a);
  if (dB < thr * hyst(BBButton::b)) set.insert(BBButton::b);
  if (set.empty()) set.insert(dA < dB ? BBButton::a : BBButton::b);  // nearest fallback
  return rollTap(id, set, mine, x, y, g, now);
}

std::set<BBButton> TouchGamepad::rollTap(int id, const std::set<BBButton>& setIn, const std::optional<std::set<BBButton>>& mine,
                                         double x, double y, const FaceGeom& g, double now_) {
  std::set<BBButton> set = setIn;
  auto other = [](BBButton btn) -> BBButton { return btn == BBButton::a ? BBButton::b : BBButton::a; };
  std::shared_ptr<FaceRoll> roll;
  auto rt = faceRoll.find(id);
  if (rt != faceRoll.end()) roll = rt->second;
  if (!roll) {
    // Seed at touchdown (mine is empty then) so the landing point anchors the travel. Landing in the
    // overlap takes the nearer button as the base with the other on.
    const std::set<BBButton>& held = (mine && !mine->empty()) ? *mine : set;
    if (held.size() == 1) {
      roll = std::make_shared<FaceRoll>(*held.begin(), false);
    } else if (held.size() == 2) {
      bool nearA = std::hypot(x - g.ax, y - g.ay) <= std::hypot(x - g.bx, y - g.by);
      roll = std::make_shared<FaceRoll>(nearA ? BBButton::a : BBButton::b, true);
    } else {
      return set;
    }
    faceRoll[id] = roll;
  }
  BBButton base = roll->base, target = other(base);
  if (!contains(set, base)) {
    faceRoll.erase(id);
    return set;
  }
  double fx, fy, tx, ty;
  if (base == BBButton::a) { fx = g.ax; fy = g.ay; tx = g.bx; ty = g.by; }
  else { fx = g.bx; fy = g.by; tx = g.ax; ty = g.ay; }
  double s = ((x - fx) * (tx - fx) + (y - fy) * (ty - fy)) / (g.sep * g.sep);
  roll->hist.push_back({now_, s});
  // The newest sample older than the window is where the thumb was at its start: keep it.
  while (roll->hist.size() > 1 && now_ - roll->hist[1].t > ROLL_WINDOW_MS) roll->hist.erase(roll->hist.begin());
  roll->low = std::min(roll->low, s);
  if (!roll->on) {
    double windowLow = roll->hist.front().s;
    for (const auto& h : roll->hist) windowLow = std::min(windowLow, h.s);
    // After a release only a quick, deliberate advance presses again, never slow creep.
    bool entered = !roll->released && contains(set, target) && s - roll->low >= ROLL_RETURN;
    if (s - windowLow >= ROLL_ENGAGE || entered) {
      roll->on = true;
      roll->ext = s;
    }
  } else {
    roll->ext = std::max(roll->ext, s);  // -Infinity on a both-held seed: starts here
    if (roll->ext - s >= ROLL_RETURN) {
      roll->on = false;
      roll->released = true;
      roll->low = s;
      roll->hist = {FaceRollSample{now_, s}};
    }
  }
  roll->s = std::round(s * 1000) / 1000;
  if (roll->on) set.insert(target); else set.erase(target);
  return set;
}

void TouchGamepad::applyFace(int id, const std::set<BBButton>& nextIn) {
  std::set<BBButton> next = nextIn;
  auto it = facePointers.find(id);
  std::set<BBButton> prev = it == facePointers.end() ? std::set<BBButton>{} : it->second;
  for (BBButton b : prev)
    if (!contains(next, b)) setBtn(b, false);
  for (BBButton b : next)
    if (!contains(prev, b)) setBtn(b, true);
  facePointers[id] = next;
  if (std::find(facePointerOrder.begin(), facePointerOrder.end(), id) == facePointerOrder.end()) facePointerOrder.push_back(id);
}

void TouchGamepad::removeFacePointer(int id) {
  facePointers.erase(id);
  facePointerOrder.erase(std::remove(facePointerOrder.begin(), facePointerOrder.end(), id), facePointerOrder.end());
}

void TouchGamepad::onFaceDown(const PointerEvent& e) {
  hapticArmed = true;
  // Second finger arriving during a committed hold: it means the other button.
  if (auto held = faceSoloHeldByOther(e.id)) {
    BBButton other = *held == BBButton::a ? BBButton::b : BBButton::a;
    faceLatch[e.id] = FaceLatch{other, e.x, e.y};
  }
  applyFace(e.id, faceButtonsAt(e.x, e.y, e.id));
}

void TouchGamepad::onFaceMove(const PointerEvent& e) {
  if (facePointers.count(e.id) == 0) return;
  hapticArmed = true;
  applyFace(e.id, faceButtonsAt(e.x, e.y, e.id));
}

void TouchGamepad::onFaceUp(const PointerEvent& e) {
  if (facePointers.count(e.id) == 0) return;
  applyFace(e.id, {});
  removeFacePointer(e.id);
  faceLatch.erase(e.id);
  faceRoll.erase(e.id);
}

}  // namespace rnl::touch
