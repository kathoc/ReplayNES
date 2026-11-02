// Where the picture and the on-screen controls go: the geometry of NESTERM's controller CSS
// (web/styles.css; portrait = `#app:not(.bb-side)` rules, landscape = `.bb-side` grid) computed for a
// screen size and its safe area, plus the buttons' names and their default bindings. Pure geometry in
// points (origin top-left); no UIKit, SDL or ImGui. Every number is the CSS value (or the ReplayNES
// deviation) it comes from in the Swift.
// Port of apps/ios/Sources/Play/Brickboy/TouchLayout.swift (ios-generalize), function by function.
// Original: NESTERM web/styles.css (Brickboy).
// The one non-1:1 piece is text width: the Swift measures the pill labels with UIFont, here the host
// sets TouchLayout::textMeasurer (a fixed estimate is used until it does).
//
// Portions derived from Brickboy / NESTERM web/styles.css, Copyright 2026 Katsuya Hokamura,
// Apache-2.0. The ReplayNES extras and TouchButton / TouchBindings are ReplayNES's own.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "touch/touch_gamepad.h"

namespace rnl::touch {

/// One on-screen button as the app sees it. Game buttons reach the engine as the physical ids
/// "touch:<name>" (bound like a controller's: TouchBindings); `menu` opens the Quick Menu.
enum class TouchButton {
  up, down, left, right,
  a, b, turboA, turboB,
  select, start,
  /// Rewind / fast-forward (bound like a controller's L2 / R2).
  l2, r2,
  menu,
  /// Frame step back / forward: handled by the app.
  stepBack, stepForward,
  /// Slow motion: handled by the app.
  slow,
  /// Pause: handled by the app.
  pause,
  /// Buttons 3 / 4 of another core's game, in AA's / BB's place; never on the NES.
  c, d
};
/// TouchButton.allCases (declaration order).
extern const std::vector<TouchButton> TouchButton_allCases;
/// TouchButton.rawValue.
const char* rawValue(TouchButton b);
/// The engine input id ("touch:dpad.up"), none for buttons handled by the app.
std::optional<std::string> inputID(TouchButton b);
bool isDirection(TouchButton b);
/// TouchButton.init(_: BBButton): the Brickboy gamepad's buttons (input-router.mjs BUTTON_NAME).
TouchButton touchButton(BBButton b);

struct TouchBinding {
  std::string input;
  std::string action;
  bool operator==(const TouchBinding& o) const { return input == o.input && action == o.action; }
};

/// Default bindings of the touch ids: the same actions as a controller's.
struct TouchBindings {
  /// NES.
  static const std::vector<TouchBinding>& defaults();
  /// Another core's layout (`actions`: its game action ids, "p1.b1", "p1.b1.turbo" ...).
  static std::vector<TouchBinding> forLayout(const std::set<std::string>& actions);
  /// Bindings to add to a saved configuration that has none for the touch ids.
  static std::vector<TouchBinding> missing(const std::vector<TouchBinding>& existing);
};

struct TouchInsets {
  double top = 0, left = 0, bottom = 0, right = 0;
  bool operator==(const TouchInsets& o) const {
    return top == o.top && left == o.left && bottom == o.bottom && right == o.right;
  }
};

/// A control drawn by the host beside the gamepad: a pill of the extras row (a ReplayNES button, or
/// NESTERM's 4/8-way switch) or one of the AA / BB rounds up-left of A / B.
struct TouchExtra {
  struct Kind {
    enum Type { button, ways };  // ways: #ways-switch, toggles 8-way / 4-way
    Type type = button;
    TouchButton btn = TouchButton::up;  // for .button
    static Kind makeButton(TouchButton b) { return Kind{button, b}; }
    static Kind makeWays() { return Kind{ways, TouchButton::up}; }
    bool operator==(const Kind& o) const { return type == o.type && (type == ways || btn == o.btn); }
  };
  /// .bb-mode-switch pill with a text label, the same pill with an icon (SF Symbol name in the
  /// Swift; `icon` names the glyph for the host), or a .gp-round (AA / BB: `frame` is the disc's square).
  struct Style {
    enum Type { pill, icon, round };
    Type type = pill;
    std::string name;  // the icon's name
    static Style makePill() { return Style{pill, {}}; }
    static Style makeIcon(const std::string& n) { return Style{icon, n}; }
    static Style makeRound() { return Style{round, {}}; }
    bool operator==(const Style& o) const { return type == o.type && name == o.name; }
  };
  Kind kind;
  Rect frame;
  Style style;

  bool operator==(const TouchExtra& o) const { return kind == o.kind && frame == o.frame && style == o.style; }
  /// Touchdown test: a pill's box grown to the 44 pt minimum (`hitFrame`); a round's disc grown by
  /// TouchLayout::turboSlop.
  bool hit(Point p) const;
  /// A pill's touch area: its box grown about its centre to at least TouchLayout::minTarget in each
  /// direction. A round's: its square plus the slop.
  Rect hitFrame() const;
  /// One of the transport row's buttons.
  bool isTransport() const;
};

struct TouchLayout {
  // Declared first: Options' defaults use it.
  static constexpr double dpadDefault = 144;
  static constexpr double dpadRangeLo = 130;  // dpadRange: 130...210
  static constexpr double dpadRangeHi = 210;

  struct Options {
    /// AA / BB (turbo A / turbo B) up-left of A / B.
    bool showAABB = true;
    /// --bb-dpad (controller.mjs applySize: 130...210 px, CSS default 144px).
    double dpadSize = dpadDefault;
    /// 8 or 4: the label of the ways switch ("8-WAY").
    int ways = 8;
    /// Display FILL (not integer scale).
    bool fill = false;
    /// The picture's displayed width / height; none = the NES picture.
    std::optional<double> pictureAspect;
    /// The game's fire buttons (the NES: A and B). 3: AA's disc is button 3 ("C"); 4: BB's is button 4 too.
    int buttons = 2;
    /// The round discs up-left of A / B are shown (turbo keys, or buttons 3 / 4).
    bool showRounds() const { return showAABB || buttons >= 3; }
    bool operator==(const Options& o) const {
      return showAABB == o.showAABB && dpadSize == o.dpadSize && ways == o.ways && fill == o.fill &&
             pictureAspect == o.pictureAspect && buttons == o.buttons;
    }
  };

  Size size;
  bool landscape = false;
  /// The area the game view covers (the renderer centres and scales the picture inside it).
  Rect gameArea;
  /// TouchGamepad's geometry (the D-pad at its natural place, A/B, SELECT/START, hit boxes).
  TouchGamepad::Geometry pad;
  /// The A/B button diameter (--gp-btn).
  double faceButton = 0;
  /// SELECT / START pills (drawn; their hit areas are in `pad`).
  Rect select;
  Rect start;
  /// The extras: the extras row's pills, then AA / BB (style round) when shown.
  std::vector<TouchExtra> extras;
  /// Where the overlays at the bottom of the game go: the game area clear of the controls.
  Rect overlayArea;

  bool operator==(const TouchLayout& o) const {
    return size == o.size && landscape == o.landscape && gameArea == o.gameArea && pad == o.pad &&
           faceButton == o.faceButton && select == o.select && start == o.start && extras == o.extras &&
           overlayArea == o.overlayArea;
  }

  std::optional<TouchExtra> extra(TouchButton b) const;

  /// NES picture aspect with the overscan rows hidden (256 x 224).
  static constexpr double pictureAspect = 256.0 / 224.0;

  // :root { font: 15px/1.4 system-ui }: buttons inherit the line height (1.4).
  static constexpr double lineHeight = 1.4;
  // .gp-pill { padding: 8px 22px; font-size: 11px; font-weight: 600; letter-spacing: 1px }
  static constexpr double pillFont = 11;
  static constexpr double pillTracking = 1;
  static constexpr double pillPadX = 22;
  static constexpr double pillPadY = 8;
  // .bb-mode-switch, .bb-turbo-switch { padding: 5px 10px; border: 1px; font-size: 11px;
  //   font-weight: 600; letter-spacing: .4px }
  static constexpr double switchTracking = 0.4;
  static constexpr double switchPadX = 10;
  static constexpr double switchPadY = 5;
  static constexpr double switchBorder = 1;
  // .bb-subbar { min-height: 34px; padding: 0 8px }
  static constexpr double subbarHeight = 34;
  static constexpr double subbarPadX = 8;
  // .gp-face { --gp-gap: 2px; --gp-tilt: .4502 }
  static constexpr double faceGap = 2;
  static constexpr double faceTilt = 0.4502;
  /// B sits this fraction of the button diameter further right than the CSS puts it.
  static constexpr double bTowardA = 0.12;
  /// AA / BB at 11 o'clock of A / B (30 degrees left of straight up).
  static constexpr double turboAngle = 30;
  /// The slow-motion button: its box.
  static constexpr Size slowSize{44, 32};
  static constexpr double slowGap = 4;
  /// The Dynamic Island (portrait): half its width and the gap kept to it.
  static constexpr double islandHalfWidth = 63;
  static constexpr double islandGap = 12;
  static double islandMidY(double safeTop);
  /// The bottom overlays' clearance above the controls (portrait).
  static constexpr double overlayGap = 8;
  /// Pause above MENU: the gap between their touch areas.
  static constexpr double pauseGap = 8;

  // ReplayNES deviations from the CSS.
  /// The D-pad is drawn 10 % larger than --bb-dpad / the portrait clamp give.
  static constexpr double dpadScale = 1.1;
  /// AA / BB: diameter relative to --gp-btn, clear gap to the A / B disc below, hit slop.
  static constexpr double turboScale = 0.72;
  static constexpr double turboGap = 8;
  static constexpr double turboSlop = 4;
  static constexpr double turboFont = 15;
  /// Apple's minimum touch target (points): every pill's touch area is at least this.
  static constexpr double minTarget = 44;
  /// The transport row, left to right: Rewind, Step Back, Step Forward, Fast-forward.
  static const std::vector<TouchButton>& transportOrder();
  static constexpr double transportHeight = 44;
  static constexpr double transportWide = 112;
  static constexpr double transportGap = 8;
  /// Space between the transport row and the picture / the gamepad (portrait).
  static constexpr double transportMargin = 4;
  /// Landscape: clear space between the picture and the control columns.
  static constexpr double pictureMargin = 8;

  static TouchLayout make(Size size, TouchInsets safe);
  static TouchLayout make(Size size, TouchInsets safe, const Options& options);

  // MARK: text metrics
  enum class Weight { regular, semibold };
  /// The host's text measurer: the advance width of `s` (no letter-spacing) at `size` pt in the
  /// system font. The Swift uses UIFont.systemFont; set this for the same numbers. Until set, a
  /// fixed estimate (0.66 em per character, 0.6 em regular) is used.
  using TextMeasurer = std::function<double(const std::string& s, double size, Weight weight)>;
  static TextMeasurer textMeasurer;

  /// Width of a label in the system font, CSS letter-spacing added after every character.
  static double textWidth(const std::string& s, double size, Weight weight, double tracking);
  /// A .gp-pill's size (SELECT / START); `minHeight`: portrait's `.gp-pill { min-height: 40px }`.
  static Size pillSize(const std::string& label, double minHeight);
  /// A .bb-mode-switch-styled pill (the extras row).
  static Size switchSize(const std::string& label);
  static std::string label(const TouchExtra::Kind& k, int ways);
  /// How an extra is drawn.
  static TouchExtra::Style style(const TouchExtra::Kind& k);
  /// The transport row centred on `midX` with its bottom at `bottom`, at most `width` wide.
  static std::vector<TouchExtra> transport(double midX, double bottom, double width);

  /// AA / BB up-left of A / B: diameter and the distance from the A (B) centre to the AA (BB) centre.
  struct TurboMetrics {
    double size;
    double step;
  };
  static TurboMetrics turboMetrics(double btn);
  /// The direction from A (B) to AA (BB): unit vector, screen coordinates (y down).
  static Vector turboDirection(double btn);
  /// The offset from A (B) to AA (BB), turned toward straight up only as far as needed to keep BB's
  /// disc right of `minLeft`.
  static Vector turboOffset(double btn, Point b, double minLeft);
  /// How far AA / BB (top of the disc) reach above the A/B midpoint.
  static double turboAbove(double btn, Vector offset);

private:
  static std::vector<TouchExtra> turboExtras(Point a, Point b, double btn, Vector v, const Options& o);
  static double faceMid(const Rect& row, double top, double above, double below);
  static TouchExtra pill(const TouchExtra::Kind& k, double x, double midY, int ways);
  struct FaceBox {
    Rect box;
    Point a;
    Point b;
  };
  static FaceBox face(double x, double midY, double btn, double padX, double padY);
  static TouchLayout portraitLayout(Size size, TouchInsets safe, const Options& o);
  static TouchLayout landscapeLayout(Size size, TouchInsets safe, const Options& o);
};

}  // namespace rnl::touch
