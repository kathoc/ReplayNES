// SPDX-License-Identifier: GPL-2.0-or-later
#include "script.h"

#include <cstdio>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "app_model.h"
#include "emulation.h"
#include "ui.h"
#include "renderer.h"

namespace rnl {

Script::Script(const std::string& text, UI* ui, EmulationController* emu, Renderer* renderer, AppModel* app)
    : ui_(ui), emu_(emu), renderer_(renderer), app_(app) {
  std::stringstream ss(text);
  std::string part;
  while (std::getline(ss, part, ';')) {
    std::istringstream ws(part);
    std::vector<std::string> words;
    std::string w;
    while (ws >> w) words.push_back(w);
    if (!words.empty()) cmds_.push_back(words);
  }
}

bool Script::pushPad(const std::string& logical, bool down) {
  // "ok" / "cancel": the UI's confirm / cancel buttons (Settings > Controls > Confirm Button).
  bool south = ui_ && ui_->southConfirm();
  std::string name = logical == "ok" ? (south ? "a" : "b") : logical == "cancel" ? (south ? "b" : "a") : logical;
  static const std::pair<const char*, int> buttons[] = {
      {"a", SDL_GAMEPAD_BUTTON_SOUTH},          {"b", SDL_GAMEPAD_BUTTON_EAST},
      {"x", SDL_GAMEPAD_BUTTON_WEST},           {"y", SDL_GAMEPAD_BUTTON_NORTH},
      {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},       {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
      {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},   {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
      {"l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
      {"menu", SDL_GAMEPAD_BUTTON_START},       {"start", SDL_GAMEPAD_BUTTON_START},
      {"view", SDL_GAMEPAD_BUTTON_BACK},        {"back", SDL_GAMEPAD_BUTTON_BACK},
      {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK},    {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
      {"home", SDL_GAMEPAD_BUTTON_GUIDE},
  };
  SDL_JoystickID id = padId ? padId() : 0;
  SDL_Event e;
  SDL_zero(e);
  e.common.timestamp = SDL_GetTicksNS();
  if (name == "l2" || name == "r2") {
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = id;
    e.gaxis.axis = name == "l2" ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    e.gaxis.value = down ? 32767 : 0;
    return SDL_PushEvent(&e);
  }
  for (auto& [n, b] : buttons)
    if (name == n) {
      e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
      e.gbutton.which = id;
      e.gbutton.button = Uint8(b);
      e.gbutton.down = down;
      return SDL_PushEvent(&e);
    }
  std::fprintf(stderr, "script: unknown pad button %s\n", name.c_str());
  return false;
}

void Script::pushMouse(float x, float y, bool down) {
  SDL_WindowID w = windowId ? windowId() : 0;
  SDL_Event e;
  SDL_zero(e);
  e.common.timestamp = SDL_GetTicksNS();
  e.type = SDL_EVENT_MOUSE_MOTION;
  e.motion.windowID = w;
  e.motion.x = x;
  e.motion.y = y;
  SDL_PushEvent(&e);
  SDL_zero(e);
  e.common.timestamp = SDL_GetTicksNS();
  e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  e.button.windowID = w;
  e.button.button = SDL_BUTTON_LEFT;
  e.button.down = down;
  e.button.clicks = 1;
  e.button.x = x;
  e.button.y = y;
  SDL_PushEvent(&e);
}

void Script::step(double now) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (now >= it->at) {
      if (it->button.rfind("mouse:", 0) == 0) {
        float x = 0, y = 0;
        std::sscanf(it->button.c_str() + 6, "%f,%f", &x, &y);
        pushMouse(x, y, it->down);
      } else if (it->button.rfind("key:", 0) == 0) {
        const SDL_Scancode sc = SDL_Scancode(std::atoi(it->button.c_str() + 4));
        SDL_Event e;
        SDL_zero(e);
        e.common.timestamp = SDL_GetTicksNS();
        e.type = it->down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = windowId ? windowId() : 0;
        e.key.scancode = sc;
        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.down = it->down;
        SDL_PushEvent(&e);
      } else if (it->button.rfind("rel:", 0) == 0 || it->button.rfind("mbutton:", 0) == 0) {
        SDL_Event e;
        SDL_zero(e);
        e.common.timestamp = SDL_GetTicksNS();
        if (it->button[0] == 'r') {  // relative motion (a captured mouse)
          e.type = SDL_EVENT_MOUSE_MOTION;
          e.motion.windowID = windowId ? windowId() : 0;
          std::sscanf(it->button.c_str() + 4, "%f,%f", &e.motion.xrel, &e.motion.yrel);
        } else {
          e.type = it->down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
          e.button.windowID = windowId ? windowId() : 0;
          e.button.button = Uint8(std::atoi(it->button.c_str() + 8));
          e.button.down = it->down;
          e.button.clicks = 1;
        }
        SDL_PushEvent(&e);
      } else {
        pushPad(it->button, it->down);
      }
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  while (next_ < cmds_.size()) {
    if (waitCond_) {
      bool met = waitCond_();
      if (!met && now < waitUntil_) return;
      std::fprintf(stderr, "script: wait %s\n", met ? "reached" : "timed out");
      waitCond_ = nullptr;
      waitUntil_ = 0;
    }
    if (now < waitUntil_) return;
    const std::vector<std::string>& c = cmds_[next_++];
    const std::string& op = c[0];
    auto arg = [&](size_t i, const char* def = "") { return i < c.size() ? c[i] : std::string(def); };
    std::fprintf(stderr, "script: %s %s\n", op.c_str(), arg(1).c_str());
    if (op == "wait") {
      waitUntil_ = now + std::atof(arg(1, "1").c_str());
      return;
    } else if (op == "menu") {
      ui_->setMenu(arg(1) != "off");
    } else if (op == "page") {
      if (!ui_->openPage(arg(1))) std::fprintf(stderr, "script: unknown page %s\n", arg(1).c_str());
    } else if (op == "chord") {
      // L1, then R1 a little later (either order works; the chord detector's window is 100 ms).
      double gap = std::atof(arg(1, "30").c_str()) / 1000.0;
      pushPad("l1", true);
      pending_.push_back({now + gap, "r1", true});
      pending_.push_back({now + gap + 0.2, "l1", false});
      pending_.push_back({now + gap + 0.2, "r1", false});
      return;
    } else if (op == "clickpill") {
      LRect r = ui_->pillRect();
      char b[64];
      std::snprintf(b, sizeof b, "mouse:%f,%f", double(r.x + r.w / 2), double(r.y + r.h / 2));
      pending_.push_back({now, b, true});
      pending_.push_back({now + 0.05, b, false});
      return;
    } else if (op == "clickcrumb") {  // a mouse click on breadcrumb segment <i> (0 = leftmost)
      LRect r = ui_->crumbRect(size_t(std::atoi(arg(1, "0").c_str())));
      if (r.w <= 0) {
        std::fprintf(stderr, "script: clickcrumb: no such segment\n");
        continue;
      }
      char b[64];
      std::snprintf(b, sizeof b, "mouse:%f,%f", double(r.x + r.w / 2), double(r.y + r.h / 2));
      pending_.push_back({now, b, true});
      pending_.push_back({now + 0.05, b, false});
      return;
    } else if (op == "key") {
      SDL_Scancode sc = SDL_GetScancodeFromName(arg(1).c_str());
      if (sc == SDL_SCANCODE_UNKNOWN) {
        std::fprintf(stderr, "script: unknown key %s\n", arg(1).c_str());
        continue;
      }
      const double held = std::atof(arg(2, "0").c_str());  // [<seconds>]: held, released later
      pending_.push_back({now, "key:" + std::to_string(int(sc)), true});
      pending_.push_back({now + held, "key:" + std::to_string(int(sc)), false});
      return;
    } else if (op == "focus") {  // lost | gained: the window's input focus (another window in front)
      SDL_Event e;
      SDL_zero(e);
      e.common.timestamp = SDL_GetTicksNS();
      e.type = arg(1, "lost") == "gained" ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
      e.window.windowID = windowId ? windowId() : 0;
      SDL_PushEvent(&e);
      return;
    } else if (op == "mousemove") {  // <dx> <dy> [<frames>]: relative motion, spread over frames (60 Hz)
      const int n = std::max(1, std::atoi(arg(3, "1").c_str()));
      const double dx = std::atof(arg(1, "0").c_str()) / n, dy = std::atof(arg(2, "0").c_str()) / n;
      char b[64];
      std::snprintf(b, sizeof b, "rel:%f,%f", dx, dy);
      for (int i = 0; i < n; ++i) pending_.push_back({now + i / 60.0, b, true});
      return;
    } else if (op == "mousebutton") {  // left|right|middle [<seconds held>]
      const std::string which = arg(1, "left");
      const int button = which == "right" ? SDL_BUTTON_RIGHT : which == "middle" ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT;
      const std::string b = "mbutton:" + std::to_string(button);
      pending_.push_back({now, b, true});
      pending_.push_back({now + std::max(0.05, std::atof(arg(2, "0.05").c_str())), b, false});
      return;
    } else if (op == "wheel") {  // mouse wheel notches (SDL event): <y> (+ = up / away), [<x>]
      SDL_Event e;
      SDL_zero(e);
      e.common.timestamp = SDL_GetTicksNS();
      e.type = SDL_EVENT_MOUSE_WHEEL;
      e.wheel.windowID = windowId ? windowId() : 0;
      e.wheel.y = float(std::atof(arg(1, "0").c_str()));
      e.wheel.x = float(std::atof(arg(2, "0").c_str()));
      e.wheel.integer_y = int(e.wheel.y);
      e.wheel.integer_x = int(e.wheel.x);
      e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
      SDL_PushEvent(&e);
      return;
    } else if (op == "layoutcheck") {
      std::vector<std::pair<int, int>> sizes;
      for (size_t i = 1; i < c.size(); ++i) {
        int w = 0, h = 0;
        if (std::sscanf(c[i].c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) sizes.push_back({w, h});
      }
      ui_->startLayoutCheck(sizes);
      waitUntil_ = now + 120;
      waitCond_ = [this] {
        if (ui_->layoutCheckRunning()) return false;
        failures += ui_->layoutFailures();
        return true;
      };
      return;
    } else if (op == "osktype") {
      // Types the rest of the command on the built-in keyboard: its presses, one per frame.
      std::string text;
      for (size_t i = 1; i < c.size(); ++i) text += (i > 1 ? " " : "") + c[i];
      std::vector<std::string> presses = ui_->oskPresses(text);
      if (presses.empty()) std::fprintf(stderr, "script: osktype: cannot type \"%s\"\n", text.c_str());
      std::vector<std::vector<std::string>> ins;
      for (const std::string& b : presses) ins.push_back({"pad", b});
      cmds_.insert(cmds_.begin() + long(next_), ins.begin(), ins.end());
    } else if (op == "pad" || op == "padhold") {
      if (pushPad(arg(1), true)) pending_.push_back({op == "pad" ? now : now + std::atof(arg(2, "1").c_str()), arg(1), false});
      return;  // one press per frame (the release goes out with the next step)
    } else if (op == "setting") {
      if (!ui_->scriptSetting(arg(1), arg(2))) std::fprintf(stderr, "script: unknown setting %s\n", arg(1).c_str());
    } else if (op == "pause") {
      emu_->setPaused(true);
    } else if (op == "play") {
      emu_->setPaused(false);
    } else if (op == "record") {
      emu_->toggleRecord();
    } else if (op == "bookmark") {
      emu_->addBookmark();
    } else if (op == "seta") {
      emu_->timelineMarkA(std::atoi(arg(1, "0").c_str()));
    } else if (op == "setb") {
      emu_->timelineMarkB(std::atoi(arg(1, "0").c_str()));
    } else if (op == "practice") {
      emu_->startPractice(std::atoi(arg(1, "0").c_str()));
    } else if (op == "stoppractice") {
      emu_->stopPractice();
    } else if (op == "seek") {
      emu_->seek(std::strtoull(arg(1, "0").c_str(), nullptr, 10));
    } else if (op == "advance") {
      emu_->frameAdvance(std::atoi(arg(1, "1").c_str()));
    } else if (op == "status") {
      const EmuStatus st = emu_->status();
      std::fprintf(stderr, "script: status frame=%llu length=%llu take=%llu takes=%zu undo=%zu recording=%d paused=%d page=%s mouse=%d section=%d practicing=%d\n",
                   (unsigned long long)st.frame, (unsigned long long)st.takeLength, (unsigned long long)st.activeTake,
                   st.takeCount, st.undoDepth, st.recording ? 1 : 0, st.paused ? 1 : 0, ui_->shownMenuPage().c_str(),
                   mouseCaptured && mouseCaptured() ? 1 : 0, ui_->selectedSection(), st.practicing ? 1 : 0);
    } else if (op == "dialog") {
      ui_->answerDialog(std::atoi(arg(1, "0").c_str()));
    } else if (op == "resetprompt") {
      app_->resetProjectPrompt();
    } else if (op == "saveas") {
      app_->saveAs();
    } else if (op == "save") {
      app_->save();
    } else if (op == "closeproject") {
      app_->closeProject();
    } else if (op == "exportdialog") {
      ui_->openExportDialog();
    } else if (op == "exportstart") {
      ui_->startExport();
    } else if (op == "crt") {
      if (onCrt) onCrt(arg(1) != "off");
    } else if (op == "shotdir") {
      shotDir_ = arg(1);
    } else if (op == "shot") {
      std::string name = arg(1, "screenshot.png");
      bool absolute = !name.empty() && (name[0] == '/' || name[0] == '\\' || (name.size() > 1 && name[1] == ':'));
      renderer_->requestScreenshot(shotDir_.empty() || absolute ? name : shotDir_ + "/" + name);
      return;  // this frame is the one saved
    } else if (op == "update") {
      if (!ui_->scriptUpdate(arg(1))) std::fprintf(stderr, "script: update %s: not possible now\n", arg(1).c_str());
    } else if (op == "updatewait") {
      std::string want = arg(1, "available");
      waitUntil_ = now + std::atof(arg(2, "60").c_str());
      waitCond_ = [this, want] { return ui_->updatePhaseName() == want; };
    } else if (op == "quit") {
      if (onQuit) onQuit();
    } else {
      std::fprintf(stderr, "script: unknown command %s\n", op.c_str());
    }
  }
}

}  // namespace rnl
