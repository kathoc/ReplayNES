// --script: scripted UI checks on the device (screenshots of the real presented frames in Gaming
// Mode, where there is no keyboard). Commands separated by ';' run one after another:
//   wait <seconds>          menu | menu off        page <id> (a page of the menu model: quick, retry,
//                           practice, settings.display, controls.controller, ... along its path)
//   pad <button>            a controller press (down this frame, up the next) injected as SDL gamepad
//                           events of pad slot 0, so it goes the way a real press does (InputRouter:
//                           chord detector, hotkeys, game, menu; PadNavFeed: the UI). Buttons: a b x y
//                           (by position: a = south, b = east, x = west, y = north), ok / cancel (the UI's
//                           confirm / cancel: east / south by default), up down left right,
//                           l1 r1 l2 r2, menu (= start, ≡), view (= back, ⧉), l3 r3
//   padhold <button> <s>    the same, held for <s> seconds (the script goes on meanwhile)
//   chord [<ms>]            L1, then R1 <ms> later (default 30), both held 0.2 s: the Quick Menu chord
//   clickpill               a mouse click on the Menu pill (SDL mouse events at its center)
//   key <scancode name>     a key press (SDL_GetScancodeFromName: "Escape", "Return", "Space", ...)
//   osktype <text>          types <text> on the built-in on-screen keyboard
//   pause | play | record | bookmark | seta <slot> | setb <slot> | practice <slot> | stoppractice
//   seek <frame> | advance <n> | dialog <button> | shot <file.png> (shotdir <dir>: relative to it) | quit
//   status                  logs "script: status frame= length= take= takes= undo= recording= paused="
//   save | saveas | closeproject | resetprompt   (project actions; dialogs / chooser stay open)
//   exportdialog | exportstart | crt [off]       (MP4 export dialog / its Export button, CRT setting)
//   setting <key> <value>   diagramFamily auto|0..4, southConfirm 0|1, timelineSlot 0..7
//   update check|apply|later|restart            (in-app update: Check now, Update, Later, Restart)
//   updatewait <phase> <seconds>                 wait until the update phase is <phase> (UI::updatePhaseName)
//   layoutcheck [WxH ...]   every screen at the window size and the given sizes: nothing may overflow
//                           or scroll ("layoutcheck:" log lines; the exit code counts failures)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace rnl {

class UI;
class EmulationController;
class Renderer;
class AppModel;

class Script {
 public:
  Script(const std::string& text, UI* ui, EmulationController* emu, Renderer* renderer, AppModel* app);
  bool active() const { return next_ < cmds_.size() || !pending_.empty(); }
  /// Frame thread, once per frame before the UI is built.
  void step(double now);
  std::function<void()> onQuit;
  std::function<void(bool)> onCrt;
  /// The joystick id the injected gamepad events carry (slot 0's pad).
  std::function<SDL_JoystickID()> padId;
  /// The window injected mouse events go to.
  std::function<SDL_WindowID()> windowId;
  /// Layout check failures so far (the exit code of a scripted run).
  int failures = 0;

 private:
  std::vector<std::vector<std::string>> cmds_;
  size_t next_ = 0;
  double waitUntil_ = 0;
  std::function<bool()> waitCond_;  // updatewait / layoutcheck: until this holds (or waitUntil_)
  struct Pending {
    double at;
    std::string button;
    bool down;
  };
  std::vector<Pending> pending_;  // injected presses / releases (time)
  std::string shotDir_;           // "shot NAME" relative to it (shotdir)
  bool pushPad(const std::string& button, bool down);
  void pushMouse(float x, float y, bool down);
  UI* ui_;
  EmulationController* emu_;
  Renderer* renderer_;
  AppModel* app_;
};

}  // namespace rnl
