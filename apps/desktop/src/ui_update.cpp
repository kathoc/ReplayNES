// In-app updates in the UI (state: UpdateService / UpdateModel): a notice on the library (start
// screen) with Update / Later, the download progress and Restart; an "Update" button on the hub
// while one is waiting; Settings -> Audio & Controls -> System: "Check for updates automatically"
// and "Check now" (Windows: WinSparkle's own dialogs behind "Check for Updates…"). All controller
// navigable (plain buttons in the page's focus order).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string>

#include "imgui_internal.h"
#include "l10n.h"
#include "settings.h"
#include "ui.h"
#include "update_service.h"

namespace rnl {

namespace {
constexpr const char* kAllowUpdatesCommand = "flatpak permission-set flatpak updates io.github.replaynes.ReplayNES yes";
constexpr const char* kUpdateCommand = "flatpak update io.github.replaynes.ReplayNES";

void wrapped(const char* text, bool disabled) {
  if (disabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  if (disabled) ImGui::PopStyleColor();
}
}  // namespace

void UI::refreshUpdate() {
  if (!d_.updates) return;
  update_ = d_.updates->snapshot();
  if (update_.serial == updateSerial_) return;
  updateSerial_ = update_.serial;
  // Check now failed (Settings): the reason in a dialog (the text under the button may be scrolled away).
  if (update_.phase == UpdatePhase::failed && update_.userInitiated) {
    Dialog d;
    d.title = TR("The update failed.");
    d.message = updateErrorText();
    showDialog(std::move(d));
    if (d_.updates) d_.updates->dismiss();
    return;
  }
  // While playing (no library on screen): a short notice when something is waiting.
  if (hasSession() && !update_.noticeHidden) {
    if (update_.phase == UpdatePhase::available) notice(TR("A new version of ReplayNES is available (menu: Update)."));
    else if (update_.phase == UpdatePhase::installed) notice(TR("The update is installed. Restart ReplayNES to use it."));
  }
}

std::string UI::updateErrorText() const {
  switch (update_.error) {
    case UpdateError::denied:
      return TRF("Updating ReplayNES is not allowed on this system. Allow it once with this command (Desktop Mode, Konsole): %@",
                 {kAllowUpdatesCommand});
    case UpdateError::noDialog:
      return TRF("The system could not ask for permission to update (Gaming Mode). Update once in Desktop Mode, or allow "
                 "it with this command in Konsole: %@",
                 {kAllowUpdatesCommand});
    case UpdateError::newPermissions:
      return TRF("This update needs new permissions. Update in Desktop Mode (Discover) or with: %@", {kUpdateCommand});
    case UpdateError::network: return TR("The update repository could not be reached. Check the network connection.");
    case UpdateError::noRepository: return TR("Updates unavailable (installed without a repository)");
    default: return update_.errorMessage;
  }
}

std::string UI::updateStatusText() const {
  switch (update_.phase) {
    case UpdatePhase::unsupported:
      switch (update_.reason) {
        case UpdateUnavailable::noRepository: return TR("Updates unavailable (installed without a repository)");
        case UpdateUnavailable::noPortal: return TR("Updates unavailable (no Flatpak portal)");
        default: return TR("Updates unavailable (not running as a Flatpak)");
      }
    case UpdatePhase::checking: return TR("Checking for updates…");
    case UpdatePhase::available: return TR("A new version of ReplayNES is available.");
    case UpdatePhase::updating: return TRF("Downloading the update… %lld%%", {update_.percent});
    case UpdatePhase::installed:
      return update_.newVersion.empty() ? std::string(TR("The update is installed. Restart ReplayNES to use it."))
                                        : TRF("ReplayNES %@ is installed. Restart to use it.", {update_.newVersion});
    case UpdatePhase::upToDate: return TR("ReplayNES is up to date.");
    case UpdatePhase::failed: return TR("The update failed.");
    case UpdatePhase::idle: break;
  }
  return {};
}

// The library's notice: one row above the search field.
void UI::buildUpdateNotice() {
  if (!update_.noticeVisible()) return;
  ImGui::PushID("##update");
  ImVec4 bg = update_.phase == UpdatePhase::failed ? ImVec4(0.42f, 0.20f, 0.14f, 1) : ImVec4(0.14f, 0.27f, 0.45f, 1);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(8)));
  ImGui::BeginChild("##updatenotice", ImVec2(0, 0),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(updateStatusText().c_str());
  switch (update_.phase) {
    case UpdatePhase::available:
      ImGui::SameLine();
      if (ImGui::Button(TR("Update"))) d_.updates->update();
      ImGui::SameLine();
      if (ImGui::Button(TR("Later"))) d_.updates->dismiss();
      break;
    case UpdatePhase::updating:
      ImGui::SameLine();
      ImGui::ProgressBar(float(update_.percent) / 100.0f, ImVec2(S(260), 0));
      break;
    case UpdatePhase::installed:
      ImGui::SameLine();
      if (ImGui::Button(TR("Restart")) && onRestart) onRestart();
      ImGui::SameLine();
      if (ImGui::Button(TR("Later"))) d_.updates->dismiss();
      break;
    case UpdatePhase::failed:
      ImGui::SameLine();
      if (ImGui::Button(TR("OK"))) d_.updates->dismiss();
      wrapped(updateErrorText().c_str(), false);
      break;
    default: break;
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::PopID();
  ImGui::Spacing();
}

// Hub: the "Update" button's dialog.
void UI::showUpdateDialog() {
  Dialog d;
  if (update_.phase == UpdatePhase::installed) {
    d.title = TR("Update Installed");
    d.message = updateStatusText() + "\n\n" + TR("Restart now? The session is saved first.");
    d.buttons = {TR("Restart"), TR("Later")};
    d.onResult = [this](int b, bool) {
      if (b == 0 && onRestart) onRestart();
      else if (d_.updates) d_.updates->dismiss();
    };
  } else if (update_.phase == UpdatePhase::available) {
    d.title = TR("Update Available");
    d.message = std::string(TR("A new version of ReplayNES is available.")) + "\n\n" +
                TR("It downloads in the background: you can keep playing, then restart when it is installed.");
    d.buttons = {TR("Update"), TR("Later")};
    d.onResult = [this](int b, bool) {
      if (!d_.updates) return;
      if (b == 0) d_.updates->update();
      else d_.updates->dismiss();
    };
  } else {
    return;
  }
  showDialog(std::move(d));
}

// Settings -> Audio & Controls -> System.
void UI::buildUpdateSettings() {
  Settings& s = *d_.settings;
  if (!d_.updates) {  // no in-app updates (e.g. a Windows build without WinSparkle.dll): the version only
    wrapped(TRF("Version %@", {RNL_APP_VERSION}).c_str(), true);
    return;
  }
  if (d_.updates->ownsDialogs()) {  // Windows: WinSparkle shows its own windows (under "System")
    bool autoCheck = d_.updates->autoCheckEnabled();
    if (ImGui::Checkbox(TR("Automatically check for updates"), &autoCheck)) d_.updates->setAutoCheck(autoCheck);
    if (ImGui::Button((std::string(TR("Check for Updates…")) + "##updatecheck").c_str())) d_.updates->checkNow();
    wrapped(TRF("Version %@", {RNL_APP_VERSION}).c_str(), true);
    wrapped(TR("When this is on, ReplayNES looks for a new version once a day. Updates come from the project’s GitHub "
               "releases; their signature is checked before they are installed, and ReplayNES restarts into the new "
               "version (the session is saved first)."),
            true);
    return;
  }
  ImGui::SeparatorText(TR("Updates"));
  if (ImGui::Checkbox(TR("Automatically check for updates"), &s.checkForUpdates)) changed();
  bool unsupported = update_.phase == UpdatePhase::unsupported;
  ImGui::BeginDisabled(unsupported || update_.busy() || !d_.updates);
  if (ImGui::Button((std::string(TR("Check now")) + "##updatecheck").c_str())) d_.updates->checkNow();
  ImGui::EndDisabled();
  std::string status = updateStatusText();
  if (update_.phase == UpdatePhase::available) {
    ImGui::SameLine();
    if (ImGui::Button((std::string(TR("Update")) + "##updateapply").c_str())) d_.updates->update();
  } else if (update_.phase == UpdatePhase::installed) {
    ImGui::SameLine();
    if (ImGui::Button((std::string(TR("Restart")) + "##updaterestart").c_str()) && onRestart) onRestart();
  }
  if (!status.empty()) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(status.c_str());
  }
  if (update_.phase == UpdatePhase::updating) ImGui::ProgressBar(float(update_.percent) / 100.0f, ImVec2(S(420), 0));
  if (update_.phase == UpdatePhase::failed) wrapped(updateErrorText().c_str(), false);
  wrapped(TRF("Version %@", {RNL_APP_VERSION}).c_str(), true);
  wrapped(TR("While ReplayNES runs, the system checks its repository about every 30 minutes and shows a notice on the "
             "library. Check now looks right away and installs the update when there is one; restart ReplayNES to use it."),
          true);
}

bool UI::scriptUpdate(const std::string& action) {
  if (!d_.updates) return false;
  if (action == "check") d_.updates->checkNow();
  else if (action == "apply") d_.updates->update();
  else if (action == "later") d_.updates->dismiss();
  else if (action == "restart") {
    if (!onRestart) return false;
    onRestart();
  } else {
    return false;
  }
  return true;
}

std::string UI::updatePhaseName() const {
  switch (update_.phase) {
    case UpdatePhase::unsupported: return "unsupported";
    case UpdatePhase::idle: return "idle";
    case UpdatePhase::checking: return "checking";
    case UpdatePhase::available: return "available";
    case UpdatePhase::updating: return "updating";
    case UpdatePhase::installed: return "installed";
    case UpdatePhase::upToDate: return "uptodate";
    case UpdatePhase::failed: return "failed";
  }
  return "?";
}

}  // namespace rnl
