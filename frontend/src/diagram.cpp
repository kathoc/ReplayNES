// Geometry of the controller settings diagram (canvas RNF_DIAGRAM_CANVAS_WIDTH x HEIGHT points,
// origin top-left). Nintendo / Xbox / generic put the left stick above the D-pad on a rounded pill
// body; PlayStation has both sticks at the bottom (and a touchpad); the Steam Deck is a wide rounded
// rectangle with the screen in the middle, D-pad / face buttons at the outer top, the sticks inside
// below them and the trackpads at the bottom. No grips: a simple modern outline.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <vector>

#include "common.hpp"

namespace {

struct Layout {
  rnf_diagram_info info{};
  std::vector<rnf_diagram_element> elements;
  std::vector<rnf_diagram_decor> decor;
};

rnf_diagram_decor decor(rnf_diagram_decor_kind k, double x, double y, double w, double h, double r) {
  rnf_diagram_decor d{};
  d.kind = k;
  d.x = x;
  d.y = y;
  d.width = w;
  d.height = h;
  d.radius = r;
  return d;
}

rnf_diagram_element make(const char* element, rnf_diagram_kind kind, double cx, double cy, double w, double h,
                         rnf_diagram_side side, const char* group = nullptr) {
  rnf_diagram_element e{};
  e.element = element;
  e.kind = kind;
  e.cx = cx;
  e.cy = cy;
  e.width = w;
  e.height = h;
  e.badge_side = side;
  e.group = group;
  // Point where the assignment badge is anchored (badge grows away from the element).
  const double gap = 4;
  if (kind == RNF_DIAGRAM_STICK_CLICK) {
    // Outside the stick well, below its left/right arrow badges.
    const double r = RNF_DIAGRAM_STICK_RADIUS;
    const double dx = r + gap;
    e.badge_x = side == RNF_SIDE_LEFT ? cx - dx : cx + dx;
    e.badge_y = cy + r - 6;
    return e;
  }
  const double minX = cx - w / 2, minY = cy - h / 2;
  const double maxX = minX + w, maxY = minY + h;
  switch (side) {
    case RNF_SIDE_LEFT: e.badge_x = minX - gap; e.badge_y = cy; break;
    case RNF_SIDE_RIGHT: e.badge_x = maxX + gap; e.badge_y = cy; break;
    case RNF_SIDE_ABOVE: e.badge_x = cx; e.badge_y = minY - gap; break;
    case RNF_SIDE_BELOW: e.badge_x = cx; e.badge_y = maxY + gap; break;
  }
  return e;
}

Layout build(rnf_controller_family family) {
  Layout l;
  const bool sym = family == RNF_FAMILY_PLAYSTATION;
  const bool deck = family == RNF_FAMILY_STEAM_DECK;
  double lsx = sym ? 225 : 165, lsy = sym ? 188 : 120;
  double rsx = 335, rsy = sym ? 188 : 182;
  double dx = sym ? 160 : 225, dy = sym ? 128 : 182;
  double fx = sym ? 400 : 395, fy = sym ? 125 : 120;
  double shoulderL = 150, shoulderR = 410;
  if (deck) {
    lsx = 156; lsy = 162; rsx = 404; rsy = 162;
    dx = 72; dy = 126; fx = 486; fy = 126;
    shoulderL = 130; shoulderR = 430;
  }
  const double R = RNF_DIAGRAM_STICK_RADIUS;
  auto& i = l.info;
  i.left_stick_x = lsx; i.left_stick_y = lsy;
  i.right_stick_x = rsx; i.right_stick_y = rsy;
  i.stick_radius = R;
  i.dpad_x = dx; i.dpad_y = dy;
  i.face_x = fx; i.face_y = fy;
  i.has_touchpad = sym;
  if (sym) { i.touchpad_x = 280; i.touchpad_y = 92; i.touchpad_width = 64; i.touchpad_height = 40; }
  // The outline: the body, then the screen / pads.
  if (deck) {
    l.decor.push_back(decor(RNF_DECOR_BODY, 16, 60, 528, 190, 44));
    l.decor.push_back(decor(RNF_DECOR_SCREEN, 194, 101, 172, 108, 6));
    l.decor.push_back(decor(RNF_DECOR_PAD, 120, 212, 60, 32, 8));
    l.decor.push_back(decor(RNF_DECOR_PAD, 380, 212, 60, 32, 8));
  } else {
    l.decor.push_back(decor(RNF_DECOR_BODY, 72, 58, 416, 178, 80));
    if (sym) l.decor.push_back(decor(RNF_DECOR_PAD, i.touchpad_x - i.touchpad_width / 2, i.touchpad_y - i.touchpad_height / 2,
                                     i.touchpad_width, i.touchpad_height, 8));
  }

  auto& e = l.elements;
  // Triggers / shoulders.
  e.push_back(make("leftTrigger", RNF_DIAGRAM_TRIGGER, shoulderL, 18, 88, 22, RNF_SIDE_LEFT));
  e.push_back(make("rightTrigger", RNF_DIAGRAM_TRIGGER, shoulderR, 18, 88, 22, RNF_SIDE_RIGHT));
  e.push_back(make("leftShoulder", RNF_DIAGRAM_SHOULDER, shoulderL, 46, 108, 18, RNF_SIDE_LEFT));
  e.push_back(make("rightShoulder", RNF_DIAGRAM_SHOULDER, shoulderR, 46, 108, 18, RNF_SIDE_RIGHT));
  // Face buttons (diamond).
  const double s = RNF_DIAGRAM_FACE_SPACING, r = RNF_DIAGRAM_FACE_RADIUS;
  e.push_back(make("face.north", RNF_DIAGRAM_FACE, fx, fy - s, r * 2, r * 2, RNF_SIDE_ABOVE));
  e.push_back(make("face.south", RNF_DIAGRAM_FACE, fx, fy + s, r * 2, r * 2, RNF_SIDE_BELOW));
  e.push_back(make("face.west", RNF_DIAGRAM_FACE, fx - s, fy, r * 2, r * 2, RNF_SIDE_LEFT));
  e.push_back(make("face.east", RNF_DIAGRAM_FACE, fx + s, fy, r * 2, r * 2, RNF_SIDE_RIGHT));
  // D-pad arms.
  const double a = RNF_DIAGRAM_DPAD_ARM;
  e.push_back(make("dpad.up", RNF_DIAGRAM_DPAD, dx, dy - a, a, a, RNF_SIDE_ABOVE, "dpad"));
  e.push_back(make("dpad.down", RNF_DIAGRAM_DPAD, dx, dy + a, a, a, RNF_SIDE_BELOW, "dpad"));
  e.push_back(make("dpad.left", RNF_DIAGRAM_DPAD, dx - a, dy, a, a, RNF_SIDE_LEFT, "dpad"));
  e.push_back(make("dpad.right", RNF_DIAGRAM_DPAD, dx + a, dy, a, a, RNF_SIDE_RIGHT, "dpad"));
  // Sticks: four direction arrows + click (the cap).
  struct Stick { const char* name; double x, y; const char* up; const char* down; const char* left; const char* right; const char* click; };
  const Stick sticks[] = {
      {"lstick", lsx, lsy, "lstick.up", "lstick.down", "lstick.left", "lstick.right", "leftThumb"},
      {"rstick", rsx, rsy, "rstick.up", "rstick.down", "rstick.left", "rstick.right", "rightThumb"},
  };
  for (auto& st : sticks) {
    const double d = R - 9, ds = 15;
    e.push_back(make(st.up, RNF_DIAGRAM_STICK_DIRECTION, st.x, st.y - d, ds, ds, RNF_SIDE_ABOVE, st.name));
    e.push_back(make(st.down, RNF_DIAGRAM_STICK_DIRECTION, st.x, st.y + d, ds, ds, RNF_SIDE_BELOW, st.name));
    e.push_back(make(st.left, RNF_DIAGRAM_STICK_DIRECTION, st.x - d, st.y, ds, ds, RNF_SIDE_LEFT, st.name));
    e.push_back(make(st.right, RNF_DIAGRAM_STICK_DIRECTION, st.x + d, st.y, ds, ds, RNF_SIDE_RIGHT, st.name));
    // The click badge sits on the outer side of the pad (left stick: left, right stick: right).
    e.push_back(make(st.click, RNF_DIAGRAM_STICK_CLICK, st.x, st.y, 22, 22,
                     std::string(st.name) == "lstick" ? RNF_SIDE_LEFT : RNF_SIDE_RIGHT));
  }
  // Small center buttons. No HOME / guide button: it belongs to the system (rnf_input_element_ignored).
  if (deck) {  // View / Menu at the top corners
    e.push_back(make("options", RNF_DIAGRAM_SMALL, 34, 80, 26, 12, RNF_SIDE_BELOW));
    e.push_back(make("menu", RNF_DIAGRAM_SMALL, 526, 80, 26, 12, RNF_SIDE_BELOW));
  } else if (sym) {
    e.push_back(make("options", RNF_DIAGRAM_SMALL, 222, 90, 30, 14, RNF_SIDE_ABOVE));
    e.push_back(make("menu", RNF_DIAGRAM_SMALL, 338, 90, 30, 14, RNF_SIDE_ABOVE));
  } else {
    e.push_back(make("options", RNF_DIAGRAM_SMALL, 245, 104, 30, 14, RNF_SIDE_ABOVE));
    e.push_back(make("menu", RNF_DIAGRAM_SMALL, 315, 104, 30, 14, RNF_SIDE_ABOVE));
  }
  // Grouped badges ("Move") go below the group.
  i.dpad_anchor_x = dx; i.dpad_anchor_y = dy + a * 1.5 + 4;
  i.lstick_anchor_x = lsx; i.lstick_anchor_y = lsy + R + 4;
  i.rstick_anchor_x = rsx; i.rstick_anchor_y = rsy + R + 4;
  return l;
}

const Layout& layout(rnf_controller_family f) {
  static const Layout symmetric = build(RNF_FAMILY_PLAYSTATION);
  static const Layout standard = build(RNF_FAMILY_GENERIC);
  static const Layout deck = build(RNF_FAMILY_STEAM_DECK);
  return f == RNF_FAMILY_PLAYSTATION ? symmetric : f == RNF_FAMILY_STEAM_DECK ? deck : standard;
}

}  // namespace

extern "C" {

void rnf_diagram_info_get(rnf_controller_family f, rnf_diagram_info* out) {
  if (out) *out = layout(f).info;
}
size_t rnf_diagram_element_count(rnf_controller_family f) { return layout(f).elements.size(); }
int rnf_diagram_element_get(rnf_controller_family f, size_t index, rnf_diagram_element* out) {
  const Layout& l = layout(f);
  if (index >= l.elements.size() || !out) return 0;
  *out = l.elements[index];
  return 1;
}

size_t rnf_diagram_decor_count(rnf_controller_family f) { return layout(f).decor.size(); }
int rnf_diagram_decor_get(rnf_controller_family f, size_t index, rnf_diagram_decor* out) {
  const Layout& l = layout(f);
  if (index >= l.decor.size() || !out) return 0;
  *out = l.decor[index];
  return 1;
}

}  // extern "C"
