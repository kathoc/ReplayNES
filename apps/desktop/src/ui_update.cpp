// In-app updates in the UI (state: UpdateService / UpdateModel): a notice on the library (start
// screen) that opens Settings > System > Updates (status, Check Now, Update / Restart, Check
// Automatically: ui_settings.cpp; Windows: WinSparkle's own dialogs behind Check Now).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string>

#include "icons.h"
#include "imgui_internal.h"
#include "l10n.h"
#include "settings.h"
#include "ui.h"
#include "ui_theme.h"
#include "update_service.h"

namespace rnl {

namespace {
constexpr const char* kAllowUpdatesCommand = "flatpak permission-set flatpak updates io.github.replaynes.ReplayNES yes";
constexpr const char* kUpdateCommand = "flatpak update io.github.replaynes.ReplayNES";

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
    if (update_.phase == UpdatePhase::available) notice(TR("A new version of ReplayNES is available (Settings › System › Updates)."));
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

// The library's notice: a small pill in its top bar; a click / tap opens Settings > System > Updates.
void UI::buildUpdateNotice(const LRect& r) {
  if (!update_.noticeVisible()) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bg = update_.phase == UpdatePhase::failed ? IM_COL32(120, 50, 36, 230) : IM_COL32(36, 80, 140, 230);
  dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), bg, r.h / 2);
  std::string t = std::string(icons::glyph("download")) + "  " + updateStatusText();
  theme::textFit(dl, metrics_.hint(), ImVec2(r.x + S(14), r.y + (r.h - metrics_.hint()) / 2), r.w - S(28), theme::kText, t);
  ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
  if (ImGui::InvisibleButton("##updatenotice", ImVec2(r.w, r.h))) openPage("system.updates");
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
