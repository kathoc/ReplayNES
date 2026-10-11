// User settings of the desktop frontend (Linux: $XDG_CONFIG_HOME/ReplayNES/linux-settings.ini,
// Windows: %APPDATA%\ReplayNES\settings.ini; key=value).
// The macOS app keeps the same preferences in UserDefaults (AppModel.swift). Parsing is pure and
// tolerant: unknown keys are ignored, out-of-range values fall back to the defaults.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace rnl {

struct Settings {
  // Display
  bool integerScale = true;  // Integer (largest integer scale) vs FILL
  bool par87 = false;
  bool hideOverscan = true;
  bool showStats = false;
  int flash = 2;  // rn_flash_level: 0 off, 1 low, 2 standard (default: safety first), 3 high
  bool showFlashIndicator = true;
  bool vtrEffect = true;  // the VTR tape look while rewinding / returning to A (rnf_vtr; display only)
  float uiScale = 1.0f;  // multiplies the automatic scale (window height / 800)
  // CRT display (nesterm physical model; display only). Defaults are nesterm's.
  bool crt = false;
  int crtLines = 240;  // 240, or 110...220 (reduced-line experiment)
  bool crtBeamGrowth = true;
  bool crtPersistence = true;
  bool crtSupply = true;
  double crtAntenna = 65.0;  // dBuV, 20...90
  // Audio
  float volume = 0.8f;
  // Controls
  // Off by default since 0.5.1. Saved as "pauseAfterRewind2": up to 0.5.0 every settings file
  // stored the old default (on) under "pauseAfterRewind" whether or not it was chosen, so that
  // key is ignored = everyone starts from the new default once (a stored "off" was the only
  // value that was surely chosen, and it stays off).
  bool pauseAfterRewind = false;
  double autosaveInterval = 2.0;  // seconds
  bool dpadStepWhenPaused = true;
  bool practiceCountdown = true;  // 3, 2, 1 after the practice run returned to A (Practice page, View)
  // 3, 2, 1 before play resumes from a pause (System > More); independent of practiceCountdown.
  bool resumeCountdown = true;
  // UI confirm / cancel (rnf_ui_confirm_element): false = east confirms, south goes back (default).
  bool southConfirm = false;
  // Text fields: "auto" (Steam's keyboard where it can be asked for, else the built-in one),
  // "builtin" (ReplayNES's controller keyboard), "steam" (Steam's only). See osk.h.
  std::string onScreenKeyboard = "auto";
  // System
  bool checkForUpdates = true;  // keep the Flatpak portal's update monitor open (update_service.h)
  std::string language = "auto";  // "auto" (the system's), "ja", "en"
  bool menuHintShown = false;     // the Menu pill pulsed on the first launch
  // Input layout version of the saved bindings (RNF_CONTROLLER_LAYOUT_VERSION; 0 = never saved).
  int controllerLayoutVersion = 0;
  // UI state worth keeping
  int diagramSlot = 0;
  std::string diagramFamily = "auto";  // "auto" or a family index (0..4)
  int timelineSlot = 0;                // A/B slot edited on the timeline

  static Settings parse(const std::string& text);
  std::string serialize() const;

  bool load(const std::string& path);  // false if the file is missing (defaults kept)
  bool save(const std::string& path) const;  // atomic (temp file + rename)
};

}  // namespace rnl
