// User settings of the Linux frontend ($XDG_CONFIG_HOME/ReplayNES/linux-settings.ini, key=value).
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
  bool pauseAfterRewind = true;
  double autosaveInterval = 2.0;  // seconds
  bool dpadStepWhenPaused = true;
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
