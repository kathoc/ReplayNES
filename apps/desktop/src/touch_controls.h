// The on-screen controller on touch screens (Android; docs/design/TOUCH_ANDROID.md): the iPhone's layout
// (touch/touch_layout) and logic (touch/touch_gamepad) driven by SDL's finger events, drawn with ImGui, its
// buttons sent to the game as "touch:<name>" inputs (InputRouter::setTouch). The extras: MENU, pause, slow,
// 8-WAY, the turbo / C / D discs; the picture's left / right quarter held = rewind / fast-forward.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "imgui.h"
#include "touch/touch_layout.h"

namespace rnl {

struct Settings;

class TouchControls {
 public:
  struct Host {
    std::function<void(const std::string& id, bool down)> press;  // "touch:a" ...
    std::function<void()> toggleMenu, togglePause, toggleSlow;
    std::function<void()> haptic;
    std::function<void(int ways)> setWays;  // 8-WAY pressed: the game's directions (setTouchWaysFor)
  };
  struct Frame {
    int widthPx = 0, heightPx = 0;
    double density = 1;  // pixels per point
    double windowToPx = 1;  // SDL's window coordinates (mouse events) -> pixels
    touch::TouchInsets safe;  // the window's safe area insets in points (cutouts, rounded corners)
    bool show = false;   // a game is open and nothing covers it (menu, dialog), no controller (by the setting)
    int fireButtons = 2;
    std::optional<double> pictureAspect;  // the picture's shown width / height
    bool fill = false;                    // Size: FILL
    double nowMs = 0;
    int ways = 8;            // the open game's D-pad directions (touchWaysFor)
    std::string sessionKey;  // which game / project is open (the zone marks show once per game)
    bool playing = false;    // not paused
  };
  explicit TouchControls(Host host);
  ~TouchControls();

  /// Once per frame before drawing: the layout for the window and settings, the D-pad's home return.
  void update(const Frame& f, const Settings& s);
  bool visible() const { return visible_; }
  /// The picture's area in pixels while shown (the layout's gameArea), else nullopt.
  std::optional<ImVec4> pictureArea() const;
  /// Where the bottom overlays go (the seek bar, hints): the layout's overlayArea in pixels, else nullopt.
  std::optional<ImVec4> overlayArea() const;
  /// The picture's rectangle as drawn (pixels), for the rewind / fast-forward quarters.
  void setPicture(float x, float y, float w, float h) { picture_ = ImVec4(x, y, w, h); }
  /// Finger events. True: the controller took it (the UI does not see it, nor the mouse SDL makes from it).
  bool handleFinger(const SDL_Event& e, double nowMs);
  /// SDL's mouse events made from a touch (which == SDL_TOUCH_MOUSEID) on the controller: dropped.
  bool takesTouchMouse(const SDL_Event& e);
  void draw(ImDrawList* dl, bool paused, bool slow) const;
  /// Everything released (hidden, a new layout, the window lost).
  void releaseAll();

 private:
  enum class Owner { pad, extra, rewind, fastForward };
  struct Finger {
    Owner owner;
    std::optional<touch::TouchExtra> extra;
  };
  touch::Point toPoints(float nx, float ny) const;
  /// What a touch at p (points) lands on: an extra, the pad, a quarter of the picture; false: the UI's.
  bool takes(touch::Point p) const;
  void onExtra(const touch::TouchExtra& e, bool down);

  Host host_;
  std::unique_ptr<touch::TouchGamepad> pad_;
  touch::TouchLayout layout_;
  bool haveLayout_ = false;
  touch::Size size_;
  touch::TouchInsets safe_;
  touch::TouchLayout::Options options_;
  double density_ = 1;
  double windowToPx_ = 1;
  int ways_ = 8;
  bool visible_ = false;
  bool vibration_ = true;
  float opacity_ = 1;
  ImVec4 picture_{0, 0, 0, 0};
  std::map<SDL_FingerID, Finger> fingers_;
  bool rewindHeld_ = false, fastForwardHeld_ = false;
  bool mouseTaken_ = false;
  // The picture's side quarters' marks for a few seconds when a game starts playing (the iPhone's ZoneHintMarks).
  std::string hintKey_;
  double hintStart_ = -1, nowMs_ = 0;
};

}  // namespace rnl
