ReplayNES for Windows (preview)
================================

ReplayNES records every frame you play: rewind, replay, re-record from any point, practice
sections with A/B repeat, takes and bookmarks. Windows 10 / 11, x64 or ARM64 (arm64 build for
Windows on Arm). No installation: unzip anywhere and run ReplayNES.exe.

Files
  ReplayNES.exe            the app (self-contained; SDL3 is built in, no DLLs)
  LICENSE.txt              GPL-2.0-or-later
  THIRD_PARTY_NOTICES.md   licenses of the components (Nestopia UE, SDL3, Dear ImGui, ...)

Where things are
  ROMs       Documents\ReplayNES\ROM        (put your own .nes files here; "Open Folder" in the
                                             library shows it in Explorer)
  Projects   Documents\ReplayNES\Projects   (one .nesrec folder per project, autosaved)
  Session    %LOCALAPPDATA%\ReplayNES\Session  (the temporary session, resume record)
  Settings   %APPDATA%\ReplayNES\settings.ini, bindings.json
  "Reset Project" and Save As (replacing a project) move the old project to the Recycle Bin.

Controls
  Xbox-style controllers (XInput / GameInput), PlayStation and Switch Pro controllers through SDL.
    D-pad / left stick = NES D-pad, A = NES B, B = NES A (by position), X / Y = turbo,
    Menu = START, View = SELECT, L2 hold = rewind, R2 hold = fast-forward, L1 = slow 1/2,
    R1 / R3 = pause + menu.
  Keyboard: arrows, X = A, Z = B, S / A = turbo, Enter = START, right Shift = SELECT, Space pause,
    Backspace rewind, Tab fast-forward, L slow, comma / period step, B bookmark, Esc / F1 menu,
    F11 full screen, F3 statistics.
  Everything can be reassigned in Settings. The Controls Guide (View button) lists all of it.

Display and timing
  Direct3D 11. The emulation is locked to the display's refresh (60 Hz every refresh, 120 Hz
  every 2nd, ...) with input sampled just before each frame; the audio follows with dynamic rate
  control. Full screen (F11) is borderless; on a variable refresh (G-SYNC / FreeSync) display,
  start with --vrr for tearing-allowed presents at the NES's own 60.0988 Hz.

Not in this preview
  CRT display and MP4 export (the Linux / macOS versions have them), in-app updates.

Command line
  ReplayNES.exe --help    (--rom FILE, --fullscreen, --lang ja|en, --vrr, --log FILE, ...)

Source code: https://github.com/kathoc/ReplayNES
