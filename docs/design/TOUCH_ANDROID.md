# Touch controls on Android (the iPhone's, in the desktop frontend)

User request 2026-11-02: "iPhone 同等のタッチ操作に対応して" (Android: ReplayNES and ReplayArcade).

## Goal

The Android app (the desktop frontend: SDL3 + Vulkan + Dear ImGui) plays with the touch screen as the
iPhone app does: the same on-screen controller (NESTERM's Brickboy controller, as ported to Swift in
apps/ios on the ios-generalize branch), the same extra buttons, gestures, bindings and settings.

The iPhone's port is the reference: apps/ios/docs/TOUCH_PORT.md (what was ported from NESTERM and every
deliberate change, dated), apps/ios/Sources/Play/Brickboy/{TouchGamepad,TouchLayout,TouchControlsView,
TouchExtrasView}.swift, GameView.swift (PlaySurfaceView), AppModel.swift (touch settings, bindings,
step repeat), QuickMenuPages.swift (the Touch Controls page), Tests/TouchControlsTests.swift,
Tests/BrickboyGoldenTests.swift with Tests/Fixtures/golden-{portrait,landscape}.json (ios-generalize).

## Adopted

### 1. The logic and the layout in C++, function by function (apps/desktop/src/touch/)

- `touch_gamepad.{h,cpp}`: TouchGamepad.swift - every constant, DpadPtr, FaceRoll, the hit test
  (regionAt), the D-pad (sectors, touchdown fold, home follow / return, calibration), A / B (overlap,
  second-finger latch, roll tap), SELECT / START, releaseAll, the 60 Hz frame tick. Time in
  milliseconds from the caller (no clock of its own); press / haptic callbacks.
- `touch_layout.{h,cpp}`: TouchLayout.swift - portrait / landscape geometry in points from the surface
  size, safe-area insets, the picture's aspect and the options (D-pad size, buttons count, turbo
  buttons), the extras (MENU, pause, slow, 8-WAY, AA / BB, C / D), TouchButton names and
  TouchBindings (NES and arcade, `touch:<name>` -> actions).
- Same names as the Swift (and so as NESTERM's input.mjs), so the three can be read side by side.
- No SDL, ImGui or platform calls: pure C++17 in rnl_logic, unit tested on every platform.
- Licence: the Brickboy controller is Apache-2.0 (© 2026 Katsuya Hokamura), as in apps/ios
  (ThirdParty/brickboy, NOTICE): the ported files keep `SPDX-License-Identifier: Apache-2.0` and a
  provenance line; THIRD_PARTY_NOTICES.md names them. (Apache-2.0 code may be combined into this
  GPL-2.0-or-later program under GPL-3.0.)

### Deviations from the iPhone's

- TouchLayout::islandMidY keeps the slow button's centre at least minTarget / 2 (22 pt) from the top: with a
  top inset between 20 and 44 pt (an Android phone's 24 pt status bar) the iPhone's rule (half the inset)
  put its 44 pt touch area 6 pt above the screen (found by the layout test on a 412 x 915 phone). The
  iPhone's insets give the same places as before.

### 2. In the app (Android only, behind a platform flag)

- Shown when the platform has a touch screen (Android) and, by default, no controller is connected
  ("Hide With Controller", default on); hidden while the Quick Menu or a dialog is open; hiding
  releases everything. While hidden on a touch device: a ☰ menu button top right (as the iPhone).
- SDL finger events (SDL_EVENT_FINGER_DOWN / MOTION / UP / CANCELED, pixel coordinates from the
  normalised ones) drive TouchGamepad. Points = pixels / the display scale (density), as the layout is
  in points (CSS px). A finger the controller does not take reaches the picture (rewind / fast-forward
  quarters) or, if nothing, the UI as before.
- While the controller is shown, SDL's mouse events synthesised from touch (`which ==
  SDL_TOUCH_MOUSEID`) do not reach ImGui (the same finger would also click the UI under the pad).
- Pressed buttons go to the engine as `touch:<name>` (rn_input_set_pressed), bound by the iPhone's
  TouchBindings (NES: A / B / SELECT / START / turbo; arcade: B = p1.b1, A = p1.b2, C, D, coin, start,
  turbo of b1 / b2; L2 / R2 = rewind / fast-forward). A tap shorter than a frame still presses once
  (rn_input_sample_game, as now).
- The picture: the layout gives the picture's rectangle (portrait: on top, the full width; landscape:
  between the two columns); GameRect uses it instead of the whole window while the controller is shown.
- Gestures: the left / right quarter of the picture held = rewind / fast-forward (one finger each),
  marks for 3 s after a game starts; pause only by the pause button (no tap on the picture); MENU
  opens the Quick Menu; the slow button toggles 1/2 speed; 8-WAY toggles 8 / 4 directions (remembered
  per ROM).
- Drawn with ImGui's draw lists in the foreground, the iPhone's colours and shapes (BBColor, the
  pressed onset), the opacity setting applied to the whole controller.
- Haptics: Android's vibrator (a short `HapticFeedbackConstants` / `Vibrator` click through JNI), once per
  action as on the iPhone; setting "Vibration".

### 3. Settings > Controls > Touch Controls (shown only where touch controls exist)

The iPhone's items and defaults: Opacity (15-100 %, 100), D-pad Size (130-210, 144), D-pad Follow ↕ (0-2,
1.0) / ↔ (0-2, 0.3), D-pad Directions (8 / 4), Turbo Buttons (on), Vibration (on), Hide With Controller
(on). Saved in settings.txt like the other settings.

As built (2026-11-02): a page cannot hold more than RNF_MENU_MAX_ITEMS (6) rows, so two pages -
Touch Controls: Opacity, D-pad (a page), Turbo Buttons, Vibration, Hide With Controller; D-pad: Size,
Follow Up / Down, Follow Left / Right, D-pad Directions (the iPhone's ↕︎ / ↔︎ are not in the UI font: words
instead; Directions is one setting, not per game yet). RNF_MENU_FEATURE_TOUCH shows them; on that platform
Touch Controls takes the Keyboard's place on the Controls page, as on the iPhone (an arcade session adds the
Turbo page there, which made 7 rows; keys keep their defaults).

## Not adopted

- Porting from NESTERM's input.mjs again: the iPhone port already carries the changes since (roll tap,
  AA / BB, the arcade buttons, the extras' places); one reference, not two.
- A shared C API (rnf_touch_*) used by the iPhone too: the iPhone would have to be rebuilt on it; later,
  if worth it - the C++ has no dependencies to make that easy.
- Buttons drawn by Android Views over the SDL surface: two UIs to keep in step, no ImGui theme.
- An SDL virtual joystick fed by touch: cannot carry the roll tap, the moving D-pad home or per-region
  rules.
- Touch controls on Windows / Linux touch screens: not asked; the flag allows it later.

## Checked (2026-11-02)

- test_touch: the golden traces (portrait, landscape) match to 1e-4; a changed follow gain fails them. The
  layout rules on 8 iPhone and 2 Android screens.
- Android emulator (1080 x 2424, portrait), ReplayArcade, 1942: SELECT inserts a coin, START starts, the
  D-pad moves the plane, the left quarter rewinds; pause / resume; the seek bar above the controls; the
  settings pages (size 184, opacity 60 % seen in play); FILL puts the tall picture under the controls (as on
  the iPhone). Screenshots in the session's scratchpad.
- Not yet: a device in landscape (the Retroid Pocket 5 was not connected), vibration felt on a device,
  multi-touch on a device.

## Done when

- Golden: the C++ TouchGamepad replays golden-portrait.json and golden-landscape.json (the iPhone's
  fixtures, recorded from the unmodified input.mjs) with the same held mask, region and home offset
  after every step (±1e-4).
- Layout: the rules of TouchControlsTests (on screen, no overlaps, D-pad 1.1x at the A / B middle,
  AA / BB placement and hit areas, the round buttons per button count, the arcade bindings, landscape
  picture between the columns, D-pad size, every game button bound once).
- On the Retroid Pocket 5 (landscape) and the Android emulator in portrait: screenshots of the
  controller over a game; in play, the D-pad / A / B / START drive the game (seen in the picture).
