ReplayNES for Windows (preview)
================================

ReplayNES records every frame you play: rewind, replay, re-record from any point, practice
sections with A/B repeat, takes and bookmarks. Windows 10 / 11, x64 or ARM64 (arm64 build for
Windows on Arm). No installation: unzip into a folder you can write to (e.g. in your user folder,
not Program Files: updates replace the files there) and run ReplayNES.exe.

Files
  ReplayNES.exe            the app (SDL3 is built in)
  WinSparkle.dll           in-app updates (WinSparkle, MIT)
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
    Menu = START, View = SELECT, L+R together (LB+RB) = Quick Menu, R = pause (seek bar),
    L = slow 1/2, L2 hold = rewind, R2 hold = fast-forward. Paused: L / R step frames, A (tap)
    resumes, B drops an A/B marker (up = to the markers). Menus: B confirms, A goes back
    (Settings > Controls > Confirm Button swaps them).
  Keyboard: arrows, X = A, Z = B, S / A = turbo, Enter = START, right Shift = SELECT, Space pause,
    Backspace rewind, Tab fast-forward, L slow, comma / period step, B bookmark, Esc / F1 Quick
    Menu, F11 full screen, F3 statistics. In menus: arrows, Enter, Backspace (back), Page Up / Down.
  The "Menu" pill in the top-right corner is a button too (mouse / touch). Everything can be
  reassigned in Settings > Controls.

Display and timing
  Direct3D 11. The emulation is locked to the display's refresh (60 Hz every refresh, 120 Hz
  every 2nd, ...) with input sampled just before each frame; the audio follows with dynamic rate
  control. Full screen (F11) is borderless; on a variable refresh (G-SYNC / FreeSync) display,
  start with --vrr for tearing-allowed presents at the NES's own 60.0988 Hz.

CRT display
  Settings > Display > CRT: nesterm's physical CRT model (NES signal > RF > TV tube) on
  Direct3D 11 compute (a Direct3D 11.0 GPU). Display only: recordings are unaffected.

MP4 export
  Quick Menu > Share > Export MP4: H.264 + AAC through Windows' Media Foundation (a hardware encoder when the GPU
  has one), into Documents\ReplayNES\Exports. Optional CRT effect and flash reduction.

Updates
  On the second start ReplayNES asks whether to check for updates automatically (once a day, on
  GitHub). Settings > System > Updates > Check Now checks right away. Updates are
  signed; ReplayNES quits, replaces its files in this folder and starts the new version (the
  session is saved first).

Command line
  ReplayNES.exe --help    (--rom FILE, --fullscreen, --lang ja|en, --vrr, --log FILE, ...)

Source code: https://github.com/kathoc/ReplayNES
