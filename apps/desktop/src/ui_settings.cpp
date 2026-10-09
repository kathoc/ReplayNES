// The values behind the menu's items (Settings rows, Retry / Share / Game tiles): what each row
// shows, what A and left / right do; the controller diagram (rnf diagram geometry; A or a click
// on a button to change it, held buttons light up) and its action picker; Add to Steam.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <string>

#include "app_model.h"
#include "emulation.h"
#include "icons.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "library.h"
#include "paths.h"
#include "platform/platform.h"
#include "settings.h"
#include "ui.h"
#include "ui_theme.h"
#include "update_service.h"

namespace rnl {

using namespace theme;

namespace {
std::string str(char* p) {
  std::string s = p ? p : "";
  rnf_string_free(p);
  return s;
}
const char* txt(const char* key) { return !key || !*key ? "" : key[0] == '=' ? key + 1 : TR(key); }

const int kCrtLines[] = {110, 120, 140, 160, 180, 200, 220, 240};
const double kAutosave[] = {2, 3, 5, 10, 30};

// The menu model item of an id (any page): page / index / info.
bool findItem(const rnf_menu* m, const std::string& id, size_t* page, size_t* index, rnf_menu_item_info* info) {
  for (size_t p = 0; p < rnf_menu_page_count(m); ++p) {
    int i = rnf_menu_item_find(m, p, id.c_str());
    if (i < 0) continue;
    if (page) *page = p;
    if (index) *index = size_t(i);
    if (info) rnf_menu_item_get(m, p, size_t(i), info);
    return true;
  }
  return false;
}
}  // namespace

// ------------------------------------------------------------------ values

namespace {
int choiceIndex(const Settings& s, const InputRouter& in, const std::string& id) {
  if (id == "display.size") return s.integerScale ? 0 : 1;
  if (id == "display.flash") return std::clamp(s.flash, 0, 3);
  if (id == "controls.osk") return s.onScreenKeyboard == "builtin" ? 1 : s.onScreenKeyboard == "steam" ? 2 : 0;
  if (id == "controls.socd") return in.socd() == "last_wins" ? 1 : in.socd() == "allow" ? 2 : 0;
  if (id == "controls.confirm") return s.southConfirm ? 1 : 0;
  if (id == "system.language") return s.language == "ja" ? 1 : s.language == "en" ? 2 : 0;
  if (id == "system.autosave") {
    for (int i = 0; i < 5; ++i)
      if (s.autosaveInterval <= kAutosave[i] + 0.01) return i;
    return 4;
  }
  return 0;
}
}  // namespace

std::string UI::itemValue(const std::string& id) const {
  const Settings& s = *d_.settings;
  const InputRouter& in = *d_.input;
  char b[64];
  rnf_menu_item_info info{};
  size_t page = 0, index = 0;
  if (findItem(menu_, id, &page, &index, &info) && info.kind == RNF_MENU_ITEM_CHOICE) {
    if (id == "display.flash") return str(rnf_flash_level_label(rn_flash_level(std::clamp(s.flash, 0, 3))));
    return txt(rnf_menu_item_choice(menu_, page, index, size_t(choiceIndex(s, in, id))));
  }
  if (id == "crt.lines") return std::to_string(CrtSettings::validLines(s.crtLines) ? s.crtLines : 240);
  if (id == "crt.antenna") {
    std::snprintf(b, sizeof b, "%.0f dB\xC2\xB5V", s.crtAntenna);
    return b;
  }
  if (id == "controls.turbo") {
    std::snprintf(b, sizeof b, "%.1f/s", 60.0988 / std::max(1, in.turboPeriod()));
    return b;
  }
  if (id == "controls.turbo_duty") return TRF("%lld frames", {in.turboDuty()});
  if (id == "controls.stick") {
    std::snprintf(b, sizeof b, "%.2f", in.analogThreshold());
    return b;
  }
  if (id == "sound.volume") return std::to_string(int(std::lround(s.volume * 100))) + "%";
  if (id == "system.ui_scale") {
    std::snprintf(b, sizeof b, "%.2f\xC3\x97", double(s.uiScale));
    return b;
  }
  if (id == "updates.status") {
    if (d_.updates && d_.updates->ownsDialogs()) return TRF("Version %@", {RNL_APP_VERSION});
    std::string t = updateStatusText();
    return t.empty() ? TRF("Version %@", {RNL_APP_VERSION}) : t;
  }
  if (id == "updates.apply") return update_.phase == UpdatePhase::installed ? TR("Restart") : "";
  if (id == "about.version") return RNL_APP_VERSION;
  if (id == "about.roms") return Paths::display(d_.library->romDir());
  if (id == "about.projects") return Paths::display(d_.library->projectsDir());
  if (id == "about.settings") return Paths::display(d_.app->paths().settingsFile());
  if (id == "about.license") return "GPL-2.0-or-later";
#if RNL_HAVE_STEAM
  if (id == "system.steam" && steamJob_.valid()) return TR("Adding to Steam…");
#endif
  return {};
}

bool UI::itemOn(const std::string& id) const {
  const Settings& s = *d_.settings;
  if (id == "display.crt") return s.crt && d_.renderer->postProcessStatus().crtAvailable;
  if (id == "display.par87") return s.par87;
  if (id == "display.overscan") return s.hideOverscan;
  if (id == "display.vtr") return s.vtrEffect;
  if (id == "crt.beam") return s.crtBeamGrowth;
  if (id == "crt.persistence") return s.crtPersistence;
  if (id == "crt.supply") return s.crtSupply;
  if (id == "controls.dpad_paused") return s.dpadStepWhenPaused;
  if (id == "controls.rewind_pause") return s.pauseAfterRewind;
  if (id == "system.fullscreen") return isFullscreen && isFullscreen();
  if (id == "system.flash_badge") return s.showFlashIndicator;
  if (id == "system.stats") return s.showStats;
  if (id == "updates.auto") return d_.updates && d_.updates->ownsDialogs() ? d_.updates->autoCheckEnabled() : s.checkForUpdates;
  if (id == "retry.playback") return hasSession() && !d_.emu->status().recording && !d_.emu->status().practicing;
  return false;
}

float UI::itemFraction(const std::string& id) const {
  const Settings& s = *d_.settings;
  const InputRouter& in = *d_.input;
  if (id == "crt.lines") return float(std::clamp(s.crtLines, 110, 240) - 110) / 130.0f;
  if (id == "crt.antenna") return float((s.crtAntenna - 20) / 70);
  if (id == "controls.turbo") return float(30 - std::clamp(in.turboPeriod(), 2, 30)) / 28.0f;
  if (id == "controls.turbo_duty") return in.turboPeriod() > 2 ? float(in.turboDuty() - 1) / float(in.turboPeriod() - 2) : 0.0f;
  if (id == "controls.stick") return float((in.analogThreshold() - 0.2) / 0.7);
  if (id == "sound.volume") return s.volume;
  if (id == "system.ui_scale") return (s.uiScale - 0.75f) / 0.75f;
  return -1;
}

bool UI::itemEnabled(const std::string& id) const {
  const EmuStatus& st = d_.emu->status();
  bool crt = d_.renderer->postProcessStatus().crtAvailable;
  if (id == "retry.undo") return st.undoDepth > 0 && !st.practicing;
  if (id == "retry.playback") return !st.practicing && st.takeLength > 0;
  if (id == "retry.takes" || id == "retry.bookmarks" || id == "retry.restart") return hasSession();
  if (id == "share.export") return RNL_HAVE_MP4_EXPORT && st.takeLength > 0;
  if (id == "display.crt" || id.rfind("crt.", 0) == 0 || id == "display.crt_detail") return crt;
  if (id == "updates.check")
    return d_.updates && (d_.updates->ownsDialogs() || (update_.phase != UpdatePhase::unsupported && !update_.busy()));
  if (id == "updates.apply") return update_.phase == UpdatePhase::available || update_.phase == UpdatePhase::installed;
  if (id == "updates.auto") return d_.updates != nullptr;
  if (id.rfind("game.", 0) == 0 && id != "game.open") return hasSession();
#if RNL_HAVE_STEAM
  if (id == "system.steam") return !steamJob_.valid();
#endif
  return true;
}

// ------------------------------------------------------------------ actions

void UI::adjustItem(const std::string& id, int dir) {
  if (!itemEnabled(id)) return;
  Settings& s = *d_.settings;
  InputRouter& in = *d_.input;
  rnf_menu_item_info info{};
  size_t page = 0, index = 0;
  if (!findItem(menu_, id, &page, &index, &info)) return;
  int step = dir == 0 ? 1 : dir;
  if (info.kind == RNF_MENU_ITEM_TOGGLE) {
    if (id == "display.crt") s.crt = !s.crt;
    else if (id == "display.par87") s.par87 = !s.par87;
    else if (id == "display.overscan") s.hideOverscan = !s.hideOverscan;
    else if (id == "display.vtr") s.vtrEffect = !s.vtrEffect;
    else if (id == "crt.beam") s.crtBeamGrowth = !s.crtBeamGrowth;
    else if (id == "crt.persistence") s.crtPersistence = !s.crtPersistence;
    else if (id == "crt.supply") s.crtSupply = !s.crtSupply;
    else if (id == "controls.dpad_paused") s.dpadStepWhenPaused = !s.dpadStepWhenPaused;
    else if (id == "controls.rewind_pause") s.pauseAfterRewind = !s.pauseAfterRewind;
    else if (id == "system.fullscreen") {
      if (onFullscreen) onFullscreen(!(isFullscreen && isFullscreen()));
      return;
    } else if (id == "system.flash_badge") s.showFlashIndicator = !s.showFlashIndicator;
    else if (id == "system.stats") s.showStats = !s.showStats;
    else if (id == "updates.auto") {
      if (d_.updates && d_.updates->ownsDialogs()) {
        d_.updates->setAutoCheck(!d_.updates->autoCheckEnabled());
        return;
      }
      s.checkForUpdates = !s.checkForUpdates;
    } else if (id == "retry.playback") {
      d_.emu->toggleRecord();
      return;
    }
    changed();
    return;
  }
  if (info.kind == RNF_MENU_ITEM_CHOICE) {
    int n = int(info.choice_count), i = choiceIndex(s, in, id);
    int next = ((i + step) % n + n) % n;
    if (id == "display.size") s.integerScale = next == 0;
    else if (id == "display.flash") s.flash = next;
    else if (id == "controls.osk") s.onScreenKeyboard = next == 1 ? "builtin" : next == 2 ? "steam" : "auto";
    else if (id == "controls.confirm") s.southConfirm = next == 1;
    else if (id == "controls.socd") {
      in.setSOCD(next == 1 ? "last_wins" : next == 2 ? "allow" : "neutral");
      return;
    } else if (id == "system.language") {
      s.language = next == 1 ? "ja" : next == 2 ? "en" : "auto";
      changed();
      if (onLanguage) onLanguage(s.language);
      return;
    } else if (id == "system.autosave") s.autosaveInterval = kAutosave[next];
    changed();
    return;
  }
  if (info.kind == RNF_MENU_ITEM_SLIDER) {
    if (dir == 0) return;
    if (id == "crt.lines") {
      int at = 7;
      for (int i = 0; i < 8; ++i)
        if (kCrtLines[i] == s.crtLines) at = i;
      s.crtLines = kCrtLines[std::clamp(at + dir, 0, 7)];
    } else if (id == "crt.antenna") {
      s.crtAntenna = std::clamp(s.crtAntenna + 5.0 * dir, 20.0, 90.0);
    } else if (id == "controls.turbo") {
      int p = std::clamp(in.turboPeriod() - dir, 2, 30);
      in.setTurbo(p, std::min(in.turboDuty(), p - 1));
      return;
    } else if (id == "controls.turbo_duty") {
      in.setTurbo(in.turboPeriod(), std::clamp(in.turboDuty() + dir, 1, std::max(1, in.turboPeriod() - 1)));
      return;
    } else if (id == "controls.stick") {
      in.setAnalogThreshold(std::clamp(std::round((in.analogThreshold() + 0.05 * dir) * 20) / 20, 0.2, 0.9));
      return;
    } else if (id == "sound.volume") {
      s.volume = std::clamp(std::round((s.volume + 0.05f * float(dir)) * 20.0f) / 20.0f, 0.0f, 1.0f);
    } else if (id == "system.ui_scale") {
      s.uiScale = std::clamp(std::round((s.uiScale + 0.05f * float(dir)) * 20.0f) / 20.0f, 0.75f, 1.5f);
    }
    changed();
  }
}

void UI::activateItem(const std::string& id) {
  if (id.empty() || !itemEnabled(id)) return;
  rnf_menu_item_info info{};
  if (!findItem(menu_, id, nullptr, nullptr, &info)) return;
  if (info.kind == RNF_MENU_ITEM_TOGGLE || info.kind == RNF_MENU_ITEM_CHOICE) return adjustItem(id, 0);
  if (info.kind == RNF_MENU_ITEM_SLIDER || info.kind == RNF_MENU_ITEM_INFO) return;
  EmulationController* emu = d_.emu;
  Settings& s = *d_.settings;
  if (id == "retry.rerecord") {
    setMenu(false);
    emu->rerecordHere();  // resumes recording from here
  } else if (id == "retry.undo") {
    emu->undoTake();
    setMenu(false);  // paused: the seek bar shows where it went
  } else if (id == "share.export") {
    openExportDialog();
  } else if (id == "crt.reset") {
    s.crtLines = 240;
    s.crtBeamGrowth = s.crtPersistence = s.crtSupply = true;
    s.crtAntenna = 65;
    changed();
  } else if (id == "controls.reset") {
    Dialog d;
    d.title = TR("Reset Controls");
    d.message = TR("Every key and button back to the defaults");
    d.buttons = {TR("Reset"), TR("Cancel")};
    d.destructiveIndex = 0;
    d.cancelIndex = 1;
    d.onResult = [this](int b, bool) {
      if (b == 0) d_.input->resetToDefaults();
    };
    showDialog(std::move(d));
  } else if (id == "system.steam") {
    startAddToSteam();
  } else if (id == "system.quit") {
    if (onQuit) onQuit();
  } else if (id == "updates.check") {
    if (d_.updates) d_.updates->checkNow();
  } else if (id == "updates.apply") {
    if (update_.phase == UpdatePhase::installed) {
      if (onRestart) onRestart();
    } else if (d_.updates) {
      d_.updates->update();
    }
  } else if (id == "about.roms") {
    d_.library->ensureFolders();
    openFolder(d_.library->romDir());
  } else if (id == "about.projects") {
    d_.library->ensureFolders();
    openFolder(d_.library->projectsDir());
  } else if (id == "game.library") {
    setMenu(false);
    d_.app->closeProject();  // asks to save first when needed; then the library
  } else if (id == "game.save") {
    d_.app->save();
  } else if (id == "game.save_as") {
    d_.app->saveAs();
  } else if (id == "game.open") {
    d_.app->openProjectChooser();
  } else if (id == "game.reset") {
    showResetChoices();
  } else if (id == "retry.restart") {
    d_.app->resetProjectPrompt();
  }
}

// ------------------------------------------------------------------ Add to Steam

// Runs steam::addToSteam off the render thread (it reads / writes the Steam folders); the result
// is shown in a dialog by pollAddToSteam().
#if RNL_HAVE_STEAM
void UI::startAddToSteam() {
  if (steamJob_.valid()) return;
  steamJob_ = std::async(std::launch::async, [] {
    steam::Report r;
    steam::addToSteam(steam::AddOptions{}, r);
    return r;
  });
}

void UI::pollAddToSteam() {
  if (!steamJob_.valid() || steamJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  steam::Report r = steamJob_.get();
  bool failed = false, running = false, changedAny = false;
  std::string error;
  for (const steam::UserResult& u : r.users) {
    switch (u.action) {
      case steam::UserAction::Failed:
        failed = true;
        if (error.empty()) error = u.error;
        break;
      case steam::UserAction::SteamRunning: running = true; break;
      case steam::UserAction::Added:
      case steam::UserAction::Updated: changedAny = true; break;
      case steam::UserAction::Unchanged: break;
    }
  }
  Dialog d;
  d.title = TR("Add to Steam");
  if (r.users.empty())
    d.message = TR("No Steam account was found. Start Steam and sign in once, then try again.");
  else if (failed)
    d.message = std::string(TR("Adding ReplayNES to Steam failed.")) + "\n\n" + r.summary + (error.empty() ? "" : "\n" + error);
  else if (running)
    d.message = TR("Steam is running. Close Steam first (Desktop Mode: Steam menu → Exit), then try again. Artwork that could "
                   "be installed appears after Steam restarts.");
  else if (changedAny)
    d.message = r.restartSteam ? TR("ReplayNES was added to Steam. Start (or restart) Steam to see it.")
                               : TR("ReplayNES was added to Steam.");
  else
    d.message = TR("ReplayNES is already in Steam with its artwork.");
  showDialog(std::move(d));
}
#else
void UI::startAddToSteam() {}
void UI::pollAddToSteam() {}
#endif

// ------------------------------------------------------------------ controller diagram

rnf_controller_family UI::diagramFamily(int slot) const {
  const PadInfo& pad = d_.input->pad(slot);
  rnf_controller_family family = pad.pad ? pad.family : RNF_FAMILY_STEAM_DECK;
  if (d_.settings->diagramFamily != "auto")
    family = rnf_controller_family(std::clamp(std::atoi(d_.settings->diagramFamily.c_str()), 0, 4));
  return family;
}

void UI::buildDiagram(rnf_controller_family family, int slot, const LRect& area, double now) {
  InputRouter& in = *d_.input;
  const UiMetrics& m = metrics_;
  const std::vector<rnf_binding>& b = in.bindings();
  const PadInfo& pad = in.pad(slot);
  float legendH = m.hint() * 2.2f;
  float k = std::min(area.w / float(RNF_DIAGRAM_CANVAS_WIDTH), (area.h - legendH) / float(RNF_DIAGRAM_CANVAS_HEIGHT));
  float cw = float(RNF_DIAGRAM_CANVAS_WIDTH) * k, ch = float(RNF_DIAGRAM_CANVAS_HEIGHT) * k;
  ImVec2 o(area.x + (area.w - cw) / 2, area.y + (area.h - legendH - ch) / 2);
  auto P = [&](double x, double y) { return ImVec2(o.x + float(x) * k, o.y + float(y) * k); };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  rnf_diagram_info info{};
  rnf_diagram_info_get(family, &info);
  // The outline (rnf_diagram_decor): a rounded body without grips, the Steam Deck's screen, pads.
  for (size_t i = 0, nd = rnf_diagram_decor_count(family); i < nd; ++i) {
    rnf_diagram_decor d{};
    if (!rnf_diagram_decor_get(family, i, &d)) continue;
    ImVec2 a = P(d.x, d.y), z = P(d.x + d.width, d.y + d.height);
    float r = float(d.radius) * k;
    switch (d.kind) {
      case RNF_DECOR_BODY:
        dl->AddRectFilled(a, z, IM_COL32(44, 46, 54, 255), r);
        dl->AddRect(a, z, IM_COL32(255, 255, 255, 90), r, 0, 1.5f);
        break;
      case RNF_DECOR_SCREEN:
        dl->AddRectFilled(a, z, IM_COL32(14, 15, 18, 255), r);
        dl->AddRect(a, z, IM_COL32(255, 255, 255, 40), r, 0, 1.0f);
        break;
      case RNF_DECOR_PAD:
        dl->AddRectFilled(a, z, IM_COL32(255, 255, 255, 22), r);
        dl->AddRect(a, z, IM_COL32(255, 255, 255, 50), r, 0, 1.0f);
        break;
    }
  }
  for (auto [x, y] : {std::pair<double, double>{info.left_stick_x, info.left_stick_y}, {info.right_stick_x, info.right_stick_y}}) {
    dl->AddCircleFilled(P(x, y), float(info.stick_radius) * k, IM_COL32(255, 255, 255, 36));
    dl->AddCircle(P(x, y), float(info.stick_radius) * k, IM_COL32(255, 255, 255, 110));
  }
  dl->AddRectFilled(P(info.dpad_x - RNF_DIAGRAM_DPAD_ARM / 2, info.dpad_y - RNF_DIAGRAM_DPAD_ARM / 2),
                    P(info.dpad_x + RNF_DIAGRAM_DPAD_ARM / 2, info.dpad_y + RNF_DIAGRAM_DPAD_ARM / 2), IM_COL32(255, 255, 255, 80));
  std::map<std::string, std::pair<rnf_group_summary, std::string>> groups;
  for (const char* g : {"dpad", "lstick", "rstick"}) {
    char* text = nullptr;
    rnf_group_summary gs = rnf_input_group_summary(b.data(), b.size(), g, slot, &text);
    groups[g] = {gs, str(text)};
  }
  std::string prefix = "gc" + std::to_string(slot) + ":";
  size_t n = rnf_diagram_element_count(family);
  float fs = std::max(m.hint() * 0.85f, 12 * k);
  diagramIds_.clear();
  diagramCenters_.clear();
  if (diagramFocus_ >= int(n)) diagramFocus_ = 0;
  LRect focusRect;
  for (size_t i = 0; i < n; ++i) {
    rnf_diagram_element e{};
    if (!rnf_diagram_element_get(family, i, &e)) continue;
    std::string el = e.element, id = prefix + el;
    ImVec2 c = P(e.cx, e.cy);
    float w = float(e.width) * k, h = float(e.height) * k;
    ImVec2 a(c.x - w / 2, c.y - h / 2), z(c.x + w / 2, c.y + h / 2);
    float hw = std::max(w, S(30)), hh = std::max(h, S(30));
    ImGui::SetCursorScreenPos(ImVec2(c.x - hw / 2, c.y - hh / 2));
    ImGui::PushID(el.c_str());
    bool clicked = ImGui::InvisibleButton("##el", ImVec2(hw, hh));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    int index = int(diagramIds_.size());
    diagramIds_.push_back(id);
    diagramCenters_.push_back(c);
    if (hovered && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) diagramFocus_ = index;
    bool focused = index == diagramFocus_;
    bool reserved = InputRouter::isReserved(el);
    if (clicked) {
      diagramFocus_ = index;
      if (reserved) notice(TR("R3: ReplayNES menu (reserved)"));
      else openAssign(id);
    }
    rnf_list* acts = rnf_input_element_actions(b.data(), b.size(), el.c_str(), slot);
    bool assigned = rnf_list_count(acts) > 0;
    bool hotkey = false;
    for (size_t j = 0; j < rnf_list_count(acts); ++j) hotkey = hotkey || std::string(rnf_list_a(acts, j)).rfind("hk.", 0) == 0;
    rnf_list_free(acts);
    bool down = in.pressed().count(id) > 0;
    ImU32 fill = down ? kBrand : assigned ? IM_COL32(64, 66, 78, 255) : IM_COL32(255, 255, 255, 22);
    ImU32 line = IM_COL32(255, 255, 255, assigned ? 170 : 90);
    ImU32 fg = down ? IM_COL32(255, 255, 255, 255) : assigned ? kText : IM_COL32(170, 170, 180, 255);
    std::string label;
    auto it = pad.labels.find(el);
    if (pad.pad && it != pad.labels.end() && family == pad.family) label = it->second;
    else label = rnf_controller_family_label(family, el.c_str());
    switch (e.kind) {
      case RNF_DIAGRAM_FACE:
      case RNF_DIAGRAM_HOME:
      case RNF_DIAGRAM_STICK_CLICK:
        if (e.kind != RNF_DIAGRAM_STICK_CLICK || down || focused) dl->AddCircleFilled(c, w / 2, fill);
        dl->AddCircle(c, w / 2, line, 0, 1.0f);
        break;
      case RNF_DIAGRAM_DPAD:
        dl->AddRectFilled(a, z, fill);
        dl->AddRect(a, z, line);
        break;
      case RNF_DIAGRAM_STICK_DIRECTION: {
        std::string dir = el.substr(el.find('.') + 1);
        ImVec2 d = dir == "up" ? ImVec2(0, -1) : dir == "down" ? ImVec2(0, 1) : dir == "left" ? ImVec2(-1, 0) : ImVec2(1, 0);
        float r = w * 0.4f;
        ImVec2 tip(c.x + d.x * r, c.y + d.y * r), b1(c.x - d.x * r * 0.4f + d.y * r, c.y - d.y * r * 0.4f + d.x * r),
            b2(c.x - d.x * r * 0.4f - d.y * r, c.y - d.y * r * 0.4f - d.x * r);
        dl->AddTriangleFilled(tip, b1, b2, down ? kBrand : fg);
        break;
      }
      default:
        dl->AddRectFilled(a, z, fill, (e.kind == RNF_DIAGRAM_TRIGGER ? 9 : 7) * k);
        dl->AddRect(a, z, line, (e.kind == RNF_DIAGRAM_TRIGGER ? 9 : 7) * k);
        break;
    }
    if (!label.empty() && e.kind != RNF_DIAGRAM_STICK_DIRECTION && e.kind != RNF_DIAGRAM_DPAD) {
      // Long labels ("STEAM") shrink to fit their button.
      float lfs = fs;
      float lw = measure(lfs, label.c_str()).x;
      if (lw > w * 0.86f) lfs *= w * 0.86f / lw;
      ImVec2 ts = measure(lfs, label.c_str());
      dl->AddText(ImGui::GetFont(), lfs, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), fg, label.c_str());
    }
    bool groupBadge = e.group && groups[e.group].first != RNF_GROUP_CUSTOM;
    std::string badge = reserved ? std::string(TR("Menu")) : str(rnf_input_element_badge(b.data(), b.size(), el.c_str(), slot));
    if (!groupBadge && !badge.empty()) {
      ImVec2 ts = measure(fs, badge.c_str());
      float pw = ts.x + S(10), ph = ts.y + S(3);
      ImVec2 bp = P(e.badge_x, e.badge_y);
      switch (e.badge_side) {
        case RNF_SIDE_LEFT: bp = ImVec2(bp.x - pw, bp.y - ph / 2); break;
        case RNF_SIDE_RIGHT: bp = ImVec2(bp.x, bp.y - ph / 2); break;
        case RNF_SIDE_ABOVE: bp = ImVec2(bp.x - pw / 2, bp.y - ph); break;
        case RNF_SIDE_BELOW: bp = ImVec2(bp.x - pw / 2, bp.y); break;
      }
      ImU32 bc = reserved ? IM_COL32(110, 110, 120, 230) : hotkey ? IM_COL32(255, 149, 0, 235) : IM_COL32(40, 110, 230, 230);
      dl->AddRectFilled(bp, ImVec2(bp.x + pw, bp.y + ph), bc, ph / 2);
      dl->AddText(ImGui::GetFont(), fs, ImVec2(bp.x + S(5), bp.y + S(1.5f)), IM_COL32(255, 255, 255, 255), badge.c_str());
    }
    if (focused) focusRect = LRect{c.x - std::max(w, S(18)) / 2 - S(2), c.y - std::max(h, S(18)) / 2 - S(2), std::max(w, S(18)) + S(4),
                                   std::max(h, S(18)) + S(4)};
  }
  for (auto& [g, v] : groups) {
    if (v.first != RNF_GROUP_MOVEMENT) continue;
    double ax = g == "dpad" ? info.dpad_anchor_x : g == "lstick" ? info.lstick_anchor_x : info.rstick_anchor_x;
    double ay = g == "dpad" ? info.dpad_anchor_y : g == "lstick" ? info.lstick_anchor_y : info.rstick_anchor_y;
    ImVec2 ts = measure(fs, v.second.c_str());
    ImVec2 bp = P(ax, ay);
    bp.x -= (ts.x + S(10)) / 2;
    dl->AddRectFilled(bp, ImVec2(bp.x + ts.x + S(10), bp.y + ts.y + S(3)), IM_COL32(40, 110, 230, 230), (ts.y + S(3)) / 2);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(bp.x + S(5), bp.y + S(1.5f)), IM_COL32(255, 255, 255, 255), v.second.c_str());
  }
  if (focusRect.w > 0) drawFocus(focusRect, focusRect.h / 2, now);
  // Legend: game / hotkeys, and the Quick Menu chord (not a single button on the picture).
  float y = area.bottom() - legendH + m.hint() * 0.6f, x = area.x + S(6);
  auto chip = [&](ImU32 c, const std::string& t) {
    dl->AddRectFilled(ImVec2(x, y + m.hint() * 0.15f), ImVec2(x + S(22), y + m.hint() * 0.85f), c, S(5));
    x += S(30);
    dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(x, y), kTextDim, t.c_str());
    x += measure(m.hint(), t.c_str()).x + S(22);
  };
  chip(IM_COL32(40, 110, 230, 230), TR("Game input (recorded)"));
  chip(IM_COL32(255, 149, 0, 235), TR("Hotkeys (not recorded)"));
  std::string menuHint = menuChordGlyph(family, true) + ": " + TR("Quick Menu");
  dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(x, y), kTextDim, menuHint.c_str());
  description_ = diagramFocus_ < int(diagramIds_.size())
                     ? str(rnf_input_element_title(diagramIds_[size_t(diagramFocus_)].substr(prefix.size()).c_str(), family, nullptr))
                     : std::string();
}

// The action picker of a controller button: the menu page "controls.assign" (rows of
// rnf_input_assign_choices, six per sheet, L / R; focus on the action assigned now).
void UI::openAssign(const std::string& physicalId) {
  std::string el = physicalId.substr(physicalId.find(':') + 1);
  if (InputRouter::isReserved(el)) {
    notice(TR("R3: ReplayNES menu (reserved)"));
    return;
  }
  assignElement_ = physicalId;
  int slot = 0;
  rnf_input_controller_slot(physicalId.c_str(), &slot);
  const std::vector<rnf_binding>& b = d_.input->bindings();
  rnf_list* acts = rnf_input_element_actions(b.data(), b.size(), el.c_str(), slot);
  std::string current = rnf_list_count(acts) ? rnf_list_a(acts, 0) : "";
  rnf_list_free(acts);
  std::vector<const char*> choices(rnf_input_assign_choices(slot, nullptr, 0));
  rnf_input_assign_choices(slot, choices.data(), choices.size());
  syncMenuCounts();
  if (rnf_menu_push(menu_, "controls.assign") != RNF_MENU_EVENT_PUSHED) return;
  for (size_t i = 0; i < choices.size(); ++i)
    if (current == choices[i]) rnf_menu_set_focus(menu_, i);
  pageChangedAt_ = -1;
}

}  // namespace rnl
