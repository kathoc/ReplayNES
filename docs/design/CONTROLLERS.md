# Controllers out of the box (and the 8BitDo FlipPad)

User requests 2026-11-17: "support the 8BitDo FlipPad" and "support many controllers from the start".
The user left the design to us ("まかせる").

## What the apps do today (2026-11-18, read in the code)

- Windows, Linux / Steam Deck, Android (the shared desktop frontend): SDL 3.4.18. `InputRouter::syncPads`
  takes only what `SDL_GetGamepads` lists, i.e. devices SDL has a gamepad mapping for: its built-in table,
  XInput, its HIDAPI drivers, and on Android every input device that reports gamepad buttons. A joystick
  SDL has no mapping for is ignored, whatever buttons it has. No external mapping table is loaded.
- Face buttons whose positions are not certain are checked once per model by the user (FACE_CHECK.md).
- iPhone / Mac: Apple's GameController framework (MFi, Xbox, PlayStation, Switch and the controllers Apple
  supports, which includes the FlipPad from iOS 26.2).

## The 8BitDo FlipPad

A USB-C controller that folds over the lower half of a phone held upright (portrait): D-pad, A B X Y,
Select, Start, L1 L2 R1 R2. iOS 26.2+ (supported by Apple), Android 13+. No battery, no Bluetooth.
(retrododo.com review, 9to5google.com, notebookcheck.net, read 2026-11-17; the manual page refused us.)

Problem found in the code: on Android, a connected controller hides the touch controls (Settings: "hide
with a controller", on by default) and the picture area of the touch layout was then dropped, so the
picture was centred on the whole screen. On a 19.5:9 phone in portrait an NES picture then spans about
28 % to 72 % of the height: the FlipPad over the lower half hides the lower half of the picture. The iPhone
app keeps the touch layout's picture area when the controls are hidden (GameView: `game.frame =
TouchLayout.make(...).gameArea`), so the picture stays at the top there.

### Adopted

- Android, like the iPhone: the touch layout is computed whether or not the controls are shown, and in
  portrait its picture area (the top of the screen under the safe area, full width) is used also when the
  controls are hidden (by a controller, or by a menu). Landscape with the controls hidden: the whole
  screen, as before (the layout's area between the control columns would only make the picture smaller).
- Side effect, wanted: opening a menu in portrait no longer moves the picture from the top to the middle.

### Not adopted

- Detecting the FlipPad by name and only then moving the picture: any controller with a phone held upright
  wants the picture where the hands are not; the iPhone app already does it for all.

## Many controllers from the start (Windows, Linux / Steam Deck, Android)

The community table SDL_GameControllerDB (github.com/mdqinc/SDL_GameControllerDB, zlib licence, updated
weekly) maps controllers SDL does not know by itself. Commit 4e87e3c6c257 (2026-11-17): 873 Windows, 752 Linux,
299 Android mappings, of which 561 / 480 / 251 GUIDs are not in SDL 3.4.18's own table (an upper bound of what it
adds: XInput and SDL's HIDAPI drivers already cover many controllers without a mapping).

### Adopted

- The table is kept in the repository (third_party/gamecontrollerdb: the file at a pinned commit, its licence,
  the commit in a VERSION file; scripts/update-gamecontrollerdb.sh replaces it) and built into the app: CMake
  takes only the lines of the target platform (Windows / Linux / Android) and makes them a byte array, so no
  platform needs a packaging step (zip, Flatpak, APK assets) for it.
- At start-up, before the controllers are listed, the app adds it with SDL_AddGamepadMappingsFromIO, then a
  user's own `gamecontrollerdb.txt` in the settings folder (a newer table, or a line for one controller,
  without waiting for an app update). The counts are logged ("controllers: ...").
- Licence: THIRD_PARTY_NOTICES.md.
- The table is updated before a release (the script), like any other pinned third-party source.

### Not adopted (now)

- Guessing a mapping for a joystick that no table knows (buttons 0..3 as A B X Y and so on): the Select /
  Start / shoulder numbers differ from device to device, and a wrong guess is worse than nothing. A guided
  button setup for such a controller is the next step if a user meets one.
- Fetching the table at run time: no network use for this; a newer table can be dropped in by hand.
- macOS / iPhone: Apple's GameController decides which controllers exist; there is nothing to add there.

## The 8BitDo FlipPad: checks

- `TouchLayout::pictureArea(controlsShown)` (touch_layout.h; TouchControls::pictureArea uses it), test_touch
  "the picture's place with the controls hidden": for every device size of the layout tests, portrait with the
  controls hidden keeps the game area at the top under the safe area and ends above half the screen's height;
  landscape hidden gives no area.
- Android emulator (Android 15, portrait), the Test Cartridge, then the Quick Menu (it hides the touch controls
  as a controller does): 0.6.7 moved the picture to the middle (about 34 % to 66 % of the height); with the
  change it stays where it is with the controls (about 11 % to 43 %). No controller can be attached to the
  emulator, so the hiding by a controller itself was not exercised; it is the same `show = false`.

### Checked (2026-11-18): the built-in table

- test_input_profiles "controllers: the built-in SDL_GameControllerDB and the user's file" (macOS, SDL 3.4.18):
  the Tomee NES Adapter (03000000571d00002100000021000000), in the table and not in SDL's own, has no mapping
  before and "Tomee NES Adapter" after; one line in a user's file is added (1 new).
- Start-up logs: Windows 11 ARM64 VM "controllers: SDL_GameControllerDB 4e87e3c6c257 (Windows): 577 new of 873
  mappings (the rest SDL knew)"; Android 15 emulator "(Android): 253 new of 299"; macOS test "(Mac OS X): 194 new
  of 331". The Linux (aarch64) Flatpak builds; not started (the VM has no display).
- Not checked: a controller that only the table knows, plugged in for real (no such device here).

## Done when

- Table part: the platform's lines are built in and added at start-up (log), a test adds the built-in table to
  SDL and finds a controller that only the table knows, a user file is added after it; the Windows, Linux and
  Android builds pass.
- FlipPad part: on Android in portrait with a controller connected, the picture is in the top area (the
  touch layout's), in landscape the whole screen; checked on an emulator with a virtual controller and by a
  test of the area; the iPhone keeps its behaviour.
