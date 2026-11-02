// The on-screen touch gamepad (NESTERM's Brickboy controller): the D-pad with its learned home and
// sectors, the A/B pad with the second-finger latch and the roll tap, SELECT / START, the 60 Hz frame
// tick. No SDL, ImGui or clock of its own: positions are points, times are milliseconds the caller
// passes in, and presses / haptics go out through callbacks.
// Port of apps/ios/Sources/Play/Brickboy/TouchGamepad.swift (ios-generalize), function by function;
// names, constants, state and the order of every step (floating point included) are the Swift's.
// Original: NESTERM web/brickboy/input.mjs (upstream kathoc/brickboy src/ui/input.ts).
// Replaced browser / UIKit plumbing: DOM boxes -> Geometry (TouchLayout), PointerEvent -> PointerEvent,
// requestAnimationFrame -> animationFrame(now) called at 60 Hz by the host, navigator.vibrate -> onHaptic.
// Not ported (as in the Swift): debug overlay, gesture recording, probe(), CSS rotation, handleKey,
// the A/B autofire ring.
//
// Copyright 2026 Katsuya Hokamura. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace rnl::touch {

// ---- CoreGraphics stand-ins (CGFloat is double; origin top-left, y down) ----
struct Point {
  double x = 0, y = 0;
  bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};
struct Size {
  double width = 0, height = 0;
  bool operator==(const Size& o) const { return width == o.width && height == o.height; }
};
struct Vector {
  double dx = 0, dy = 0;
};
struct Rect {
  double x = 0, y = 0, w = 0, h = 0;
  double minX() const { return x; }
  double maxX() const { return x + w; }
  double midX() const { return x + w / 2; }
  double minY() const { return y; }
  double maxY() const { return y + h; }
  double midY() const { return y + h / 2; }
  double width() const { return w; }
  double height() const { return h; }
  // CGRect.contains: minX <= x < maxX, minY <= y < maxY (an empty rect contains nothing).
  bool contains(Point p) const { return p.x >= minX() && p.x < maxX() && p.y >= minY() && p.y < maxY(); }
  Rect offsetBy(double dx, double dy) const { return {x + dx, y + dy, w, h}; }
  Rect insetBy(double dx, double dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
  // CGRect.intersection; a disjoint pair gives the empty rect {0, 0, 0, 0} (CGRect.null).
  Rect intersection(const Rect& o) const {
    double l = std::max(minX(), o.minX()), r = std::min(maxX(), o.maxX());
    double t = std::max(minY(), o.minY()), b = std::min(maxY(), o.maxY());
    if (r <= l || b <= t) return {};
    return {l, t, r - l, b - t};
  }
  bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

/// buttons.mjs `Button`: adapter identifiers matching Brickboy src/emu/joypad.ts. These are
/// identifiers, not NES bit masks.
enum class BBButton : int { right = 0, left = 1, up = 2, down = 3, a = 4, b = 5, select = 6, start = 7 };
/// BBButton.allCases.
inline constexpr std::array<BBButton, 8> BBButton_allCases = {
    BBButton::right, BBButton::left, BBButton::up,     BBButton::down,
    BBButton::a,     BBButton::b,    BBButton::select, BBButton::start};

// ---- D-pad feel constants (input.mjs, top of file; values and names unchanged) ----
inline constexpr double R_COMMIT_OF_REACH = 0.6;
inline constexpr double R_COMMIT_MIN = 0.12;  // pad radii
inline constexpr double R_COMMIT_MAX = 0.3;   // pad radii
inline constexpr double REACH_DEFAULT = 0.45; // assumed reach (pad radii) before any taps are in
inline constexpr double DZ_OF_REACH = 0.25;
inline constexpr double DZ_MIN = 0.08;        // pad radii
inline constexpr double DZ_MAX = 0.2;         // pad radii
inline constexpr double SPEED_STILL = 0.06;   // px/ms below which home-calibration may run
inline constexpr double R_TARGET = 0.28;
inline constexpr double R_MAX_TARGET = 0.6;
inline constexpr double CARD_HALF_ENGAGE = 29.25; // 65% of the circle to the cardinals
inline constexpr double CARD_HALF_HELD = 22.5;    // an even 50/50 once engaged
inline constexpr double SHARE_RAMP_TICKS = 18;    // ~300 ms of calibration ticks to relax across
inline constexpr double CARD_HALF_4WAY = 45;
inline constexpr double R_COMMIT_4WAY = 1.6;      // multiplies the commit radius (and its clamps)
inline constexpr double ANGLE_HYST = 6;           // extra degrees to leave a sector - only while nearly still
inline constexpr double HOME_LEAK = 1.2;          // per-second rate the home folds toward the finger
inline constexpr double HOME_LEAK_X = 1;          // horizontal calibrates at the vertical rate too
inline constexpr double HOME_MAX_X = 0.4;         // max horizontal home offset, in pad radii
inline constexpr double HOME_MAX_Y = 0.55;        // max vertical home offset, in pad radii
inline constexpr double RELAX_ON_TURN = 0.6;
inline constexpr double HOME_RETURN_DELAY = 260;  // ms of complete stillness before the return starts
inline constexpr double HOME_RETURN_TAU = 420;    // ms time-constant of the ease once it does
inline constexpr double TD_DECAY = 0.92;          // per touchdown - an effective window of ~12 recent taps
inline constexpr double TD_MIN_W = 2.5;           // accumulated weight before the estimate is acted on
inline constexpr double TD_GAIN = 0.3;
inline constexpr double TD_LEAK = 0.03;
inline constexpr double EDGE_MARGIN = 8;          // px the pad must stay inside the viewport
inline constexpr int HAPTIC_MS = 8;               // weak tick on each press edge (see onHaptic)
// A/B shared pad (input.mjs; NESTERM roll-tap additions ROLL_*).
inline constexpr double FACE_THR = 0.62;
inline constexpr double FACE_LATCH_TRAVEL = 0.3;  // fraction of the centre separation
inline constexpr double FACE_HYST = 1.12;
inline constexpr double ROLL_RETURN = 0.08;
inline constexpr double ROLL_ENGAGE = 0.12;
inline constexpr double ROLL_WINDOW_MS = 160;

/// input.mjs `DIR8`: eight directions on the pad (centre angle in screen degrees: 0 = right,
/// 90 = down; the button(s) each engages).
enum class Dir8 { R, DR, D, DL, L, UL, U, UR };
double c(Dir8 d);                        // Dir8.c
const std::vector<BBButton>& btns(Dir8 d); // Dir8.btns

/// input.mjs `halfOf`: half-width of one sector, given how much of the circle the cardinals hold.
double halfOf(Dir8 name, double cardHalf);
/// input.mjs `DIR8_ORDER`.
extern const std::array<Dir8, 8> DIR8_ORDER;
/// input.mjs `DIR8_SETS` (index with static_cast<size_t>(Dir8)) / `NO_DIRS`.
extern const std::array<std::set<BBButton>, 8> DIR8_SETS;
extern const std::set<BBButton> NO_DIRS;

/// input.mjs `norm360`.
double norm360(double deg);
/// input.mjs `angularDist`.
double angularDist(double a, double b);
/// input.mjs `sectorRaw`: raw sector for an angle, ignoring hysteresis.
Dir8 sectorRaw(double angle, double cardHalf);

/// One pointer event: `id` = pointerId, `x` / `y` = clientX / clientY (points in the surface that
/// covers the whole window), `t` = timeStamp in ms (the same clock as animationFrame's `now`).
struct PointerEvent {
  int id = 0;
  double x = 0;
  double y = 0;
  double t = 0;
};

/// Per-active-pointer D-pad tracking (input.mjs `DpadPtr`). Held by shared_ptr: the JS mutates the
/// object in place.
struct DpadPtr {
  double lastX;
  double lastY;
  double lastT;
  double seenT;
  double speed = 0;
  std::optional<Dir8> dir8;
  std::optional<Dir8> calibDir;
  int heldTicks = 0;
  double accDX = 0;
  double accDY = 0;
  std::set<BBButton> dirs;

  DpadPtr(double lastX, double lastY, double lastT) : lastX(lastX), lastY(lastY), lastT(lastT), seenT(lastT) {}
};

/// One sample of a face roll's history: `t` (ms) and `s` (progress along A-B).
struct FaceRollSample {
  double t;
  double s;
};

/// NESTERM roll state per face pointer (input.mjs `rollTap`'s `roll` object).
struct FaceRoll {
  BBButton base;
  bool on;
  std::vector<FaceRollSample> hist;
  double low = std::numeric_limits<double>::infinity();
  double ext = -std::numeric_limits<double>::infinity();
  bool released = false;
  double s = 0;

  FaceRoll(BBButton base, bool on) : base(base), on(on) {}
};

class TouchGamepad {
public:
  /// Where the DOM elements are (points, the surface's coordinates); TouchLayout computes them.
  struct Geometry {
    /// window.innerWidth / innerHeight (elemViewport).
    Size viewport;
    /// The .bb-root content box (frameRect): where the pad may be placed.
    Rect frame;
    /// The .gp-dpad box at its natural place (before transform: translate(ox, oy)).
    Rect dpad;
    /// Hit boxes of .gp-ext-up / .gp-ext-down: 34% of the pad's height above and below it.
    double dpadExtFraction = 0.34;
    /// Portrait: .bb-gamepad's box (the clip also clips the hit targets); none in landscape.
    std::optional<Rect> dpadClip;
    /// The .gp-face box (padding included): the A/B shared pad's hit area.
    Rect face;
    /// Centres of .gp-a / .gp-b with their translateY(-+--gp-off), not pressed.
    Point a;
    Point b;
    /// .gp-system's box in portrait (one hit area over both pills); none in landscape, where only the
    /// pills themselves take touches.
    std::optional<Rect> system;
    Rect select;
    Rect start;
    /// A pressed round moves 3 px up, easing over 60 ms; faceGeometry sees it.
    double pressOffset = -3;
    double pressTransitionMs = 60;
    /// .gp-edge-left { left: -4px }: how far the left glow bar reaches outside the pad.
    double edgeLeftOutset = -4;

    bool operator==(const Geometry& o) const {
      return viewport == o.viewport && frame == o.frame && dpad == o.dpad && dpadExtFraction == o.dpadExtFraction &&
             dpadClip == o.dpadClip && face == o.face && a == o.a && b == o.b && system == o.system &&
             select == o.select && start == o.start && pressOffset == o.pressOffset &&
             pressTransitionMs == o.pressTransitionMs && edgeLeftOutset == o.edgeLeftOutset;
    }
  };

  /// Which element a pointer went down on (the element that holds its pointer capture).
  enum class Region { dpad, face, system };

  /// A pointer latched to one button by the second-finger rule, with the point it landed on.
  struct FaceLatch {
    BBButton btn;
    double x;
    double y;
  };

  /// faceGeometry's result.
  struct FaceGeom {
    double ax, ay, bx, by, sep;
  };

  /// padBox's result: the pad's own size and its centre on screen.
  struct PadBox {
    double w, h, cx, cy;
  };

  std::function<void(BBButton, bool)> press;
  /// Optional per-gesture feedback hook (audio tick); fires once per batch on the same edge as the
  /// haptic, so a diagonal ticks once.
  std::function<void(BBButton)> feedback;
  /// navigator.vibrate(HAPTIC_MS): the haptic tick.
  std::function<void()> onHaptic;
  /// Something visible changed: a gp-active class or the pad's transform.
  std::function<void()> onVisual;
  /// A requestAnimationFrame callback is waiting: the host should call animationFrame(now) at 60 Hz.
  std::function<void()> onAnimationFrameRequest;

  Geometry geometry;
  /// Live press count per button (all sources); edges drive press + visual. Read-only for callers.
  std::map<BBButton, int> counts;
  /// State for each active D-pad pointer. Read-only for callers.
  std::map<int, std::shared_ptr<DpadPtr>> dpadPtrs;
  /// A/B currently engaged by each active face pointer. Read-only for callers.
  std::map<int, std::set<BBButton>> facePointers;
  /// NESTERM roll state per face pointer. Read-only for callers.
  std::map<int, std::shared_ptr<FaceRoll>> faceRoll;
  /// Pointers latched to one button. Read-only for callers.
  std::map<int, FaceLatch> faceLatch;
  /// Start/Select engaged by each active system pointer. Read-only for callers.
  std::map<int, std::set<BBButton>> sysPointers;
  /// Current home offset (px) from the pad's natural CSS centre. Read-only for callers.
  double ox = 0;
  double oy = 0;
  /// Decaying touchdown sums behind the rest estimate (see foldTouchdown). Read-only for callers.
  double tdW = 0;
  double tdX = 0;
  double tdY = 0;
  double tdA = 0;
  double tdUX = 0;
  double tdUY = 0;
  /// 8 = diagonals available, 4 = cardinals only. Read-only for callers.
  int ways = 8;
  /// Calibration gain per axis (multiples of HOME_LEAK); see setHomeGain. Read-only for callers.
  double gainX = HOME_LEAK_X;
  double gainY = 1;

  /// input.mjs `constructor(root, press, feedback)` (the DOM building is the host's).
  explicit TouchGamepad(std::function<void(BBButton, bool)> press, std::function<void(BBButton)> feedback = {});
  // The animation callbacks capture `this`.
  TouchGamepad(const TouchGamepad&) = delete;
  TouchGamepad& operator=(const TouchGamepad&) = delete;

  // MARK: requestAnimationFrame (run by the host at 60 Hz)
  bool hasPendingAnimationFrame() const;
  /// One display frame: runs the callbacks requested before it (requested during it run on the next
  /// frame, as in a browser). `now` in ms on the touch clock.
  void animationFrame(double now);

  // MARK: public surface
  /// The gp-active class of a button's elements: its count is above 0.
  bool isActive(BBButton btn) const;
  int wayCount() const;
  /// input.mjs `relayout`: re-clamp the home after the pad's geometry changed from outside.
  void relayout();
  /// Which element a new touch at `p` lands on (the DOM hit test).
  std::optional<Region> regionAt(Point p) const;
  /// pointerdown on the element under the finger (that element then holds the capture).
  std::optional<Region> pointerDown(const PointerEvent& e);
  void pointerMove(const PointerEvent& e);
  void pointerUp(const PointerEvent& e);
  /// input.mjs `releasePointer`.
  void releasePointer(int id);
  /// input.mjs `setWays`.
  void setWays(int ways);
  /// input.mjs `setHomeGain`.
  void setHomeGain(double x, double y);
  /// input.mjs `releaseAll`.
  void releaseAll();

  /// input.mjs `setHome`.
  void setHome(double ox, double oy);
  /// input.mjs `resetHome`.
  void resetHome();
  /// input.mjs `engageDeadzone`.
  double engageDeadzone(double radius) const;
  /// input.mjs `commitRadius`.
  double commitRadius(double radius) const;
  /// input.mjs `restEstimate`: the thumb's rest point as a home offset, or none while too few taps.
  std::optional<Point> restEstimate() const;
  /// input.mjs `systemButtonsAt`.
  std::set<BBButton> systemButtonsAt(double x, double y) const;
  /// input.mjs `faceGeometry`.
  FaceGeom faceGeometry() const;
  /// CSS `ease` = cubic-bezier(0.25, 0.1, 0.25, 1): progress at time fraction x.
  static double cssEase(double x);

private:
  struct PressAnim {
    double from;
    double t0;
  };
  struct RafCallback {
    int id;
    std::function<void(double)> cb;
  };

  /// Insertion order of dpadPtrs (a JS Map iterates in insertion order).
  std::vector<int> dpadOrder;
  std::vector<int> facePointerOrder;
  /// Pointer capture: the region each pointer went down on.
  std::map<int, Region> captured;
  /// rAF id for the post-release ease back to centre (0 = not running).
  int homingRaf = 0;
  double homeTickLast = 0;
  /// rAF id for the while-held calibration clock (0 = not running).
  int calibRaf = 0;
  double calibLast = 0;
  /// Set true at the entry of each engaging gesture so a multi-button batch fires one haptic tick.
  bool hapticArmed = false;
  /// The running --gp-press transition of each round: where it started and when.
  std::map<BBButton, PressAnim> pressAnim;
  /// The clock of the latest event / frame (performance.now()).
  double now = 0;
  double homeStartT = 0;
  Point homeRest;
  int rafNext = 1;
  std::vector<RafCallback> rafCallbacks;

  int requestAnimationFrame(std::function<void(double)> cb);
  void cancelAnimationFrame(int id);

  void setBtn(BBButton btn, bool down);
  void haptic();
  void stopHomeReturn();
  void startHomeReturn();
  void homeReturnTick(double now);
  Point toElem(double dx, double dy) const;
  PadBox padBox() const;
  Rect elemRect() const;
  double visibleLeftOffset() const;
  std::set<BBButton> resolveDirs(DpadPtr& st, double vx, double vy, double r, double rCommit, bool sticky,
                                 double cardHalf, double engageDz);
  void applyDpad(DpadPtr& st, const std::set<BBButton>& next);
  void foldTouchdown(double px, double py, double hx, double hy);
  void calibrateFromTaps();
  void onDpadDown(const PointerEvent& e);
  void onDpadMove(const PointerEvent& e);
  void readDirs(DpadPtr& st);
  void startCalib();
  void calibTick(double now);
  double padRadiusForCalib() const;
  void calibrate(DpadPtr& st, double dt);
  void onDpadUp(const PointerEvent& e);
  void removeDpadPtr(int id);
  void applySystem(int id, const std::set<BBButton>& next);
  void onSysDown(const PointerEvent& e);
  void onSysMove(const PointerEvent& e);
  void onSysUp(const PointerEvent& e);
  double pressShift(BBButton btn) const;
  std::optional<BBButton> faceSoloHeldByOther(int id) const;
  std::set<BBButton> faceButtonsAt(double x, double y, int id);
  std::set<BBButton> rollTap(int id, const std::set<BBButton>& setIn, const std::optional<std::set<BBButton>>& mine,
                             double x, double y, const FaceGeom& g, double now);
  void applyFace(int id, const std::set<BBButton>& next);
  void removeFacePointer(int id);
  void onFaceDown(const PointerEvent& e);
  void onFaceMove(const PointerEvent& e);
  void onFaceUp(const PointerEvent& e);
};

}  // namespace rnl::touch
