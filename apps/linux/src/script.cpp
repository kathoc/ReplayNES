// SPDX-License-Identifier: GPL-2.0-or-later
#include "script.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "app_model.h"
#include "emulation.h"
#include "ui.h"
#include "vk_renderer.h"

namespace rnl {

Script::Script(const std::string& text, UI* ui, EmulationController* emu, VkRenderer* renderer, AppModel* app)
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

bool Script::pushPad(const std::string& name, bool down) {
  static const std::pair<const char*, int> buttons[] = {
      {"a", SDL_GAMEPAD_BUTTON_SOUTH},          {"b", SDL_GAMEPAD_BUTTON_EAST},
      {"x", SDL_GAMEPAD_BUTTON_WEST},           {"y", SDL_GAMEPAD_BUTTON_NORTH},
      {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},       {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
      {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},   {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
      {"l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
      {"menu", SDL_GAMEPAD_BUTTON_START},       {"start", SDL_GAMEPAD_BUTTON_START},
      {"view", SDL_GAMEPAD_BUTTON_BACK},        {"back", SDL_GAMEPAD_BUTTON_BACK},
      {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK},    {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
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

void Script::step(double now) {
  for (auto it = releases_.begin(); it != releases_.end();) {
    if (now >= it->at) {
      pushPad(it->button, false);
      it = releases_.erase(it);
    } else {
      ++it;
    }
  }
  while (next_ < cmds_.size()) {
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
    } else if (op == "tab") {
      static const std::pair<const char*, UI::Tab> tabs[] = {
          {"playback", UI::Tab::playback}, {"takes", UI::Tab::takes},     {"bookmarks", UI::Tab::bookmarks},
          {"practice", UI::Tab::practice}, {"library", UI::Tab::library}, {"settings", UI::Tab::settings},
          {"guide", UI::Tab::guide}};
      for (auto& [n, t] : tabs)
        if (arg(1) == n) ui_->selectTab(t);
    } else if (op == "settings") {
      if (onSettingsPage) onSettingsPage(std::atoi(arg(1, "0").c_str()));
    } else if (op == "pad" || op == "padhold") {
      if (pushPad(arg(1), true)) releases_.push_back({op == "pad" ? now : now + std::atof(arg(2, "1").c_str()), arg(1)});
      return;  // one press per frame (the release goes out with the next step)
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
    } else if (op == "panel") {
      ui_->showPracticePanel(true);
    } else if (op == "shot") {
      renderer_->requestScreenshot(arg(1, "screenshot.png"));
      return;  // this frame is the one saved
    } else if (op == "quit") {
      if (onQuit) onQuit();
    } else {
      std::fprintf(stderr, "script: unknown command %s\n", op.c_str());
    }
  }
}

}  // namespace rnl
