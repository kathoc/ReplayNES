// Modal popups: dialogs (DialogHost), the file chooser and the rename field. ImGui navigation
// (A / B); styled like the menus (UI::updateScale sets the style).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include "imgui_internal.h"
#include "l10n.h"
#include "paths.h"
#include "ui.h"

namespace fs = std::filesystem;

namespace rnl {

void UI::buildDialogs() {
  if (dialogs_.empty()) return;
  ImGuiIO& io = ImGui::GetIO();
  if (!dialogOpened_) {
    ImGui::OpenPopup("##dialog");
    dialogOpened_ = true;
  }
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(S(520), io.DisplaySize.x * 0.9f), 0),
                                      ImVec2(std::min(S(860), io.DisplaySize.x * 0.95f), io.DisplaySize.y * 0.9f));
  int result = -1;
  bool checked = dialogs_.front().checked;
  if (ImGui::BeginPopupModal("##dialog", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoMove)) {
    Dialog& d = dialogs_.front();
    float wrap = std::min(S(820), io.DisplaySize.x * 0.9f);
    ImGui::PushFont(nullptr, metrics_.title());
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextUnformatted(d.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    if (!d.message.empty()) {
      ImGui::Spacing();
      ImGui::PushTextWrapPos(wrap);
      ImGui::TextUnformatted(d.message.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    if (!d.checkbox.empty()) ImGui::Checkbox(d.checkbox.c_str(), &d.checked);
    checked = d.checked;
    ImGui::Spacing();
    for (size_t i = 0; i < d.buttons.size(); ++i) {
      if (i) ImGui::SameLine();
      if (int(i) == std::clamp(d.defaultIndex, 0, int(d.buttons.size()) - 1) && ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(true);
      }
      bool destructive = int(i) == d.destructiveIndex;
      if (destructive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1));
      if (ImGui::Button(d.buttons[i].c_str(), ImVec2(std::max(S(120), ImGui::CalcTextSize(d.buttons[i].c_str()).x + S(28)), 0)))
        result = int(i);
      if (destructive) ImGui::PopStyleColor();
    }
    inlinePrompts(d.buttons.size() > 1 ? TR("Cancel") : TR("Close"));
    int cancel = d.cancelIndex >= 0 ? d.cancelIndex : int(d.buttons.size()) - 1;
    if (result < 0 && !ImGui::IsWindowAppearing() &&
        (ImGui::IsKeyPressed(cancelKey(), false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
      result = cancel;
    if (dialogAnswer_ >= 0) {
      result = std::min(dialogAnswer_, int(d.buttons.size()) - 1);
      dialogAnswer_ = -1;
    }
    if (result >= 0) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (result >= 0) {
    Dialog d = std::move(dialogs_.front());
    dialogs_.pop_front();
    dialogOpened_ = false;
    if (d.onResult) d.onResult(result, checked);
  }
}

// ------------------------------------------------------------------ file chooser

void UI::chooserList() {
  Chooser& c = *chooser_;
  c.entries.clear();
  std::error_code ec;
  for (fs::directory_iterator it(c.dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::string n = it->path().filename().string();
    if (n.empty() || n[0] == '.') continue;
    bool dir = it->is_directory(ec);
    std::string lower = n;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    bool nesrec = lower.size() > 7 && lower.compare(lower.size() - 7, 7, ".nesrec") == 0;
    bool nes = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".nes") == 0;
    if (c.req.mode == ChooserRequest::Mode::openROM && !dir && !nes) continue;
    if (c.req.mode != ChooserRequest::Mode::openROM && !dir) continue;
    if (c.req.mode == ChooserRequest::Mode::openROM && nesrec) continue;
    c.entries.push_back({n, dir && !nesrec});
  }
  std::sort(c.entries.begin(), c.entries.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second;  // folders first
    std::string x = a.first, y = b.first;
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    std::transform(y.begin(), y.end(), y.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return x < y;
  });
}

void UI::buildChooser() {
  if (!chooser_ || !dialogs_.empty()) return;
  Chooser& c = *chooser_;
  ImGuiIO& io = ImGui::GetIO();
  if (!c.opened) {
    ImGui::OpenPopup("##chooser");
    c.opened = true;
  }
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(S(900), io.DisplaySize.x * 0.95f), std::min(S(640), io.DisplaySize.y * 0.92f)));
  bool done = false, cancelled = false;
  std::string chosen;
  if (ImGui::BeginPopupModal("##chooser", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoResize)) {
    ImGui::PushFont(nullptr, metrics_.title());
    ImGui::TextUnformatted(c.req.title.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s", Paths::display(c.dir).c_str());
    bool save = c.req.mode == ChooserRequest::Mode::saveProject;
    float footer = ImGui::GetFrameHeightWithSpacing() * (save ? 3.2f : 1.6f);
    ImGui::BeginChild("##entries", ImVec2(0, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    bool appearing = ImGui::IsWindowAppearing();
    std::string go;
    if (rnf_paths_equal(c.dir.c_str(), c.req.root.c_str()) == 0) {
      if (appearing && !save) ImGui::SetKeyboardFocusHere();
      if (ImGui::Selectable("..", false, 0, ImVec2(0, S(36)))) go = fs::path(c.dir).parent_path().string();
    }
    for (size_t i = 0; i < c.entries.size(); ++i) {
      const auto& [n, isDir] = c.entries[i];
      if (i == 0 && appearing && !save && go.empty()) ImGui::SetKeyboardFocusHere();
      std::string label = isDir ? "[" + n + "]" : n;
      ImGui::PushID(int(i));
      if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, S(36)))) {
        std::string full = c.dir + "/" + n;
        if (isDir) {
          go = full;
        } else if (save) {
          std::string stemName = n.size() > 7 ? n.substr(0, n.size() - 7) : n;
          std::snprintf(c.name, sizeof c.name, "%s", stemName.c_str());
          c.focusName = true;
        } else {
          chosen = full;
          done = true;
        }
      }
      ImGui::PopID();
    }
    if (c.entries.empty()) ImGui::TextDisabled("%s", TR("(empty)"));
    ImGui::EndChild();
    if (!go.empty()) {
      c.dir = go;
      chooserList();
    }
    if (save) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(TR("Name"));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(".nesrec").x - S(16));
      if (appearing || c.focusName) {
        ImGui::SetKeyboardFocusHere();
        c.focusName = false;
      }
      bool enter = ImGui::InputText("##name", c.name, sizeof c.name, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      ImGui::TextDisabled(".nesrec");
      if (!c.confirmReplace.empty()) {
        ImGui::TextColored(ImVec4(1, 0.75f, 0.4f, 1), "%s",
                           (kRecycleBin ? TRF("“%@” already exists. Replace it? (The existing project is moved to the Recycle Bin.)",
                                              {fs::path(c.confirmReplace).filename().string()})
                                        : TRF("“%@” already exists. Replace it? (The existing project is moved to the Trash.)",
                                              {fs::path(c.confirmReplace).filename().string()}))
                               .c_str());
      } else {
        ImGui::Spacing();
      }
      bool doSave = enter;
      if (!c.confirmReplace.empty()) {
        if (ImGui::Button(TR("Replace"))) {
          chosen = c.confirmReplace;
          done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"))) c.confirmReplace.clear();
      } else {
        if (ImGui::Button(TR("Save"))) doSave = true;
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"))) cancelled = true;
      }
      if (doSave && c.confirmReplace.empty()) {
        std::string file = chooserProjectFileName(c.name);
        if (!file.empty()) {
          std::string path = c.dir + "/" + file;
          std::error_code ec;
          if (fs::exists(path, ec)) c.confirmReplace = path;
          else {
            chosen = path;
            done = true;
          }
        }
      }
    } else {
      if (ImGui::Button(TR("Cancel"))) cancelled = true;
    }
    if (!textEntryOwnsPad()) {  // the on-screen keyboard shows its own buttons
      ImGui::SameLine(0, S(24));
      inlinePrompts(TR("Back"));
    }
    if (!ImGui::GetIO().WantTextInput && !appearing &&
        (ImGui::IsKeyPressed(cancelKey(), false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
      if (!c.confirmReplace.empty()) c.confirmReplace.clear();
      else if (!rnf_paths_equal(c.dir.c_str(), c.req.root.c_str())) {
        c.dir = fs::path(c.dir).parent_path().string();
        chooserList();
      } else cancelled = true;
    }
    if (done || cancelled) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (done || cancelled) {
    ChooserRequest req = std::move(c.req);
    chooser_.reset();
    if (done && req.onChosen) req.onChosen(chosen);
    if (cancelled && req.onCancel) req.onCancel();
  }
}

// ------------------------------------------------------------------ rename

void UI::buildRename() {
  if (!rename_ || !dialogs_.empty() || chooser_) return;
  Rename& r = *rename_;
  ImGuiIO& io = ImGui::GetIO();
  if (!r.opened) {
    ImGui::OpenPopup("##rename");
    r.opened = true;
  }
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.3f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(S(640), io.DisplaySize.x * 0.9f), 0));
  bool ok = false, cancel = false;
  if (ImGui::BeginPopupModal("##rename", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
    ImGui::PushFont(nullptr, metrics_.title());
    ImGui::TextUnformatted(r.title.c_str());
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputText("##text", r.buf, sizeof r.buf, ImGuiInputTextFlags_EnterReturnsTrue)) ok = true;
    if (ImGui::Button(TR("OK"), ImVec2(S(120), 0))) ok = true;
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(120), 0))) cancel = true;
    if (!textEntryOwnsPad()) inlinePrompts(TR("Cancel"));  // the keyboard shows its own buttons
    if (!io.WantTextInput && !ImGui::IsWindowAppearing() &&
        (ImGui::IsKeyPressed(cancelKey(), false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
      cancel = true;
    if (ok || cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (ok || cancel) {
    Rename done = std::move(*rename_);
    rename_.reset();
    if (ok && done.apply) done.apply(done.buf);
  }
}

}  // namespace rnl
