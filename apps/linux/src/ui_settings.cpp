// Settings: display (Integer / FILL, 8:7, overscan, flash reduction, UI scale), audio + controls,
// the controller diagram (rnf diagram geometry; click / A on an element to assign, held buttons
// light up, per-pad reset) and the keyboard / game input bindings (press-to-assign, turbo, SOCD).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <map>
#include <string>

#include "app_model.h"
#include "emulation.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "paths.h"
#include "settings.h"
#include "ui.h"
#include "ui_widgets.h"

namespace rnl {

namespace {
std::string str(char* p) {
  std::string s = p ? p : "";
  rnf_string_free(p);
  return s;
}
std::string displayName(const InputRouter& in, const std::string& id) {
  int slot = 0;
  const char* face = nullptr;
  std::string f;
  if (rnf_input_controller_slot(id.c_str(), &slot) && slot >= 0 && slot < InputRouter::kSlots) {
    const PadInfo& p = in.pad(slot);
    size_t colon = id.find(':');
    if (p.pad && colon != std::string::npos) {
      auto it = p.labels.find(id.substr(colon + 1));
      if (it != p.labels.end()) {
        f = it->second;
        face = f.c_str();
      }
    }
  }
  return str(rnf_input_display_name(RNF_KEYBOARD_SDL, id.c_str(), face));
}
void wrappedDisabled(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}
}  // namespace

void UI::buildSettings() {
  // Tabs: L1 / R1 (and mouse / touch); not in the D-pad focus order, so up / down stay on the page.
  const char* pages[] = {TR("Display"), TR("Audio & Controls"), TR("Controller"), TR("Game Input & Hotkeys")};
  rnf_controller_family fam = promptFamily();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float gh = ImGui::GetFrameHeight() * 0.8f;
  auto glyphHere = [&](const char* el) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = glyph(dl, ImVec2(p.x, p.y + (ImGui::GetFrameHeight() - gh) * 0.5f), el, fam, gh);
    ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
  };
  glyphHere("leftShoulder");
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  for (int i = 0; i < 4; ++i) {
    ImGui::SameLine(0, S(6));
    bool sel = settingsPage_ == i;
    ImGui::PushStyleColor(ImGuiCol_Button, sel ? ImVec4(0.30f, 0.42f, 0.62f, 1) : ImVec4(0.18f, 0.19f, 0.23f, 1));
    if (ImGui::Button(pages[i])) {
      settingsPage_ = i;
      focusFirst_ = true;
    }
    ImGui::PopStyleColor();
  }
  ImGui::PopItemFlag();
  ImGui::SameLine(0, S(6));
  glyphHere("rightShoulder");
  ImGui::Separator();
  ImGui::BeginChild("##settingspage", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
  if (focusFirst_) {
    ui::FocusNextItem();
    focusFirst_ = false;
  }
  switch (settingsPage_) {
    case 0: buildDisplaySettings(); break;
    case 1: buildAudioControlSettings(); break;
    case 2: buildControllerSettings(); break;
    case 3: buildInputSettings(); break;
  }
  scrollWithRightStick();
  ImGui::EndChild();
}

void UI::buildDisplaySettings() {
  Settings& s = *d_.settings;
  ImGui::SeparatorText(TR("Display"));
  ImGui::TextUnformatted(TR("Display Size"));
  if (ImGui::RadioButton(TR("Pixel-perfect (largest integer scale that fits)"), s.integerScale)) {
    s.integerScale = true;
    changed();
  }
  if (ImGui::RadioButton(TR("FILL (fill the window, aspect ratio kept)"), !s.integerScale)) {
    s.integerScale = false;
    changed();
  }
  if (ImGui::Checkbox(TR("8:7 pixel aspect ratio (as on a CRT TV)"), &s.par87)) changed();
  if (ImGui::Checkbox(TR("Hide overscan (8 px on each side)"), &s.hideOverscan)) changed();
  bool fs = isFullscreen && isFullscreen();
  if (ImGui::Checkbox(TR("Full Screen"), &fs) && onFullscreen) onFullscreen(fs);
  if (ImGui::Checkbox(TR("Show latency measurement"), &s.showStats)) changed();
  float ui = s.uiScale;
  ImGui::SetNextItemWidth(S(360));
  if (ImGui::SliderFloat(TR("UI Size"), &ui, 0.75f, 1.5f, "%.2f×")) {
    s.uiScale = std::round(ui * 20.0f) / 20.0f;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) changed();
  ImGui::SeparatorText(TR("Flash Reduction (Photosensitivity)"));
  for (int l = 0; l <= 3; ++l) {
    if (l) ImGui::SameLine();
    std::string label = str(rnf_flash_level_label(rn_flash_level(l)));
    if (ImGui::RadioButton((label + "##flash" + std::to_string(l)).c_str(), s.flash == l)) {
      s.flash = l;
      changed();
    }
  }
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(str(rnf_flash_level_detail(rn_flash_level(s.flash))).c_str());
  ImGui::PopTextWrapPos();
  wrappedDisabled(TR("Detects scenes where the whole screen flashes hard (explosions, lightning, …) and holds only the display "
                     "toward the darker side to reduce the number of flashes (based on the WCAG 2.x general flash and red flash "
                     "thresholds). Small flashes and normal scrolling are shown as they are. Recorded input, game progress and "
                     "reproducibility are not affected. It can also be applied to MP4 exports."));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.65f, 0.25f, 1));
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(TR("Caution: this does not reliably prevent photosensitive seizures. If you feel unwell, stop playing "
                            "immediately."));
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  if (ImGui::Checkbox(TR("Show “Flash Reduction Active” on screen while reducing"), &s.showFlashIndicator)) changed();
  buildCrtSettings();
}

void UI::buildAudioControlSettings() {
  Settings& s = *d_.settings;
  ImGui::SeparatorText(TR("Audio"));
  ImGui::SetNextItemWidth(S(420));
  float vol = s.volume * 100.0f;
  if (ImGui::SliderFloat(TR("Volume"), &vol, 0, 100, "%.0f%%")) {
    s.volume = vol / 100.0f;
    changed();
  }
  wrappedDisabled(TR("Audio is muted while paused, rewinding, seeking, in slow motion or fast-forwarding."));
  ImGui::SeparatorText(TR("Controls"));
  if (ImGui::Checkbox(TR("Pause when rewind / fast-forward is released"), &s.pauseAfterRewind)) changed();
  wrappedDisabled(TR("While paused, move the focus up to the timeline: the D-pad ←/→ steps back / advances (hold for faster)."));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Autosave interval"));
  for (double v : {2.0, 3.0, 5.0, 10.0, 30.0}) {
    ImGui::SameLine();
    std::string l = v == 2 ? TR("2 s") : v == 3 ? TR("3 s") : v == 5 ? TR("5 s") : v == 10 ? TR("10 s") : TR("30 s");
    if (ImGui::RadioButton((l + "##as").c_str(), s.autosaveInterval == v)) {
      s.autosaveInterval = v;
      changed();
    }
  }
  // Text fields (ui_osk.cpp).
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("On-screen keyboard"));
  const std::pair<const char*, const char*> oskModes[] = {
      {"auto", TR("Auto (Steam’s if available, otherwise built-in)")}, {"builtin", TR("Built-in")}, {"steam", "Steam"}};
  for (const auto& [v, l] : oskModes) {
    ImGui::SameLine();
    if (ImGui::RadioButton((std::string(l) + "##osk").c_str(), s.onScreenKeyboard == v)) {
      s.onScreenKeyboard = v;
      changed();
    }
  }
  wrappedDisabled(TR("For text fields (Search ROMs, names). Built-in: ReplayNES’s controller keyboard (A type, B delete, X space, "
                     "Y Shift, L1 / R1 move the cursor, View (⧉) symbols, Menu (≡) done). Steam: Steam’s on-screen keyboard "
                     "(Gaming Mode; it can type Japanese). Auto asks for Steam’s and shows the built-in one when a button "
                     "press still reaches ReplayNES (Steam’s keyboard did not open)."));
  ImGui::SeparatorText(TR("Language"));
  ImGui::TextUnformatted(std::string(rnf_l10n_language()) == "ja" ? TR("Japanese (follows the system language)")
                                                                   : TR("English (follows the system language)"));
  wrappedDisabled(TR("ReplayNES uses Japanese when the first language of the system is Japanese, English otherwise."));
  ImGui::SeparatorText(TR("Files"));
  wrappedDisabled(TRF("Settings file: %@", {Paths::display(d_.app->paths().settingsFile())}).c_str());
  wrappedDisabled(TRF("Temporary session: %@", {Paths::display(d_.app->paths().tempProject())}).c_str());
  ImGui::SeparatorText(TR("System"));
  bool busy = steamJob_.valid();
  ImGui::BeginDisabled(busy);
  if (ImGui::Button((std::string(busy ? TR("Adding to Steam…") : TR("Add to Steam")) + "##addtosteam").c_str())) startAddToSteam();
  ImGui::EndDisabled();
  wrappedDisabled(TR("Adds ReplayNES to the Steam library with its artwork, for every Steam account on this device. Close Steam "
                     "first (Desktop Mode)."));
  buildUpdateSettings();  // ui_update.cpp
}

// "Add to Steam": runs steam::addToSteam off the render thread (it reads / writes the Steam folders); the result
// is shown in a dialog by pollAddToSteam().
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

// ------------------------------------------------------------------ controller diagram

void UI::buildControllerSettings() {
  Settings& s = *d_.settings;
  InputRouter& in = *d_.input;
  int slot = std::clamp(s.diagramSlot, 0, InputRouter::kSlots - 1);
  const PadInfo& pad = in.pad(slot);
  // Pad picker.
  for (int i = 0; i < InputRouter::kSlots; ++i) {
    if (i % 2) ImGui::SameLine(S(520));
    const PadInfo& p = in.pad(i);
    std::string player = i == 0 ? "1P" : i == 1 ? "2P" : "—";
    std::string label = TRF("Pad %lld (%@): %@", {i + 1, player, p.pad ? p.name : std::string(TR("Not Connected"))});
    if (ImGui::RadioButton((label + "##pad").c_str(), slot == i)) {
      s.diagramSlot = i;
      changed();
    }
  }
  // Layout (family) picker.
  rnf_controller_family family = pad.pad ? pad.family : RNF_FAMILY_STEAM_DECK;
  bool autoFamily = s.diagramFamily == "auto";
  if (!autoFamily) family = rnf_controller_family(std::clamp(std::atoi(s.diagramFamily.c_str()), 0, 4));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Layout"));
  ImGui::SameLine();
  if (ImGui::RadioButton(TR("Automatic"), autoFamily)) {
    s.diagramFamily = "auto";
    changed();
  }
  for (int f : {RNF_FAMILY_STEAM_DECK, RNF_FAMILY_XBOX, RNF_FAMILY_PLAYSTATION, RNF_FAMILY_NINTENDO, RNF_FAMILY_GENERIC}) {
    ImGui::SameLine();
    std::string t = str(rnf_controller_family_title(rnf_controller_family(f)));
    if (ImGui::RadioButton((t + "##fam").c_str(), !autoFamily && int(family) == f)) {
      s.diagramFamily = std::to_string(f);
      changed();
    }
  }
  if (pad.pad) {
    std::string kind = str(rnf_controller_family_title(pad.family));
    wrappedDisabled(TRF("%@ (%@) · Pressed buttons light up · Click a button on the picture to change its assignment",
                        {pad.name, kind})
                        .c_str());
  } else {
    wrappedDisabled(TR("Not connected (once connected, pressed buttons light up on the picture) · Click a button on the picture "
                       "to change its assignment"));
  }
  buildDiagram(family, slot, std::min(ImGui::GetContentRegionAvail().x, S(760)));
  // Legend + reset.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(ImVec2(p.x, p.y + S(6)), ImVec2(p.x + S(24), p.y + S(18)), IM_COL32(40, 110, 230, 220), S(6));
  ImGui::SetCursorScreenPos(ImVec2(p.x + S(30), p.y));
  ImGui::TextDisabled("%s", TR("Game input (recorded)"));
  ImGui::SameLine(0, S(24));
  p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(ImVec2(p.x, p.y + S(6)), ImVec2(p.x + S(24), p.y + S(18)), IM_COL32(255, 149, 0, 230), S(6));
  ImGui::SetCursorScreenPos(ImVec2(p.x + S(30), p.y));
  ImGui::TextDisabled("%s", TR("Hotkeys (not recorded)"));
  ImGui::SameLine(0, S(24));
  ImGui::TextDisabled("%s", TR("R3 / Guide: ReplayNES menu (reserved)"));
  if (ImGui::Button(TRF("Reset Pad %lld to Defaults", {slot + 1}).c_str())) in.resetController(slot);
}

void UI::buildDiagram(rnf_controller_family family, int slot, float width) {
  InputRouter& in = *d_.input;
  const std::vector<rnf_binding>& b = in.bindings();
  const PadInfo& pad = in.pad(slot);
  float k = width / float(RNF_DIAGRAM_CANVAS_WIDTH);
  float height = float(RNF_DIAGRAM_CANVAS_HEIGHT) * k + S(24);
  ImVec2 o = ImGui::GetCursorScreenPos();
  o.x += S(4);
  o.y += S(16);
  auto P = [&](double x, double y) { return ImVec2(o.x + float(x) * k, o.y + float(y) * k); };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  rnf_diagram_info info{};
  rnf_diagram_info_get(family, &info);
  // Body: a rounded body and two grips, drawn as one opaque silhouette (outline of the union).
  bool sym = rnf_controller_family_is_symmetric(family) != 0;
  float lx = sym ? 175 : 168;
  ImU32 bodyFill = IM_COL32(44, 47, 56, 255), bodyLine = IM_COL32(255, 255, 255, 110);
  for (float gx : {lx, 560 - lx}) dl->AddEllipse(P(gx, 192), ImVec2(64 * k, 60 * k), bodyLine, 0, 0, 3.0f);
  for (float gx : {lx, 560 - lx}) dl->AddEllipseFilled(P(gx, 192), ImVec2(64 * k, 60 * k), bodyFill);
  dl->AddRect(P(92, 70), P(468, 202), bodyLine, 50 * k, 0, 1.5f);
  dl->AddRectFilled(P(92 + 1, 70 + 1), P(468 - 1, 202 - 1), bodyFill, 50 * k);
  // The body's own outline is hidden where the grips continue below it.
  dl->PushClipRect(P(0, 150), P(560, 262), true);
  for (float gx : {lx, 560 - lx}) dl->AddEllipseFilled(P(gx, 192), ImVec2(64 * k - 1.5f, 60 * k - 1.5f), bodyFill);
  dl->PopClipRect();
  if (info.has_touchpad)
    dl->AddRectFilled(P(info.touchpad_x - info.touchpad_width / 2, info.touchpad_y - info.touchpad_height / 2),
                      P(info.touchpad_x + info.touchpad_width / 2, info.touchpad_y + info.touchpad_height / 2),
                      IM_COL32(255, 255, 255, 30), 8 * k);
  for (auto [x, y] : {std::pair<double, double>{info.left_stick_x, info.left_stick_y}, {info.right_stick_x, info.right_stick_y}}) {
    dl->AddCircleFilled(P(x, y), float(info.stick_radius) * k, IM_COL32(255, 255, 255, 40));
    dl->AddCircle(P(x, y), float(info.stick_radius) * k, IM_COL32(255, 255, 255, 120));
  }
  dl->AddRectFilled(P(info.dpad_x - RNF_DIAGRAM_DPAD_ARM / 2, info.dpad_y - RNF_DIAGRAM_DPAD_ARM / 2),
                    P(info.dpad_x + RNF_DIAGRAM_DPAD_ARM / 2, info.dpad_y + RNF_DIAGRAM_DPAD_ARM / 2), IM_COL32(255, 255, 255, 80));
  // Group summaries ("Move" on the D-pad / sticks).
  std::map<std::string, std::pair<rnf_group_summary, std::string>> groups;
  for (const char* g : {"dpad", "lstick", "rstick"}) {
    char* text = nullptr;
    rnf_group_summary gs = rnf_input_group_summary(b.data(), b.size(), g, slot, &text);
    groups[g] = {gs, str(text)};
  }
  std::string prefix = "gc" + std::to_string(slot) + ":";
  size_t n = rnf_diagram_element_count(family);
  ImGui::PushFont(nullptr, std::max(S(11), 12 * k));
  for (size_t i = 0; i < n; ++i) {
    rnf_diagram_element e{};
    if (!rnf_diagram_element_get(family, i, &e)) continue;
    std::string el = e.element, id = prefix + el;
    ImVec2 c = P(e.cx, e.cy);
    float w = float(e.width) * k, h = float(e.height) * k;
    ImVec2 a(c.x - w / 2, c.y - h / 2), z(c.x + w / 2, c.y + h / 2);
    // Touch-friendly hit area (at least 30 px).
    float hw = std::max(w, S(30)), hh = std::max(h, S(30));
    ImGui::SetCursorScreenPos(ImVec2(c.x - hw / 2, c.y - hh / 2));
    ImGui::PushID(el.c_str());
    bool clicked = ImGui::InvisibleButton("##el", ImVec2(hw, hh));
    bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::PopID();
    bool reserved = InputRouter::isReserved(el);
    if (clicked) {
      if (reserved) notice(TR("R3 / Guide: ReplayNES menu (reserved)"));
      else assignElement_ = id;
    }
    rnf_list* acts = rnf_input_element_actions(b.data(), b.size(), el.c_str(), slot);
    bool assigned = rnf_list_count(acts) > 0;
    bool hotkey = false;
    for (size_t j = 0; j < rnf_list_count(acts); ++j) hotkey = hotkey || std::string(rnf_list_a(acts, j)).rfind("hk.", 0) == 0;
    rnf_list_free(acts);
    bool down = in.pressed().count(id) > 0;
    ImU32 fill = down ? IM_COL32(255, 196, 64, 255) : assigned ? IM_COL32(60, 64, 76, 255) : IM_COL32(255, 255, 255, 22);
    ImU32 line = hovered ? IM_COL32(255, 200, 64, 255) : IM_COL32(255, 255, 255, assigned ? 190 : 100);
    float lw = hovered ? 2.5f : 1.0f;
    ImU32 fg = down ? IM_COL32(0, 0, 0, 255) : assigned ? IM_COL32(240, 240, 245, 255) : IM_COL32(170, 170, 180, 255);
    std::string label;
    auto it = pad.labels.find(el);
    if (pad.pad && it != pad.labels.end() && family == pad.family) label = it->second;
    else label = rnf_controller_family_label(family, el.c_str());
    switch (e.kind) {
      case RNF_DIAGRAM_FACE:
      case RNF_DIAGRAM_HOME:
      case RNF_DIAGRAM_STICK_CLICK:
        if (e.kind != RNF_DIAGRAM_STICK_CLICK || down || hovered) dl->AddCircleFilled(c, w / 2, fill);
        dl->AddCircle(c, w / 2, line, 0, lw);
        break;
      case RNF_DIAGRAM_DPAD: dl->AddRectFilled(a, z, fill); dl->AddRect(a, z, line, 0, 0, lw); break;
      case RNF_DIAGRAM_STICK_DIRECTION: {
        std::string dir = el.substr(el.find('.') + 1);
        ImVec2 d = dir == "up" ? ImVec2(0, -1) : dir == "down" ? ImVec2(0, 1) : dir == "left" ? ImVec2(-1, 0) : ImVec2(1, 0);
        float r = w * 0.4f;
        ImVec2 tip(c.x + d.x * r, c.y + d.y * r), b1(c.x - d.x * r * 0.4f + d.y * r, c.y - d.y * r * 0.4f + d.x * r),
            b2(c.x - d.x * r * 0.4f - d.y * r, c.y - d.y * r * 0.4f - d.x * r);
        dl->AddTriangleFilled(tip, b1, b2, down ? IM_COL32(255, 196, 64, 255) : fg);
        if (hovered) dl->AddCircle(c, w * 0.6f, line, 0, lw);
        break;
      }
      default:
        dl->AddRectFilled(a, z, fill, (e.kind == RNF_DIAGRAM_TRIGGER ? 9 : 7) * k);
        dl->AddRect(a, z, line, (e.kind == RNF_DIAGRAM_TRIGGER ? 9 : 7) * k, 0, lw);
        break;
    }
    if (!label.empty() && e.kind != RNF_DIAGRAM_STICK_DIRECTION && e.kind != RNF_DIAGRAM_DPAD) {
      ImVec2 ts = ImGui::CalcTextSize(label.c_str());
      dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), fg, label.c_str());
    }
    // Assignment badge (a grouped "Move" badge replaces the four direction badges).
    bool groupBadge = e.group && groups[e.group].first != RNF_GROUP_CUSTOM;
    std::string badge = reserved ? std::string(TR("Menu")) : str(rnf_input_element_badge(b.data(), b.size(), el.c_str(), slot));
    if (!groupBadge && !badge.empty()) {
      ImVec2 ts = ImGui::CalcTextSize(badge.c_str());
      float pw = ts.x + S(10), ph = ts.y + S(2);
      ImVec2 bp = P(e.badge_x, e.badge_y);
      switch (e.badge_side) {
        case RNF_SIDE_LEFT: bp = ImVec2(bp.x - pw, bp.y - ph / 2); break;
        case RNF_SIDE_RIGHT: bp = ImVec2(bp.x, bp.y - ph / 2); break;
        case RNF_SIDE_ABOVE: bp = ImVec2(bp.x - pw / 2, bp.y - ph); break;
        case RNF_SIDE_BELOW: bp = ImVec2(bp.x - pw / 2, bp.y); break;
      }
      ImU32 bc = reserved ? IM_COL32(120, 120, 130, 230) : hotkey ? IM_COL32(255, 149, 0, 235) : IM_COL32(40, 110, 230, 220);
      dl->AddRectFilled(bp, ImVec2(bp.x + pw, bp.y + ph), bc, ph / 2);
      dl->AddText(ImVec2(bp.x + S(5), bp.y + S(1)), IM_COL32(255, 255, 255, 255), badge.c_str());
    }
  }
  for (auto& [g, v] : groups) {
    if (v.first != RNF_GROUP_MOVEMENT) continue;
    double ax = g == "dpad" ? info.dpad_anchor_x : g == "lstick" ? info.lstick_anchor_x : info.rstick_anchor_x;
    double ay = g == "dpad" ? info.dpad_anchor_y : g == "lstick" ? info.lstick_anchor_y : info.rstick_anchor_y;
    ImVec2 ts = ImGui::CalcTextSize(v.second.c_str());
    ImVec2 bp = P(ax, ay);
    bp.x -= (ts.x + S(10)) / 2;
    dl->AddRectFilled(bp, ImVec2(bp.x + ts.x + S(10), bp.y + ts.y + S(2)), IM_COL32(40, 110, 230, 220), (ts.y + S(2)) / 2);
    dl->AddText(ImVec2(bp.x + S(5), bp.y + S(1)), IM_COL32(255, 255, 255, 255), v.second.c_str());
  }
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(o.x - S(4), o.y - S(16) + height));
  ImGui::Dummy(ImVec2(width, S(4)));
}

void UI::buildAssignPicker() {
  if (assignElement_.empty() || !dialogs_.empty()) return;
  InputRouter& in = *d_.input;
  ImGuiIO& io = ImGui::GetIO();
  if (!ImGui::IsPopupOpen("##assign")) ImGui::OpenPopup("##assign");
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(S(560), io.DisplaySize.x * 0.9f), std::min(S(640), io.DisplaySize.y * 0.92f)));
  bool close = false;
  if (ImGui::BeginPopupModal("##assign", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
    int slot = 0;
    rnf_input_controller_slot(assignElement_.c_str(), &slot);
    std::string el = assignElement_.substr(assignElement_.find(':') + 1);
    const PadInfo& pad = in.pad(slot);
    rnf_controller_family fam = pad.pad ? pad.family : RNF_FAMILY_STEAM_DECK;
    if (d_.settings->diagramFamily != "auto") fam = rnf_controller_family(std::clamp(std::atoi(d_.settings->diagramFamily.c_str()), 0, 4));
    ImGui::PushFont(nullptr, S(24));
    ImGui::TextUnformatted(str(rnf_input_element_title(el.c_str(), fam, nullptr)).c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s", TR("Action for this button"));
    const std::vector<rnf_binding>& b = in.bindings();
    rnf_list* acts = rnf_input_element_actions(b.data(), b.size(), el.c_str(), slot);
    std::vector<std::string> current;
    for (size_t j = 0; j < rnf_list_count(acts); ++j) current.push_back(rnf_list_a(acts, j));
    rnf_list_free(acts);
    ImGui::BeginChild("##actions", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    bool appearing = ImGui::IsWindowAppearing();
    if (appearing) ImGui::SetKeyboardFocusHere();
    if (ImGui::Selectable((std::string(current.empty() ? "✓ " : "   ") + TR("None")).c_str())) {
      in.setAssignment(assignElement_, "");
      close = true;
    }
    std::vector<rnf_action_group> order = slot == 1 ? std::vector<rnf_action_group>{RNF_GROUP_PLAYER2, RNF_GROUP_PLAYER1, RNF_GROUP_HOTKEY}
                                                    : std::vector<rnf_action_group>{RNF_GROUP_PLAYER1, RNF_GROUP_PLAYER2, RNF_GROUP_HOTKEY};
    for (rnf_action_group g : order) {
      ImGui::SeparatorText(str(rnf_input_group_title(g)).c_str());
      for (size_t i = 0, n = rnf_input_action_count(); i < n; ++i) {
        rnf_action_info a{};
        if (!rnf_input_action_get(i, &a) || a.group != g) continue;
        bool on = std::find(current.begin(), current.end(), a.id) != current.end();
        std::string label = std::string(on ? "✓ " : "   ") + str(rnf_input_action_label(a.id)) + "##" + a.id;
        if (ImGui::Selectable(label.c_str())) {
          in.setAssignment(assignElement_, a.id);
          close = true;
        }
      }
    }
    ImGui::EndChild();
    if (current.size() > 1) ImGui::TextDisabled("%s", TR("Choosing one makes it the only action assigned to this button."));
    if (ImGui::Button(TR("Cancel"))) close = true;
    ImGui::SameLine(0, S(24));
    inlinePrompts(TR("Cancel"));
    if (!appearing && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) close = true;
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (close) assignElement_.clear();
}

// ------------------------------------------------------------------ bindings

void UI::buildInputSettings() {
  InputRouter& in = *d_.input;
  const std::vector<rnf_binding>& b = in.bindings();
  wrappedDisabled(TR("Hotkeys are handled separately from game input and are not recorded. They also work while paused."));
  if (ImGui::Button(TR("Reset to Defaults"))) in.resetToDefaults();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", TRF("Settings file: %@", {Paths::display(d_.app->paths().bindingsFile())}).c_str());
  for (rnf_action_group g : {RNF_GROUP_PLAYER1, RNF_GROUP_PLAYER2, RNF_GROUP_HOTKEY}) {
    ImGui::SeparatorText(str(rnf_input_group_title(g)).c_str());
    for (size_t i = 0, n = rnf_input_action_count(); i < n; ++i) {
      rnf_action_info a{};
      if (!rnf_input_action_get(i, &a) || a.group != g) continue;
      ImGui::PushID(a.id);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(str(rnf_input_action_label(a.id)).c_str());
      ImGui::SameLine(S(330));
      std::vector<std::string> ids;
      for (const rnf_binding& x : b)
        if (std::string(x.action) == a.id) ids.push_back(x.input);
      std::sort(ids.begin(), ids.end());
      if (ids.empty()) {
        ImGui::TextDisabled("%s", TR("Unassigned"));
        ImGui::SameLine();
      }
      for (const std::string& id : ids) {
        std::string chip = displayName(in, id) + "  ×##" + id;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(14));
        if (ImGui::Button(chip.c_str())) in.unbind(id, a.id);
        ImGui::PopStyleVar();
        ImGui::SameLine();
      }
      if (capturingAction_ == a.id) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", TR("Press a key/button… (Esc to cancel)"));
      } else if (ImGui::Button(TR("Add…"))) {
        capturingAction_ = a.id;
        std::string action = a.id;
        in.beginCapture([this, action](const std::string& id) {
          capturingAction_.clear();
          if (!id.empty() && !InputRouter::isReserved(id.substr(id.find(':') + 1))) d_.input->bind(id, action);
        });
      }
      ImGui::PopID();
    }
  }
  ImGui::SeparatorText(TR("Turbo (Turbo A / B)"));
  int period = in.turboPeriod(), duty = in.turboDuty();
  ImGui::SetNextItemWidth(S(360));
  if (ImGui::SliderInt("##period", &period, 2, 30, "%d")) in.setTurbo(period, std::min(duty, period - 1));
  ImGui::SameLine();
  ImGui::TextUnformatted(TRF("Period: %lld frames", {in.turboPeriod()}).c_str());
  ImGui::SetNextItemWidth(S(360));
  if (ImGui::SliderInt("##duty", &duty, 1, std::max(1, period - 1), "%d")) in.setTurbo(period, duty);
  ImGui::SameLine();
  ImGui::TextUnformatted(TRF("Press length: %lld frames", {in.turboDuty()}).c_str());
  wrappedDisabled(TRF("About %.1f presses per second. The recording stores the button state after turbo is applied, so playback "
                      "doesn’t depend on this setting.",
                      {60.0988 / std::max(1, period)})
                      .c_str());
  ImGui::SeparatorText(TR("Simultaneous Opposite Directions (SOCD)"));
  std::string socd = in.socd();
  if (ImGui::RadioButton(TR("Release both (neutral)"), socd == "neutral")) in.setSOCD("neutral");
  if (ImGui::RadioButton(TR("Last pressed wins"), socd == "last_wins")) in.setSOCD("last_wins");
  if (ImGui::RadioButton(TR("Press both (impossible on real hardware)"), socd == "allow")) in.setSOCD("allow");
  ImGui::SeparatorText(TR("Analog Stick"));
  float th = float(in.analogThreshold());
  ImGui::SetNextItemWidth(S(360));
  if (ImGui::SliderFloat("##threshold", &th, 0.2f, 0.9f, "%.2f")) in.setAnalogThreshold(th);
  char thText[16];
  std::snprintf(thText, sizeof thText, "%.2f", in.analogThreshold());
  ImGui::SameLine();
  ImGui::TextUnformatted(TRF("D-pad threshold %@", {std::string(thText)}).c_str());
}

}  // namespace rnl
