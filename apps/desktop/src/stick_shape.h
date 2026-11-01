// A controller stick's position as an analog game input (another core's layout): the shape the
// layout's axis hints ask for (rn_session_input_layout_json: "deadzone", "square").
//  * A small round dead zone for the stick's own rest noise; outside it the travel is stretched
//    evenly, so the motion starts at its edge.
//  * square: a round stick reaches a square travel's corners (an arcade flight stick, a gun's aim):
//    pushed fully in any direction, the larger axis is at its end.
//  * deadzone: the core ignores values below it (FinalBurn Neo: a third of the travel for the Sega
//    flight sticks); each axis starts past it, so the smallest push already moves the game.
// Sticks driving only digital directions are not shaped (StickShape::active false).
// Pure: no state. Values -1..1, y up.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

namespace rnl {

struct StickShape {
  bool active = false;  // the stick drives an analog axis with a hint
  bool square = false;
  float deadzone = 0;   // the core's, 0..1
};

/// The stick's rest noise: positions within this radius are the centre.
constexpr float kStickRest = 0.06f;
/// After the round dead zone, an axis this close to 0 is 0 (pushing straight along one axis does not
/// creep the other past the core's dead zone).
constexpr float kAxisSnap = 0.02f;

inline std::pair<float, float> shapeStick(float x, float y, const StickShape& s) {
  if (!s.active) return {x, y};
  const float r = std::hypot(x, y);
  if (r <= kStickRest) return {0.f, 0.f};
  const float rr = std::min(1.f, (r - kStickRest) / (1 - kStickRest));
  float ux = x / r, uy = y / r;
  if (s.square) {
    const float m = std::max(std::fabs(ux), std::fabs(uy));
    ux /= m;
    uy /= m;
  }
  auto axis = [&](float u) {
    const float v = std::max(-1.f, std::min(1.f, u * rr));
    if (std::fabs(v) < kAxisSnap) return 0.f;
    return std::copysign(s.deadzone + (1 - s.deadzone) * std::fabs(v), v);
  };
  return {axis(ux), axis(uy)};
}

}  // namespace rnl
