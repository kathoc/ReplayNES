// Interface between the UI and the MP4 export (apps/linux/src/export/, owned by the export port:
// FFmpeg H.264/HEVC + AAC like the macOS MP4Exporter). The menu shows "Export…" and calls
// exportOpen(); while the menu is visible the UI calls exportDrawUI() every frame so the export
// can draw its own ImGui dialog / progress. rn_renderer_new must be called on the frame thread
// (exportOpen / exportDrawUI run there); encoding belongs on a worker thread.
// export_hook_stub.cpp is the placeholder until the export lands (replace it in CMakeLists.txt).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "replaynes/replaynes.h"

namespace rnl {

struct ExportHost {
  rn_session* session = nullptr;  // the current session (frame thread)
  uint64_t takeLength = 0;
  std::string romName;            // default file name stem
  std::string outputDir;          // ~/Documents/ReplayNES (the sandbox can write there)
  rn_flash_level flashLevel = RN_FLASH_STANDARD;
  bool par87 = false;             // display settings as defaults
  bool hideOverscan = true;
  float uiScale = 1.0f;
  std::function<void(const std::string&)> notice;
};

/// The menu entry is enabled (an exporter is built in and the take is not empty).
bool exportAvailable(const ExportHost& host);
/// "Export…" chosen.
void exportOpen(const ExportHost& host);
/// Every UI frame while the menu is visible (dialogs, progress). Returns true while the export UI
/// is showing (it then has the gamepad focus).
bool exportDrawUI(const ExportHost& host);

}  // namespace rnl
