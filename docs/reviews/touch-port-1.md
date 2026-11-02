# touch-port-1: TouchGamepad.swift / TouchLayout.swift to C++17

Files (all under apps/desktop/src/touch/, namespace `rnl::touch`): `touch_gamepad.h/.cpp`,
`touch_layout.h/.cpp`; both .cpp added to `rnl_logic` in apps/desktop/CMakeLists.txt. No tests, no
other files changed, nothing committed. Source: ios-generalize `TouchGamepad.swift` (1019 lines),
`TouchLayout.swift` (600 lines). Both .cpp files carry `#pragma STDC FP_CONTRACT OFF` (clang only):
Swift does not fuse a*b+c, clang's C++ default would on arm64, which could differ by ulps.

## Public API (touch_gamepad.h)

- Plain structs: `Point{x,y}`, `Size{width,height}`, `Vector{dx,dy}`,
  `Rect{x,y,w,h}` (+ `minX/maxX/midX/minY/maxY/midY/width/height`, `contains` (CGRect semantics),
  `offsetBy`, `insetBy`, `intersection` (disjoint gives `{0,0,0,0}`)).
- `enum class BBButton {right,left,up,down,a,b,select,start}`, `BBButton_allCases`.
- All constants (`R_COMMIT_OF_REACH` ... `ROLL_WINDOW_MS`, `HAPTIC_MS`) as `inline constexpr`.
- `enum class Dir8`; `double c(Dir8)`; `const std::vector<BBButton>& btns(Dir8)`;
  `double halfOf(Dir8, double)`; `DIR8_ORDER`; `DIR8_SETS` (array indexed by `size_t(Dir8)`);
  `NO_DIRS`; `norm360`, `angularDist`, `sectorRaw(double, double)`.
- `struct PointerEvent{int id; double x, y, t;}`, `struct DpadPtr`, `struct FaceRoll`, `FaceRollSample{t,s}`.
- `class TouchGamepad` (non-copyable):
  - `TouchGamepad(std::function<void(BBButton,bool)> press, std::function<void(BBButton)> feedback = {})`
  - public members: `press`, `feedback`, `onHaptic`, `onVisual`, `onAnimationFrameRequest` (std::function),
    `Geometry geometry`; state `counts, dpadPtrs, facePointers, faceRoll, faceLatch, sysPointers, ox, oy,
    tdW, tdX, tdY, tdA, tdUX, tdUY, ways, gainX, gainY` (Swift `private(set)`; read-only by convention)
  - `Geometry{viewport, frame, dpad, dpadExtFraction, std::optional<Rect> dpadClip, face, a, b,
    std::optional<Rect> system, select, start, pressOffset, pressTransitionMs, edgeLeftOutset}` + `operator==`
  - `enum class Region{dpad, face, system}`
  - `bool hasPendingAnimationFrame() const`; `void animationFrame(double now)`
  - `bool isActive(BBButton) const`; `int wayCount() const`; `void relayout()`
  - `std::optional<Region> regionAt(Point) const`
  - `std::optional<Region> pointerDown(const PointerEvent&)`; `void pointerMove(const PointerEvent&)`;
    `void pointerUp(const PointerEvent&)`; `void releasePointer(int)`
  - `void setWays(int)`; `void setHomeGain(double, double)`; `void releaseAll()`
  - `void setHome(double, double)`; `void resetHome()`
  - `double engageDeadzone(double) const`; `double commitRadius(double) const`;
    `std::optional<Point> restEstimate() const`
  - `std::set<BBButton> systemButtonsAt(double, double) const`;
    `FaceGeom faceGeometry() const` (`{ax,ay,bx,by,sep}`); `static double cssEase(double)`

## Public API (touch_layout.h)

- `enum class TouchButton` (same cases/order), `TouchButton_allCases`, `rawValue(b)`, `inputID(b)`
  (`std::optional<std::string>`), `isDirection(b)`, `touchButton(BBButton)` (Swift `TouchButton.init(_:)`).
- `TouchBinding{input, action}`; `TouchBindings::defaults()`, `::forLayout(const std::set<std::string>&)`,
  `::missing(const std::vector<TouchBinding>&)`.
- `TouchInsets{top,left,bottom,right}`.
- `TouchExtra{Kind kind; Rect frame; Style style;}` with `Kind{type(button|ways), btn}`
  (`Kind::makeButton/makeWays`), `Style{type(pill|icon|round), name}` (`Style::makePill/makeIcon/makeRound`),
  `hit(Point)`, `hitFrame()`, `isTransport()`.
- `TouchLayout{size, landscape, gameArea, pad, faceButton, select, start, extras, overlayArea}`,
  `Options{showAABB, dpadSize, ways, fill, std::optional<double> pictureAspect, buttons, showRounds()}`,
  `extra(TouchButton)`, all static constants under the Swift names (`dpadDefault`, `dpadRangeLo/Hi`,
  `pictureAspect`, `lineHeight`, ..., `pictureMargin`), `islandMidY`, `transportOrder()`,
  `make(Size, TouchInsets)` / `make(Size, TouchInsets, const Options&)`, `textWidth`, `pillSize`,
  `switchSize`, `label(Kind, ways)`, `style(Kind)`, `transport(midX, bottom, width)`, `turboMetrics`,
  `turboDirection`, `turboOffset`, `turboAbove`; `operator==` on the Equatable types.

## Swift constructs that did not map 1:1

| Swift | C++ |
|---|---|
| `[weak self]` rAF closures | lambdas capturing `this`; the class is non-copyable so they stay valid |
| `enum Dir8: String` with `c` / `btns` computed properties | `enum class Dir8` + free functions `c()` / `btns()` |
| `Dictionary<Dir8, Set>` `DIR8_SETS` | `std::array<std::set<BBButton>,8>` indexed by enum value |
| `Set<BBButton>` + `ordered()` | `std::set<BBButton>`; sorted order equals `BBButton.allCases` order, so `ordered()` is implicit |
| `private(set) var` | public members, documented read-only |
| `CGRect/CGPoint/CGSize/CGVector` | `Rect/Point/Size/Vector`; `CGRect.null` from `intersection` becomes the empty rect |
| `class DpadPtr`, `class FaceRoll` (reference types) | `std::shared_ptr` in the maps |
| tuples `(btn, x, y)`, `(ax,ay,...)` | small structs `FaceLatch`, `FaceGeom`, `PadBox`, `PressAnim`, `RafCallback` |
| `for ... in dpadOrder` while the body edits it | iterates a copy (Swift arrays are values) |
| `TouchButton.inputID` / `isDirection` / `rawValue` / `init(_: BBButton)` | free functions `inputID`, `isDirection`, `rawValue`, `touchButton` |
| `TouchLayout.portrait(...)` / `landscape(...)` static funcs next to the `landscape` property | renamed `portraitLayout` / `landscapeLayout` (C++ cannot overload a member name with a function) |
| `make(... options: Options = Options())` | two overloads (a default argument of a nested type with default member initializers is rejected by clang inside the class) |
| `dpadRange: ClosedRange` | `dpadRangeLo` / `dpadRangeHi` |
| `UIFont` text measurement (`textWidth`) | `TouchLayout::textMeasurer` (std::function set by the host); until set a fixed estimate (0.66 em/char semibold) is used. Pill widths, and so SELECT/START/MENU/8-WAY placement, depend on it: for the 1e-4 comparison with iPhone data the host (or the test) must set a measurer returning the iPhone's widths |
| `String.count` | `characterCount()` (code points, variation selectors not counted) |
| SF Symbol names in `.icon(String)` | kept as plain strings (`Style::name`) |
| `Equatable` | `operator==` where used; `TouchLayout`, `Geometry`, `Options`, `TouchExtra` have it |
| `.rounded()` / `(.up)` / `(.down)` | `std::round` / `std::ceil` / `std::floor` |

Behaviour kept: `now` is the caller's ms clock (set by pointer events and `animationFrame`); callbacks
`press`, `onHaptic`, `onVisual`, `onAnimationFrameRequest` may be empty (checked before calling).
The sector math uses `kPi = 3.14159265358979323846` (same double as Swift `.pi`).

## Check

`c++ -std=c++17 -Wall -fsyntax-only -Iapps/desktop/src apps/desktop/src/touch/*.cpp` : exit 0, no output.
Also `-Wall -Wextra -Wno-unused-parameter`: clean (the CMake RNL_WARNINGS set minus -Wno-missing-field-initializers).
A throwaway smoke program (outside the repo) linked both .cpp files, made a 390x844 portrait layout,
tapped B and the D-pad: presses and releases came out as expected. Not compared against the iPhone data yet.
