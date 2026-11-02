// The C++ touch controller (apps/desktop/src/touch, docs/design/TOUCH_ANDROID.md) against NESTERM's own,
// as the iPhone's BrickboyGoldenTests does: fixtures/touch/golden-{portrait,landscape}.json were recorded by
// running the unmodified NESTERM web/brickboy/input.mjs in a browser (390 x 844 and 844 x 390 CSS px, manual
// requestAnimationFrame / performance.now, CSS transitions off; copied from apps/ios/Tests/Fixtures on
// ios-generalize). Each scenario replays the same pointer events and display frames and must give, after
// every step, the same held buttons, the same region for every touchdown and the same home offset (ox, oy).
// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <fstream>
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
