// SPDX-License-Identifier: GPL-2.0-or-later
#include "script.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "app_model.h"
#include "emulation.h"
#include "imgui.h"
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

void Script::step(double now) {
  ImGuiIO& io = ImGui::GetIO();
  if (pendingRelease_ >= 0) {
    io.AddKeyEvent(ImGuiKey(pendingRelease_), false);
    pendingRelease_ = -1;
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
    } else if (op == "pad") {
      static const std::pair<const char*, ImGuiKey> keys[] = {
          {"up", ImGuiKey_GamepadDpadUp},     {"down", ImGuiKey_GamepadDpadDown}, {"left", ImGuiKey_GamepadDpadLeft},
          {"right", ImGuiKey_GamepadDpadRight}, {"a", ImGuiKey_GamepadFaceDown},  {"b", ImGuiKey_GamepadFaceRight},
          {"x", ImGuiKey_GamepadFaceLeft},    {"y", ImGuiKey_GamepadFaceUp},     {"l1", ImGuiKey_GamepadL1},
          {"r1", ImGuiKey_GamepadR1},         {"start", ImGuiKey_GamepadStart}};
      for (auto& [n, k] : keys)
        if (arg(1) == n) {
          io.AddKeyEvent(k, true);
          pendingRelease_ = int(k);
        }
      return;  // one key per frame
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
