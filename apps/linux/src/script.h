// --script: scripted UI checks on the device (screenshots of the real presented frames in Gaming
// Mode, where there is no keyboard). Commands separated by ';' run one after another:
//   wait <seconds>          menu | menu off        tab <playback|takes|bookmarks|practice|library|settings|guide>
//   settings <0-3>          pad <up|down|left|right|a|b|x|y|l1|r1|start> (ImGui gamepad navigation)
//   pause | play | record | bookmark | seta <slot> | setb <slot> | practice <slot> | stoppractice
//   seek <frame> | advance <n> | dialog <button> | panel | shot <file.png> | quit
//   save | saveas | closeproject | resetprompt   (project actions; dialogs / chooser stay open)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace rnl {

class UI;
class EmulationController;
class VkRenderer;
class AppModel;

class Script {
 public:
  Script(const std::string& text, UI* ui, EmulationController* emu, VkRenderer* renderer, AppModel* app);
  bool active() const { return next_ < cmds_.size(); }
  /// Frame thread, once per frame before the UI is built.
  void step(double now);
  std::function<void()> onQuit;
  std::function<void(int)> onSettingsPage;

 private:
  std::vector<std::vector<std::string>> cmds_;
  size_t next_ = 0;
  double waitUntil_ = 0;
  int pendingRelease_ = -1;  // ImGuiKey to release on the next frame
  UI* ui_;
  EmulationController* emu_;
  VkRenderer* renderer_;
  AppModel* app_;
};

}  // namespace rnl
