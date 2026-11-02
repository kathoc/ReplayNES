// The C++ touch controller (apps/desktop/src/touch, docs/design/TOUCH_ANDROID.md) against NESTERM's own,
// as the iPhone's BrickboyGoldenTests does: fixtures/touch/golden-{portrait,landscape}.json were recorded by
// running the unmodified NESTERM web/brickboy/input.mjs in a browser (390 x 844 and 844 x 390 CSS px, manual
// requestAnimationFrame / performance.now, CSS transitions off; copied from apps/ios/Tests/Fixtures on
// ios-generalize). Each scenario replays the same pointer events and display frames and must give, after
// every step, the same held buttons, the same region for every touchdown and the same home offset (ox, oy).
// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../../../engine/src/util/Json.h"
#include "support/rn_test.h"
#include "touch/touch_gamepad.h"

using rnl::touch::BBButton;
using rnl::touch::TouchGamepad;

namespace {

rn::Json loadGolden(const char* name) {
  std::ifstream in(std::string(RNL_TOUCH_FIXTURES) + "/" + name);
  std::stringstream text;
  text << in.rdbuf();
  rn::Json j;
  std::string err;
  REQUIRE(rn::Json::parse(text.str(), j, &err));
  return j;
}

rnl::touch::Rect rect(const rn::Json& a) { return {a.at(0).asDouble(), a.at(1).asDouble(), a.at(2).asDouble(), a.at(3).asDouble()}; }

TouchGamepad::Geometry geometry(const rn::Json& g) {
  TouchGamepad::Geometry o;
  o.viewport = {g["viewport"].at(0).asDouble(), g["viewport"].at(1).asDouble()};
  o.frame = rect(g["frame"]);
  o.dpad = rect(g["dpad"]);
  if (g.has("dpadClip") && !g["dpadClip"].isNull()) o.dpadClip = rect(g["dpadClip"]);
  o.face = rect(g["face"]);
  o.a = {g["a"].at(0).asDouble(), g["a"].at(1).asDouble()};
  o.b = {g["b"].at(0).asDouble(), g["b"].at(1).asDouble()};
  if (g.has("system") && !g["system"].isNull()) o.system = rect(g["system"]);
  o.select = rect(g["select"]);
  o.start = rect(g["start"]);
  // The recording ran with CSS transitions off: the 3 px press offset applies at once.
  o.pressTransitionMs = 0;
  return o;
}

const char* regionName(TouchGamepad::Region r) {
  return r == TouchGamepad::Region::dpad ? "dpad" : r == TouchGamepad::Region::face ? "face" : "system";
}

// Steps that differ from the recording (one divergence cascades: stop after a few).
int replay(const rn::Json& s, const std::string& label) {
  TouchGamepad pad([](BBButton, bool) {});
  pad.geometry = geometry(s["geometry"]);
  pad.setHomeGain(0.3, 1);  // controller.mjs applyGain defaults (recentre-x 0.3, recentre-y 1)
  const rn::Json& ops = s["ops"];
  const rn::Json& trace = s["trace"];
  REQUIRE_EQ(ops.size(), trace.size());
  int mismatches = 0;
  for (size_t i = 0; i < ops.size() && mismatches <= 5; ++i) {
    const rn::Json& op = ops.at(i);
    const rn::Json& want = trace.at(i);
    const std::string kind = op["op"].asString();
    bool down = false;
    std::optional<TouchGamepad::Region> region;
    if (kind == "frame") {
      pad.animationFrame(op["t"].asDouble());
    } else if (kind == "ways") {
      pad.setWays(int(op["n"].asInt()));
    } else {
      rnl::touch::PointerEvent e;
      e.id = int(op["id"].asInt());
      e.x = op["x"].asDouble();
      e.y = op["y"].asDouble();
      e.t = op["t"].asDouble();
      if (kind == "down") {
        down = true;
        region = pad.pointerDown(e);
      } else if (kind == "move") {
        pad.pointerMove(e);
      } else {
        pad.pointerUp(e);  // "up" and "cancel" (input.mjs ends both the same way)
      }
    }
    int mask = 0;
    for (int b = 0; b < 8; ++b)
      if (pad.isActive(BBButton(b))) mask |= 1 << b;
    const std::string where = label + " " + s["name"].asString() + " step " + std::to_string(i) + " (" + kind + ")";
    bool ok = mask == int(want["mask"].asInt());
    // 1e-4 px: Chrome's getBoundingClientRect of the translated pad carries single-precision rounding.
    ok = ok && std::fabs(pad.ox - want["ox"].asDouble()) <= 1e-4 && std::fabs(pad.oy - want["oy"].asDouble()) <= 1e-4;
    if (down) {
      const std::string got = region ? regionName(*region) : "";
      const std::string expected = want.has("region") && want["region"].isString() ? want["region"].asString() : "";
      ok = ok && got == expected;
    }
    if (!ok) {
      ++mismatches;
      std::fprintf(stderr, "touch golden: %s: mask %d (want %lld), ox %.6f (want %.6f), oy %.6f (want %.6f)\n",
                   where.c_str(), mask, (long long)want["mask"].asInt(), pad.ox, want["ox"].asDouble(), pad.oy,
                   want["oy"].asDouble());
    }
  }
  return mismatches;
}

void replayAll(const char* file) {
  const rn::Json g = loadGolden(file);
  const rn::Json& scenarios = g["scenarios"];
  CHECK_EQ(scenarios.size(), size_t(8));
  for (size_t i = 0; i < scenarios.size(); ++i) CHECK_EQ(replay(scenarios.at(i), g["label"].asString()), 0);
}

}  // namespace

TEST_CASE("touch controller: portrait traces match NESTERM's input.mjs") { replayAll("golden-portrait.json"); }

TEST_CASE("touch controller: landscape traces match NESTERM's input.mjs") { replayAll("golden-landscape.json"); }

// ---- The layout: the rules of the iPhone's TouchLayoutTests (apps/ios/Tests/TouchControlsTests.swift),
// on the iPhone's screens and two Android ones (a landscape handheld like the Retroid Pocket 5: 1920 x 1080
// px at density 2.625; a common phone in portrait), in points.

#include <set>
#include <vector>

#include "touch/touch_layout.h"

namespace {

using rnl::touch::Point;
using rnl::touch::Rect;
using rnl::touch::Size;
using rnl::touch::TouchBindings;
using rnl::touch::TouchButton;
using rnl::touch::TouchExtra;
using rnl::touch::TouchInsets;
using rnl::touch::TouchLayout;

struct Device {
  const char* name;
  Size size;
  TouchInsets safe;
};
const Device kDevices[] = {
    {"iPhone 17", {402, 874}, {62, 0, 34, 0}},
    {"iPhone 17 landscape", {874, 402}, {0, 62, 21, 62}},
    {"iPhone 17 Pro Max", {440, 956}, {62, 0, 34, 0}},
    {"iPhone 17 Pro Max landscape", {956, 440}, {0, 62, 21, 62}},
    {"iPhone SE", {375, 667}, {20, 0, 0, 0}},
    {"iPhone SE landscape", {667, 375}, {}},
    {"390 x 844", {390, 844}, {}},
    {"844 x 390", {844, 390}, {}},
    {"Android handheld landscape", {731.43, 411.43}, {}},
    {"Android phone", {412, 915}, {24, 0, 48, 0}},
};

// CGRect.intersects / contains(rect), as the iPhone's tests use them.
bool intersects(const Rect& a, const Rect& b) {
  return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
bool containsRect(const Rect& outer, const Rect& r) {
  return r.x >= outer.x - 1e-9 && r.y >= outer.y - 1e-9 && r.x + r.w <= outer.x + outer.w + 1e-9 && r.y + r.h <= outer.y + outer.h + 1e-9;
}
Rect disc(Point c, double d) { return {c.x - d / 2, c.y - d / 2, d, d}; }

TouchLayout::Options css() {
  TouchLayout::Options o;
  o.showAABB = false;
  return o;
}

}  // namespace

TEST_CASE("touch layout: everything on screen and apart, the pills' touch areas 44 pt") {
  for (const TouchLayout::Options& o : {TouchLayout::Options(), css()}) {
    for (const Device& d : kDevices) {
      const TouchLayout l = TouchLayout::make(d.size, d.safe, o);
      const Rect screen{0, 0, d.size.width, d.size.height};
      std::vector<std::pair<std::string, Rect>> boxes = {{"dpad", l.pad.dpad}, {"face", l.pad.face}, {"select", l.select}, {"start", l.start}};
      std::vector<TouchExtra> pills, rounds;
      for (const TouchExtra& e : l.extras) (e.style.type == TouchExtra::Style::round ? rounds : pills).push_back(e);
      for (const TouchExtra& p : pills) boxes.push_back({"pill", p.frame});
      for (const TouchExtra& p : pills) {
        const Rect h = p.hitFrame();
        CHECK(h.w >= 44 - 0.01);
        CHECK(h.h >= 44 - 0.01);
        if (!containsRect(screen, h))
          std::fprintf(stderr, "touch layout: %s touch area %.2f,%.2f %.2fx%.2f off %.0fx%.0f (%s)\n", rnl::touch::rawValue(p.kind.btn), h.x,
                       h.y, h.w, h.h, d.size.width, d.size.height, d.name);
        CHECK(containsRect(screen, h));
        for (const Rect& r : {l.pad.dpad, l.select, l.start, disc(l.pad.a, l.faceButton), disc(l.pad.b, l.faceButton)}) CHECK_FALSE(intersects(h, r));
        for (const TouchExtra& q : pills)
          if (!(q.kind == p.kind)) CHECK_FALSE(intersects(h, q.hitFrame()));
      }
      for (const auto& [n, r] : boxes) CHECK(containsRect(screen, r));
      for (const TouchExtra& r : rounds) CHECK(containsRect(screen, r.frame));
      for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j)
          if (intersects(boxes[i].second, boxes[j].second)) {
            std::fprintf(stderr, "touch layout: %s overlaps %s (%s)\n", boxes[i].first.c_str(), boxes[j].first.c_str(), d.name);
            CHECK(false);
          }
      for (const TouchExtra& r : rounds)
        for (const auto& [n, b] : boxes)
          if (n != "face") CHECK_FALSE(intersects(r.frame, b));
      for (TouchButton b : {TouchButton::slow, TouchButton::pause, TouchButton::menu}) CHECK(l.extra(b).has_value());
      for (const TouchExtra& e : l.extras) CHECK_FALSE(e.isTransport());
      CHECK_EQ(l.extra(TouchButton::turboA).has_value(), o.showAABB);
      CHECK_EQ(l.extra(TouchButton::turboB).has_value(), o.showAABB);
    }
  }
}

TEST_CASE("touch layout: the D-pad 10 % larger, on the A / B midpoint; its size setting") {
  for (const TouchLayout::Options& o : {TouchLayout::Options(), css()}) {
    for (const Device& d : kDevices) {
      const TouchLayout l = TouchLayout::make(d.size, d.safe, o);
      CHECK(std::fabs(l.pad.dpad.midY() - (l.pad.a.y + l.pad.b.y) / 2) < 0.01);
      const double size = l.landscape ? o.dpadSize : std::min(o.dpadSize, std::max(112.0, l.size.height * 0.24));
      CHECK_EQ(l.pad.dpad.w, std::round(size * 1.1));
      CHECK_EQ(l.pad.dpad.w, l.pad.dpad.h);
      if (l.pad.dpadClip) CHECK(containsRect(*l.pad.dpadClip, l.pad.dpad));
    }
  }
  TouchLayout::Options big;
  big.dpadSize = 210;
  CHECK_EQ(TouchLayout::make({844, 390}, {}, big).pad.dpad.w, std::round(210 * 1.1));
  CHECK(std::fabs(TouchLayout::make({320, 568}, {}, big).pad.dpad.w - std::round(568 * 0.24 * 1.1)) < 0.01);
}

TEST_CASE("touch layout: the round discs follow the game's buttons") {
  struct Case {
    int buttons;
    bool aabb;
    std::vector<TouchButton> want;
  };
  const Case cases[] = {{2, true, {TouchButton::turboB, TouchButton::turboA}}, {2, false, {}},
                        {3, true, {TouchButton::turboB, TouchButton::c}},      {3, false, {TouchButton::c}},
                        {4, true, {TouchButton::d, TouchButton::c}},           {4, false, {TouchButton::d, TouchButton::c}}};
  for (const Case& c : cases) {
    TouchLayout::Options o;
    o.buttons = c.buttons;
    o.showAABB = c.aabb;
    for (const Device& d : kDevices) {
      std::vector<TouchButton> got;
      for (const TouchExtra& e : TouchLayout::make(d.size, d.safe, o).extras)
        if (e.style.type == TouchExtra::Style::round) got.push_back(e.kind.btn);
      CHECK(got == c.want);
    }
  }
  const std::string cId = rnl::touch::inputID(TouchButton::c).value_or("");
  CHECK_EQ(cId, std::string("touch:c"));
}

TEST_CASE("touch layout: 8-WAY and MENU places; landscape picture between the controls") {
  for (const Device& d : kDevices) {
    const TouchLayout l = TouchLayout::make(d.size, d.safe, TouchLayout::Options());
    std::optional<Rect> ways;
    for (const TouchExtra& e : l.extras)
      if (e.kind.type == TouchExtra::Kind::ways) ways = e.frame;
    REQUIRE(ways.has_value());
    const Rect menu = l.extra(TouchButton::menu)->frame;
    if (l.landscape) {
      CHECK(ways->maxX() < l.gameArea.minX());
      CHECK(menu.minX() > l.gameArea.maxX());
      CHECK(std::fabs(l.gameArea.midX() - l.size.width / 2) < 0.01);
      std::vector<Rect> visible = {l.pad.dpad, l.select, l.start, disc(l.pad.a, l.faceButton), disc(l.pad.b, l.faceButton)};
      for (const TouchExtra& e : l.extras) visible.push_back(e.frame);
      for (const Rect& r : visible) CHECK_FALSE(intersects(r, l.gameArea));
      // The D-pad's home cannot follow the thumb over the picture.
      TouchGamepad pad([](BBButton, bool) {});
      pad.geometry = l.pad;
      pad.setHome(1000, 0);
      CHECK(l.pad.dpad.maxX() + pad.ox <= l.gameArea.minX() + 0.01);
    } else {
      CHECK(std::fabs(ways->midY() - l.select.midY()) < 0.01);
      CHECK(std::fabs(menu.midY() - l.start.midY()) < 0.01);
      CHECK(ways->maxX() < l.select.minX());
      CHECK(menu.minX() > l.start.maxX());
    }
  }
}

TEST_CASE("touch bindings: every game button once; an arcade layout's buttons") {
  std::set<std::string> ids;
  for (TouchButton b : rnl::touch::TouchButton_allCases)
    if (b != TouchButton::c && b != TouchButton::d)
      if (auto id = rnl::touch::inputID(b)) ids.insert(*id);
  std::set<std::string> bound;
  for (const auto& t : TouchBindings::defaults()) bound.insert(t.input);
  CHECK(bound == ids);
  CHECK_EQ(TouchBindings::defaults().size(), ids.size());
  CHECK_FALSE(rnl::touch::inputID(TouchButton::menu).has_value());
  CHECK_FALSE(rnl::touch::inputID(TouchButton::slow).has_value());
  CHECK_FALSE(rnl::touch::inputID(TouchButton::pause).has_value());
  const std::set<std::string> actions = {"p1.up", "p1.down", "p1.left", "p1.right", "p1.b1", "p1.b1.turbo", "p1.b2",
                                         "p1.b2.turbo", "p1.b3", "p1.b3.turbo", "p1.coin", "p1.start"};
  std::map<std::string, std::string> m;
  for (const auto& t : TouchBindings::forLayout(actions)) m.emplace(t.input, t.action);
  CHECK_EQ(m["touch:b"], std::string("p1.b1"));
  CHECK_EQ(m["touch:a"], std::string("p1.b2"));
  CHECK_EQ(m["touch:c"], std::string("p1.b3"));
  CHECK_EQ(m["touch:turboB"], std::string("p1.b1.turbo"));
  CHECK_EQ(m["touch:turboA"], std::string("p1.b2.turbo"));
  CHECK_EQ(m.count("touch:d"), size_t(0));
  CHECK_EQ(m["touch:select"], std::string("p1.coin"));
}
