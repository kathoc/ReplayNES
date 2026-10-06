// --script: scripted UI checks on the device (screenshots of the real presented frames in Gaming
// Mode, where there is no keyboard). Commands separated by ';' run one after another:
//   wait <seconds>          menu | menu off        tab <playback|takes|bookmarks|practice|library|settings|guide>
//   settings <0-3>
//   pad <button>            a controller press (down this frame, up the next) injected as SDL gamepad
//                           events of pad slot 0, so it goes the way a real press does (InputRouter:
//                           hotkeys / game / R3 menu; PadNavFeed: the UI). Buttons: a b x y (by position:
//                           a = south, b = east, x = west, y = north), up down left right, l1 r1 l2 r2,
//                           menu (= start, ≡), view (= back, ⧉), l3 r3
//   padhold <button> <s>    the same, held for <s> seconds (the script goes on meanwhile)
//   pause | play | record | bookmark | seta <slot> | setb <slot> | practice <slot> | stoppractice
//   seek <frame> | advance <n> | dialog <button> | panel | shot <file.png> | quit
//   save | saveas | closeproject | resetprompt   (project actions; dialogs / chooser stay open)
//   exportdialog | exportstart | crt [off]       (MP4 export dialog / its Export button, CRT setting)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

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
  std::function<void(bool)> onCrt;
  /// The joystick id the injected gamepad events carry (slot 0's pad).
  std::function<SDL_JoystickID()> padId;

 private:
  std::vector<std::vector<std::string>> cmds_;
  size_t next_ = 0;
  double waitUntil_ = 0;
  struct Release {
    double at;
    std::string button;
  };
  std::vector<Release> releases_;  // injected presses to release (time)
  bool pushPad(const std::string& button, bool down);
  UI* ui_;
  EmulationController* emu_;
  VkRenderer* renderer_;
  AppModel* app_;
};

}  // namespace rnl
