// Geometry of the controller settings diagram (canvas RNF_DIAGRAM_CANVAS_WIDTH x HEIGHT points,
// origin top-left). Nintendo / Xbox / generic / Steam Deck put the left stick above the D-pad;
// PlayStation has both sticks at the bottom.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <vector>

#include "common.hpp"

namespace {

struct Layout {
  rnf_diagram_info info{};
  std::vector<rnf_diagram_element> elements;
};

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
  const double lsx = sym ? 225 : 165, lsy = sym ? 188 : 120;
  const double rsx = 335, rsy = sym ? 188 : 182;
  const double dx = sym ? 160 : 225, dy = sym ? 128 : 182;
  const double fx = sym ? 400 : 395, fy = sym ? 125 : 120;
  const double R = RNF_DIAGRAM_STICK_RADIUS;
  auto& i = l.info;
  i.left_stick_x = lsx; i.left_stick_y = lsy;
  i.right_stick_x = rsx; i.right_stick_y = rsy;
  i.stick_radius = R;
  i.dpad_x = dx; i.dpad_y = dy;
  i.face_x = fx; i.face_y = fy;
  i.has_touchpad = sym;
  if (sym) { i.touchpad_x = 252; i.touchpad_y = 84; i.touchpad_width = 56; i.touchpad_height = 44; }

  auto& e = l.elements;
  // Triggers / shoulders.
  e.push_back(make("leftTrigger", RNF_DIAGRAM_TRIGGER, 150, 18, 88, 22, RNF_SIDE_LEFT));
  e.push_back(make("rightTrigger", RNF_DIAGRAM_TRIGGER, 410, 18, 88, 22, RNF_SIDE_RIGHT));
  e.push_back(make("leftShoulder", RNF_DIAGRAM_SHOULDER, 150, 48, 108, 18, RNF_SIDE_LEFT));
  e.push_back(make("rightShoulder", RNF_DIAGRAM_SHOULDER, 410, 48, 108, 18, RNF_SIDE_RIGHT));
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
  // Small center buttons.
  if (sym) {
    e.push_back(make("options", RNF_DIAGRAM_SMALL, 227, 90, 34, 14, RNF_SIDE_ABOVE));
    e.push_back(make("menu", RNF_DIAGRAM_SMALL, 333, 90, 34, 14, RNF_SIDE_ABOVE));
    e.push_back(make("home", RNF_DIAGRAM_HOME, 280, 152, 22, 22, RNF_SIDE_BELOW));
  } else {
    e.push_back(make("options", RNF_DIAGRAM_SMALL, 245, 104, 30, 14, RNF_SIDE_ABOVE));
    e.push_back(make("menu", RNF_DIAGRAM_SMALL, 315, 104, 30, 14, RNF_SIDE_ABOVE));
    e.push_back(make("home", RNF_DIAGRAM_HOME, 280, 134, 22, 22, RNF_SIDE_BELOW));
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
  return f == RNF_FAMILY_PLAYSTATION ? symmetric : standard;
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

}  // extern "C"
