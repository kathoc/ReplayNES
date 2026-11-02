// Where the picture and the on-screen controls go: see touch_layout.h.
// Port of apps/ios/Sources/Play/Brickboy/TouchLayout.swift (ios-generalize), function by function.
// Original: NESTERM web/styles.css (Brickboy).
//
// Portions derived from Brickboy / NESTERM web/styles.css, Copyright 2026 Katsuya Hokamura,
// Apache-2.0. The ReplayNES extras and TouchButton / TouchBindings are ReplayNES's own.
// SPDX-License-Identifier: Apache-2.0
#include "touch/touch_layout.h"

#include <algorithm>
#include <cctype>
#include <cmath>

// Swift does not fuse a * b + c; keep the same rounding as the iPhone's reference data.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace rnl::touch {

namespace {

constexpr double kPi = 3.14159265358979323846;

using Kind = TouchExtra::Kind;
using Style = TouchExtra::Style;

Kind buttonKind(TouchButton b) { return Kind::makeButton(b); }

// Swift String.count for the labels here: code points, a variation selector (U+FE0E / U+FE0F)
// belongs to the previous character.
int characterCount(const std::string& s) {
  int n = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char ch = static_cast<unsigned char>(s[i]);
    size_t len = ch < 0x80 ? 1 : (ch >> 5) == 0x6 ? 2 : (ch >> 4) == 0xE ? 3 : 4;
    bool vs = len == 3 && s.compare(i, 3, "\xEF\xB8\x8E") == 0;
    bool vs2 = len == 3 && s.compare(i, 3, "\xEF\xB8\x8F") == 0;
    if (!vs && !vs2) n++;
    i += len;
  }
  return n;
}

}  // namespace

// MARK: TouchButton

const std::vector<TouchButton> TouchButton_allCases = {
    TouchButton::up,     TouchButton::down,   TouchButton::left,     TouchButton::right,     TouchButton::a,
    TouchButton::b,      TouchButton::turboA, TouchButton::turboB,   TouchButton::select,    TouchButton::start,
    TouchButton::l2,     TouchButton::r2,     TouchButton::menu,     TouchButton::stepBack,  TouchButton::stepForward,
    TouchButton::slow,   TouchButton::pause,  TouchButton::c,        TouchButton::d};

const char* rawValue(TouchButton b) {
  switch (b) {
    case TouchButton::up: return "up";
    case TouchButton::down: return "down";
    case TouchButton::left: return "left";
    case TouchButton::right: return "right";
    case TouchButton::a: return "a";
    case TouchButton::b: return "b";
    case TouchButton::turboA: return "turboA";
    case TouchButton::turboB: return "turboB";
    case TouchButton::select: return "select";
    case TouchButton::start: return "start";
    case TouchButton::l2: return "l2";
    case TouchButton::r2: return "r2";
    case TouchButton::menu: return "menu";
    case TouchButton::stepBack: return "stepBack";
    case TouchButton::stepForward: return "stepForward";
    case TouchButton::slow: return "slow";
    case TouchButton::pause: return "pause";
    case TouchButton::c: return "c";
    case TouchButton::d: return "d";
  }
  return "";
}

std::optional<std::string> inputID(TouchButton b) {
  switch (b) {
    case TouchButton::up:
    case TouchButton::down:
    case TouchButton::left:
    case TouchButton::right: return std::string("touch:dpad.") + rawValue(b);
    case TouchButton::menu:
    case TouchButton::stepBack:
    case TouchButton::stepForward:
    case TouchButton::slow:
    case TouchButton::pause: return std::nullopt;
    default: return std::string("touch:") + rawValue(b);
  }
}

bool isDirection(TouchButton b) {
  return b == TouchButton::up || b == TouchButton::down || b == TouchButton::left || b == TouchButton::right;
}

TouchButton touchButton(BBButton b) {
  switch (b) {
    case BBButton::right: return TouchButton::right;
    case BBButton::left: return TouchButton::left;
    case BBButton::up: return TouchButton::up;
    case BBButton::down: return TouchButton::down;
    case BBButton::a: return TouchButton::a;
    case BBButton::b: return TouchButton::b;
    case BBButton::select: return TouchButton::select;
    case BBButton::start: return TouchButton::start;
  }
  return TouchButton::up;
}

// MARK: TouchBindings

const std::vector<TouchBinding>& TouchBindings::defaults() {
  static const std::vector<TouchBinding> v = {
      {"touch:dpad.up", "p1.up"},         {"touch:dpad.down", "p1.down"},
      {"touch:dpad.left", "p1.left"},     {"touch:dpad.right", "p1.right"},
      {"touch:a", "p1.a"},                {"touch:b", "p1.b"},
      {"touch:turboA", "p1.turbo_a"},     {"touch:turboB", "p1.turbo_b"},
      {"touch:select", "p1.select"},      {"touch:start", "p1.start"},
      {"touch:l2", "hk.rewind"},          {"touch:r2", "hk.fast_forward"},
  };
  return v;
}

std::vector<TouchBinding> TouchBindings::forLayout(const std::set<std::string>& actions) {
  std::vector<TouchBinding> all = {
      {"touch:dpad.up", "p1.up"},     {"touch:dpad.down", "p1.down"},
      {"touch:dpad.left", "p1.left"}, {"touch:dpad.right", "p1.right"},
      {"touch:b", "p1.b1"},           {"touch:a", "p1.b2"},
      {"touch:c", "p1.b3"},           {"touch:d", "p1.b4"},
      // Turbo: BB = button 1's, AA = button 2's (InputLayout::turboOf: "<id>.turbo").
      {"touch:turboB", "p1.b1.turbo"}, {"touch:turboA", "p1.b2.turbo"},
      {"touch:select", "p1.coin"},    {"touch:start", "p1.start"},
  };
  std::vector<TouchBinding> out;
  for (const auto& b : all)
    if (actions.count(b.action) != 0) out.push_back(b);
  out.push_back({"touch:l2", "hk.rewind"});
  out.push_back({"touch:r2", "hk.fast_forward"});
  return out;
}

std::vector<TouchBinding> TouchBindings::missing(const std::vector<TouchBinding>& existing) {
  for (const auto& b : existing)
    if (b.input.compare(0, 6, "touch:") == 0) return {};
  return defaults();
}

// MARK: TouchExtra

bool TouchExtra::hit(Point p) const {
  if (style.type == Style::round) {
    return std::hypot(p.x - frame.midX(), p.y - frame.midY()) <= frame.width() / 2 + TouchLayout::turboSlop;
  }
  return hitFrame().contains(p);
}

Rect TouchExtra::hitFrame() const {
  if (style.type == Style::round) return frame.insetBy(-TouchLayout::turboSlop, -TouchLayout::turboSlop);
  double m = TouchLayout::minTarget;
  return frame.insetBy(-std::max(0.0, (m - frame.width()) / 2), -std::max(0.0, (m - frame.height()) / 2));
}

bool TouchExtra::isTransport() const {
  if (kind.type != Kind::button) return false;
  const auto& o = TouchLayout::transportOrder();
  return std::find(o.begin(), o.end(), kind.btn) != o.end();
}

// MARK: TouchLayout

std::optional<TouchExtra> TouchLayout::extra(TouchButton b) const {
  for (const auto& e : extras)
    if (e.kind == buttonKind(b)) return e;
  return std::nullopt;
}

double TouchLayout::islandMidY(double safeTop) { return safeTop > 20 ? std::round(safeTop / 2) : 22; }

const std::vector<TouchButton>& TouchLayout::transportOrder() {
  static const std::vector<TouchButton> v = {TouchButton::l2, TouchButton::stepBack, TouchButton::stepForward, TouchButton::r2};
  return v;
}

TouchLayout::TextMeasurer TouchLayout::textMeasurer;

TouchLayout TouchLayout::make(Size size, TouchInsets safe) { return make(size, safe, Options()); }

TouchLayout TouchLayout::make(Size size, TouchInsets safe, const Options& options) {
  // controller.mjs syncViewport: root.classList.toggle('bb-side', width > height).
  return size.width > size.height ? landscapeLayout(size, safe, options) : portraitLayout(size, safe, options);
}

// MARK: text metrics

double TouchLayout::textWidth(const std::string& s, double size, Weight weight, double tracking) {
  double w;
  if (textMeasurer) {
    w = textMeasurer(s, size, weight);
  } else {
    w = characterCount(s) * size * (weight == Weight::semibold ? 0.66 : 0.6);
  }
  return std::ceil(w * 100) / 100 + tracking * double(characterCount(s));
}

Size TouchLayout::pillSize(const std::string& label, double minHeight) {
  double w = textWidth(label, pillFont, Weight::semibold, pillTracking) + 2 * pillPadX;
  return Size{w, std::max(minHeight, pillFont * lineHeight + 2 * pillPadY)};
}

Size TouchLayout::switchSize(const std::string& label) {
  double w = textWidth(label, pillFont, Weight::semibold, switchTracking) + 2 * (switchPadX + switchBorder);
  return Size{w, pillFont * lineHeight + 2 * (switchPadY + switchBorder)};
}

std::string TouchLayout::label(const TouchExtra::Kind& k, int ways) {
  if (k.type == Kind::ways) return ways == 4 ? "4-WAY" : "8-WAY";
  switch (k.btn) {
    case TouchButton::l2: return "\xE2\x97\x80\xE2\x97\x80";            // U+25C0 U+25C0
    case TouchButton::r2: return "\xE2\x96\xB6\xE2\x96\xB6";            // U+25B6 U+25B6
    case TouchButton::menu: return "MENU";
    case TouchButton::turboA: return "AA";
    case TouchButton::turboB: return "BB";
    case TouchButton::c: return "C";
    case TouchButton::d: return "D";
    case TouchButton::stepBack: return "\xE2\x97\x80\xEF\xB8\x8E|";     // U+25C0 U+FE0E |
    case TouchButton::stepForward: return "|\xE2\x96\xB6\xEF\xB8\x8E";  // | U+25B6 U+FE0E
    default: {
      std::string s = rawValue(k.btn);
      for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      return s;
    }
  }
}

TouchExtra::Style TouchLayout::style(const TouchExtra::Kind& k) {
  if (k.type != Kind::button) return Style::makePill();
  switch (k.btn) {
    case TouchButton::l2: return Style::makeIcon("backward.fill");
    case TouchButton::r2: return Style::makeIcon("forward.fill");
    case TouchButton::stepBack: return Style::makeIcon("backward.frame.fill");
    case TouchButton::stepForward: return Style::makeIcon("forward.frame.fill");
    case TouchButton::slow: return Style::makeIcon("tortoise.fill");
    case TouchButton::pause: return Style::makeIcon("pause.fill");
    case TouchButton::turboA:
    case TouchButton::turboB:
    case TouchButton::c:
    case TouchButton::d: return Style::makeRound();
    default: return Style::makePill();
  }
}

std::vector<TouchExtra> TouchLayout::transport(double midX, double bottom, double width) {
  double g = transportGap;
  // Whole points, even (the steps are exactly half).
  double wide = std::floor(std::min(transportWide, std::max(2 * minTarget, (width - 3 * g) / 3)) / 2) * 2;
  const auto& order = transportOrder();
  std::vector<double> widths;
  for (TouchButton b : order) widths.push_back(b == TouchButton::l2 || b == TouchButton::r2 ? wide : wide / 2);
  double sum = 0;
  for (double w : widths) sum = sum + w;  // reduce(0, +)
  double x = midX - (sum + 3 * g) / 2;
  std::vector<TouchExtra> out;
  for (size_t i = 0; i < order.size(); i++) {
    Kind k = buttonKind(order[i]);
    out.push_back(TouchExtra{k, Rect{x, bottom - transportHeight, widths[i], transportHeight}, style(k)});
    x += widths[i] + g;
  }
  return out;
}

TouchLayout::TurboMetrics TouchLayout::turboMetrics(double btn) {
  double s = std::round(btn * turboScale);
  return TurboMetrics{s, (btn + s) / 2 + turboGap};
}

Vector TouchLayout::turboDirection(double btn) {
  double r = turboAngle * kPi / 180;
  return Vector{-std::sin(r), -std::cos(r)};
}

Vector TouchLayout::turboOffset(double btn, Point b, double minLeft) {
  TurboMetrics t = turboMetrics(btn);
  Vector u = turboDirection(btn);
  Vector v{u.dx * t.step, u.dy * t.step};
  if (b.x + v.dx - t.size / 2 < minLeft) {
    v.dx = std::min(0.0, minLeft + t.size / 2 - b.x);
    v.dy = -std::sqrt(std::max(0.0, t.step * t.step - v.dx * v.dx));
  }
  return v;
}

double TouchLayout::turboAbove(double btn, Vector v) {
  return (btn + faceGap) * faceTilt - v.dy + turboMetrics(btn).size / 2;
}

std::vector<TouchExtra> TouchLayout::turboExtras(Point a, Point b, double btn, Vector v, const Options& o) {
  TurboMetrics t = turboMetrics(btn);
  auto disc = [&](Point c) {
    return Rect{c.x + v.dx - t.size / 2, c.y + v.dy - t.size / 2, t.size, t.size};
  };
  // B first (the left one), as .gp-face lists its rounds. Buttons 3 / 4 take AA's / BB's place; a
  // turbo disc only with the turbo setting on.
  std::optional<TouchButton> left = o.buttons >= 4 ? std::optional<TouchButton>(TouchButton::d)
                                    : o.showAABB   ? std::optional<TouchButton>(TouchButton::turboB)
                                                   : std::nullopt;
  std::optional<TouchButton> right = o.buttons >= 3 ? std::optional<TouchButton>(TouchButton::c)
                                     : o.showAABB   ? std::optional<TouchButton>(TouchButton::turboA)
                                                    : std::nullopt;
  std::vector<TouchExtra> out;
  if (left) out.push_back(TouchExtra{buttonKind(*left), disc(b), Style::makeRound()});
  if (right) out.push_back(TouchExtra{buttonKind(*right), disc(a), Style::makeRound()});
  return out;
}

double TouchLayout::faceMid(const Rect& row, double top, double above, double below) {
  return std::min(std::max(row.midY(), top + above), row.maxY() - below);
}

TouchExtra TouchLayout::pill(const TouchExtra::Kind& k, double x, double midY, int ways) {
  Size s = switchSize(label(k, ways));
  return TouchExtra{k, Rect{x, midY - s.height / 2, s.width, s.height}, style(k)};
}

TouchLayout::FaceBox TouchLayout::face(double x, double midY, double btn, double padX, double padY) {
  double off = (btn + faceGap) * faceTilt;
  double w = 2 * padX + 2 * btn + faceGap;
  double h = btn + 2 * padY;
  Rect box{x, midY - h / 2, w, h};
  Point b{x + padX + btn / 2 + bTowardA * btn, midY + off};
  Point a{x + padX + btn + faceGap + btn / 2, midY - off};
  return FaceBox{box, a, b};
}

// MARK: portrait (#app:not(.bb-side))

TouchLayout TouchLayout::portraitLayout(Size size, TouchInsets safe, const Options& o) {
  double W = size.width, H = size.height;
  // The picture on top, full width (ReplayNES; NESTERM's top bar is not used). app.mjs resizeScreen
  // shrinks the screen row to the picture so the controls get the rest.
  double gameH = std::round(W / pictureAspect);
  // The controls stay where the NES picture's row puts them. Portrait FILL: a picture taller than
  // that row still spans the screen's width, its lower part under the controls; a wider picture
  // fits the row as before.
  double fullH = o.fill ? (o.pictureAspect ? std::round(W / std::max(*o.pictureAspect, 0.01)) : gameH) : gameH;
  Rect game{0, safe.top, W, std::max(gameH, fullH)};

  // NESTERM's sub bar (.bb-subbar) is not used: the gamepad starts right under the picture's row.
  Rect subbar{0, safe.top + gameH, W, 0};
  std::vector<TouchExtra> extras;

  // #app:not(.bb-side) > .bb-root > .bb-gamepad { flex: 1 1 auto;
  //   grid-template-rows: minmax(0, 1fr) auto; padding: 6px 12px calc(6px + safe-bottom);
  //   padding-inline: clamp(12px, 4.5vw, 24px); gap: 4px 8px; clip-path: inset(0) }
  Rect gamepad{0, subbar.maxY(), W, std::max(0.0, H - subbar.maxY())};
  double pi = std::min(std::max(12.0, 0.045 * W), 24.0);
  double contentTop = gamepad.minY() + 6;
  double contentBottom = gamepad.maxY() - (6 + safe.bottom);
  // Row 2: .gp-system { padding: 4px 0 } around the pills (min-height: 40px).
  Size sel = pillSize("SELECT", 40), sta = pillSize("START", 40);
  double systemH = std::max(sel.height, sta.height) + 8;
  Rect row1{pi, contentTop, W - 2 * pi, std::max(0.0, contentBottom - contentTop - 4 - systemH)};
  Rect row2{pi, row1.maxY() + 4, W - 2 * pi, systemH};

  // #app:not(.bb-side) .gp-face { justify-self: end;
  //   --gp-btn: clamp(48px, min(19vw, el-h * .13), 78px); padding: calc(var(--gp-off) + 4px) 4px }
  double btn = std::min(std::max(48.0, std::min(0.19 * W, 0.13 * H)), 78.0);
  double off = (btn + faceGap) * faceTilt;
  double faceW = 2 * 4 + 2 * btn + faceGap;
  double faceX = W - pi - faceW;
  // #app:not(.bb-side) .gp-dpad { width: min(var(--bb-dpad, 144px), max(112px, el-h * .24)) } - here
  // x 1.1 and centred on the A/B midpoint (ReplayNES).
  double d = std::round(std::min(o.dpadSize, std::max(112.0, H * 0.24)) * dpadScale);
  // AA / BB up-left of A / B, BB kept `turboGap` clear of the D-pad (narrow screens).
  Vector tv = turboOffset(btn, Point{faceX + 4 + btn / 2 + bTowardA * btn, 0}, pi + 2 + d + turboGap);
  double below = btn / 2 + off + 4;
  double above = o.showRounds() ? turboAbove(btn, tv) : below;
  double m = faceMid(row1, row1.minY(), above, below);
  FaceBox f = face(faceX, m, btn, 4, off + 4);
  Rect dpad{pi + 2, m - d / 2, d, d};
  if (o.showRounds()) {
    auto t = turboExtras(f.a, f.b, btn, tv, o);
    extras.insert(extras.end(), t.begin(), t.end());
  }
  // Slow motion: in the top band, left of the Dynamic Island, centred on the island's height.
  double slowX = W / 2 - islandHalfWidth - islandGap - slowSize.width;
  extras.push_back(TouchExtra{buttonKind(TouchButton::slow),
                              Rect{std::max(pi, slowX), std::max(0.0, islandMidY(safe.top) - slowSize.height / 2),
                                   slowSize.width, slowSize.height},
                              style(buttonKind(TouchButton::slow))});

  // .gp-system { display: flex; justify-content: center; gap: 22px }.
  double total = sel.width + 22 + sta.width;
  double px = row2.minX() + (row2.width() - total) / 2;
  Rect select{px, row2.minY() + 4, sel.width, sel.height};
  Rect start{select.maxX() + 22, row2.minY() + 4, sta.width, sta.height};
  // 8-WAY and MENU at the ends of the SELECT / START row.
  extras.push_back(pill(Kind::makeWays(), row2.minX(), select.midY(), o.ways));
  double menuW = switchSize(label(buttonKind(TouchButton::menu), o.ways)).width;
  TouchExtra menu = pill(buttonKind(TouchButton::menu), row2.maxX() - menuW, select.midY(), o.ways);
  extras.push_back(menu);
  // Pause right above MENU, centred on it.
  double pauseSlop = std::max(0.0, (minTarget - slowSize.height) / 2);  // its touch area below the pill
  extras.push_back(TouchExtra{buttonKind(TouchButton::pause),
                              Rect{menu.frame.midX() - slowSize.width / 2,
                                   menu.hitFrame().minY() - pauseGap - pauseSlop - slowSize.height, slowSize.width,
                                   slowSize.height},
                              style(buttonKind(TouchButton::pause))});

  TouchGamepad::Geometry g;
  g.viewport = size;
  g.frame = Rect{0, 0, size.width, size.height};  // .bb-root has no padding in portrait
  g.dpad = dpad;
  g.dpadClip = gamepad;
  g.face = f.box;
  g.a = f.a;
  g.b = f.b;
  g.system = row2;
  g.select = select;
  g.start = start;
  TouchLayout l;
  l.size = size;
  l.landscape = false;
  l.gameArea = game;
  l.pad = g;
  l.faceButton = btn;
  l.select = select;
  l.start = start;
  l.extras = extras;
  double controlsTop = std::min({dpad.minY(), f.a.y - btn / 2, f.b.y - btn / 2});
  for (const auto& e : extras)
    if (e.style.type == Style::round) controlsTop = std::min(controlsTop, e.frame.minY());
  l.overlayArea = Rect{0, game.minY(), W, std::max(0.0, std::min(game.maxY(), controlsTop - overlayGap) - game.minY())};
  return l;
}

// MARK: landscape (.bb-side)

TouchLayout TouchLayout::landscapeLayout(Size size, TouchInsets safe, const Options& o) {
  double W = size.width, H = size.height;
  // .bb-side > .bb-root { padding: 0 calc(safe-right + edge-gap) 0 calc(safe-left + edge-gap);
  //   grid-template-columns: minmax(144px, 16%) 1fr minmax(144px, 16%);
  //   grid-template-rows: auto auto 1fr auto;
  //   grid-template-areas: "info screen icons" "ways screen repeat" "dpad screen face" "select screen start" }
  // (--bb-edge-gap-* resolve to 0: min(0px, inset * 1000).)
  Rect content{safe.left, 0, std::max(0.0, W - safe.left - safe.right), H};
  double col = std::max(144.0, 0.16 * content.width());
  Rect leftCol{content.minX(), 0, col, H};
  Rect rightCol{content.maxX() - col, 0, col, H};

  // Row 1 (info / icons, margin-top: safe-top): slow motion and pause, their 44 pt touch areas from
  // the top, `pauseGap` above row 2's.
  double pillH = switchSize("MENU").height;
  double row1H = std::ceil(safe.top + minTarget + pauseGap + std::max(0.0, (minTarget - pillH) / 2));
  // Row 2 (ways / repeat): the switches.
  Rect row2{0, row1H, W, pillH};
  // Row 4 (select / start): .gp-pill { align-self: end; margin-bottom: calc(8px + safe-bottom) }.
  Size sel = pillSize("SELECT", 0), sta = pillSize("START", 0);
  double row4H = std::max(sel.height, sta.height) + 8 + safe.bottom;
  Rect row3{0, row2.maxY(), W, std::max(0.0, H - row2.maxY() - row4H)};

  int ways = o.ways;
  auto centred = [&](const Kind& k, const Rect& c, double midY) {
    return pill(k, c.midX() - switchSize(label(k, ways)).width / 2, midY, ways);
  };
  // #ways-switch / #turbo-switch { justify-self: center; align-self: center }; MENU in NESTERM's
  // repeat switch place. Row 1 stays empty (the transport is under the picture).
  std::vector<TouchExtra> extras = {centred(Kind::makeWays(), leftCol, row2.midY()),
                                    centred(buttonKind(TouchButton::menu), rightCol, row2.midY())};
  // Row 1: slow motion above 8-WAY (the left, like a controller's L side), pause above MENU.
  double row1Mid = safe.top + minTarget / 2;
  const std::pair<TouchButton, Rect> row1Items[] = {{TouchButton::slow, leftCol}, {TouchButton::pause, rightCol}};
  for (const auto& it : row1Items) {
    extras.push_back(TouchExtra{buttonKind(it.first),
                                Rect{it.second.midX() - slowSize.width / 2, row1Mid - slowSize.height / 2,
                                     slowSize.width, slowSize.height},
                                style(buttonKind(it.first))});
  }

  // .gp-face { --gp-btn: min(19vw, 78px); padding: 48px 16px }; justify-self: end; align-self: center.
  double btn = std::min(0.19 * W, 78.0);
  double faceW = 2 * 16 + 2 * btn + faceGap;
  double faceX = rightCol.maxX() - faceW;
  // AA / BB up-left of A / B without narrowing the picture: B's disc edge (CSS place) is the limit;
  // the face moves right into its 16 px padding first (keeping A 8 pt from the screen edge), then
  // AA / BB turn toward straight up as far as needed.
  Vector tv{0, 0};
  if (o.showRounds()) {
    double bLeft = faceX + 16 + bTowardA * btn;
    TurboMetrics t = turboMetrics(btn);
    Vector u = turboDirection(btn);
    double protrude = std::max(0.0, -(u.dx * t.step) + t.size / 2 - btn / 2);
    faceX += std::min({protrude, 16.0, 8 + safe.right});
    tv = turboOffset(btn, Point{faceX + 16 + btn / 2 + bTowardA * btn, 0}, bLeft);
  }
  double below = btn / 2 + 48;
  double above = o.showRounds() ? turboAbove(btn, tv) : below;
  double m = faceMid(row3, row2.maxY() + 4, above, below);
  FaceBox f = face(faceX, m, btn, 16, 48);
  f.box = f.box.intersection(Rect{0, 0, W, H});
  if (o.showRounds()) {
    auto t = turboExtras(f.a, f.b, btn, tv, o);
    extras.insert(extras.end(), t.begin(), t.end());
  }

  // .bb-side > .bb-root .gp-dpad { justify-self: start; margin-inline-start: var(--bb-ctl-inset)
  //   (16px); align-self: center; width: var(--bb-dpad, 144px) } - x 1.1, on the A/B midpoint.
  double d = std::round(o.dpadSize * dpadScale);
  Rect dpad{leftCol.minX() + 16, m - d / 2, d, d};

  double pillBottom = H - 8 - safe.bottom;
  Rect select{leftCol.midX() - sel.width / 2, pillBottom - sel.height, sel.width, sel.height};
  Rect start{rightCol.midX() - sta.width / 2, pillBottom - sta.height, sta.width, sta.height};

  // The picture fits between the control columns (ReplayNES; NESTERM draws the controls over a
  // full-screen picture): symmetric about the screen's centre, `pictureMargin` clear of the nearest
  // visible control (D-pad, A / B / AA / BB discs, pills).
  double leftEdge = std::max(dpad.maxX(), select.maxX());
  for (const auto& e : extras)
    if (e.frame.midX() < W / 2) leftEdge = std::max(leftEdge, e.frame.maxX());
  double rightEdge = std::min({f.b.x - btn / 2, f.a.x - btn / 2, start.minX()});
  for (const auto& e : extras)
    if (e.frame.midX() >= W / 2) rightEdge = std::min(rightEdge, e.frame.minX());
  double half = std::max(0.0, std::min(W / 2 - leftEdge, rightEdge - W / 2) - pictureMargin);
  // No transport row: the picture has the full height between the columns.
  Rect game = o.fill ? Rect{0, 0, W, H} : Rect{W / 2 - half, 0, 2 * half, H};

  TouchGamepad::Geometry g;
  g.viewport = size;
  // The .bb-root content box (its padding is the safe area), its right side ending at the picture
  // (ReplayNES): the pad's home may follow the thumb up to the picture, not over it.
  g.frame = Rect{content.minX(), 0, std::max(dpad.maxX(), game.minX()) + EDGE_MARGIN - content.minX(), H};
  g.dpad = dpad;
  g.dpadClip = std::nullopt;  // .bb-gamepad is display: contents: no clip
  g.face = f.box;
  g.a = f.a;
  g.b = f.b;
  g.system = std::nullopt;  // .gp-system is display: contents: only the pills take touches
  g.select = select;
  g.start = start;
  TouchLayout l;
  l.size = size;
  l.landscape = true;
  l.gameArea = game;
  l.pad = g;
  l.faceButton = btn;
  l.select = select;
  l.start = start;
  l.extras = extras;
  l.overlayArea = Rect{W / 2 - half, 0, 2 * half, H};
  return l;
}

}  // namespace rnl::touch
