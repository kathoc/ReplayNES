# iPhone / Mac: controller kind (pad / arcade stick) and button layout, remembered per controller

2026-11-21, user request: 「接続された物理コントローラーに応じて、コントローラー種類の設定を記憶させたい」,
confirmed as **both** the pad / arcade stick kind and the face-button layout (Nintendo / Xbox / PlayStation).

## Today

- Desktop (Windows / Linux / Android, apps/desktop/src/input_router.cpp): another core's layout (ReplayArcade)
  keeps two binding profiles per game, `bindings-<layout>.json` (pad) and `bindings-<layout>@stick.json`
  (arcade stick, `rnf_input_bindings_filename_for_kind`). A controller's kind is detected from SDL's type
  and its name (`detectDeviceKind`), can be overridden per controller on the Controller page
  ("Kind: Automatic (Pad)" row: automatic → pad → stick → automatic), the override saved per GUID in
  `controller-kinds.json`. The engine plays a composition: each controller's bindings from its kind's
  profile, keyboard / globals from player 1's kind (`rebuildRuntime`). Edits go to the kind of the slot
  being edited.
- iPhone / Mac (apps/macos/Sources/App/InputManager.swift, shared): one profile per game (pad only, since
  2026-11-19 `bindings-<layout>.json`); no kind. The face-button layout (family) is derived from the
  controller's name and category each time it attaches (`ControllerFamily.from`, `rnf_gc_face_positions`);
  it cannot be changed.

## Adopted

1. **Kind detection shared**: the name rules move from the desktop to the frontend core
   (`rnf_controller_kind_from_name(name)`: "steam deck" → pad; arcade / fight stick / … → stick), used by
   the desktop's `detectDeviceKind` (after SDL's type) and by InputManager. One list, two platforms.
2. **Per-controller memory (Apple)**: a key per physical controller, `vendorName` + `productCategory`
   (GameController has no GUID). `controller-kinds.json` (same format as the desktop: key → "pad" | "stick")
   and `controller-families.json` (key → "nintendo" | "xbox" | "playstation"), next to the bindings in the
   product's Application Support folder. Absent = automatic.
3. **Two profiles (Apple, another core's layout only)**: as the desktop. InputManager keeps a pad and a
   stick profile (rn_input each), reads / saves each from its kind's file, plays the composition in
   `handle` (each `gc<slot>:` binding from that slot's kind, the rest from slot 1's kind). The UI's
   `config` and every edit (assign, reset, turbo) use the profile of the kind of the pad shown on the
   Controller page (its slot), else player 1's. The NES layout is unchanged (one profile, bindings.json).
4. **Family override (Apple, every layout)**: `attach` uses the override when set: the family, the
   labels, and the face positions (`rnf_gc_face_positions_for_family`: Nintendo = the printed-glyph rule,
   else Apple's diamond A bottom / B right / X left / Y top; PlayStation as Xbox). Changing it re-attaches
   the controller (positions and labels follow at once).
5. **Controller page (iPhone)**: two rows under the diagram, "Kind: Automatic (Pad)" (another core's layout
   only, as the desktop) and "Button Layout: Automatic (Nintendo)"; a tap or Confirm on the focused row
   cycles automatic → … → automatic. They are part of the page's D-pad focus (after the diagram's elements).
   The Mac's Controller page is not changed in this step (the Mac app is a first build; same rows later).

## Rejected

- Kind per game: the kind is the device's, so it is per controller; the bindings are per game and kind.
- A single merged profile with "stick:" prefixed ids: the desktop's two-profile format is already on disk
  for the same games (Steam Deck users); the same files keep the platforms interchangeable.

## Done when

- Frontend test: `rnf_controller_kind_from_name` (FlipPad pad, "8BitDo Arcade Stick" stick, "Steam Deck" pad);
  desktop tests unchanged.
- iOS unit tests: the composition (pad slot 1 + stick slot 2 take each profile's gc bindings), the overrides
  read / written, positions for an overridden family.
- Simulator (ReplayArcade, Gradius): set kind = stick on the Controller page, assign A = Button 3 (Turbo),
  restart: `bindings-fbneo%3Agradius@stick.json` has it, the pad file does not, the page shows it; set kind
  back to automatic: the pad bindings show. Family override: Xbox on the virtual pad moves A to the bottom.
- Real iPhone: both apps updated; the user checks the FlipPad.

## Result (2026-11-21)

- Frontend: `rnf_controller_kind_from_name`, `rnf_gc_face_positions_for_family` (test_frontend_input 31 cases,
  0 failures); the desktop's detectDeviceKind uses the shared rules (test_input_profiles 18, desktop 135: 0 failures).
- InputManager: pad / stick profiles + composition (rebuildRuntime), `edit` for every change, choices in
  `controller-kinds.json` / `controller-families.json` keyed "vendorName|productCategory".
- Simulator (ReplayArcade, Gradius, the simulator's "Gamepad" MFi): kind = stick (test hook) survives a
  restart; A = Button 1 then goes to `bindings-fbneo%3Agradius@stick.json` while the pad file keeps
  Button 3 (Turbo); the Controller page shows "Kind: Arcade Stick" and "Button Layout: Automatic (Other)";
  kind back to automatic: the action picker marks the pad's Button 3 (Turbo); layout = Nintendo survives a
  restart and moves A to the right (buttonA=face.east).
- macOS tests 128 (0 failures), l10n 0 problems; iOS tests 184, 2 failures = the known QuickMenuModelTests.

Differences from the plan: the composition and the overrides are checked end to end in the simulator, not
by iOS unit tests (InputManager is in the app, not in the unit-test target). Not done: the arcade stick
diagram on the Controller page (the desktop draws a stick for kind = stick; the iPhone keeps the pad's
picture), and the Mac's Controller page rows.
