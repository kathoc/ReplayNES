// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "app_model.h"
#include "emulation.h"
#include "fonts.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "library.h"
#include "paths.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui_widgets.h"
#include "pad_nav.h"

namespace fs = std::filesystem;

namespace rnl {

UI::UI(Deps d) : d_(d) {}

bool UI::hasSession() const { return d_.emu->session() != nullptr; }

std::vector<UI::Tab> UI::tabs() const {
  if (!hasSession()) return {Tab::library, Tab::settings, Tab::guide};
  return {Tab::playback, Tab::takes, Tab::bookmarks, Tab::practice, Tab::settings, Tab::guide};
}

static const char* tabTitle(UI::Tab t) {
  switch (t) {
    case UI::Tab::playback: return TR("Playback");
    case UI::Tab::takes: return TR("Takes");
    case UI::Tab::bookmarks: return TR("Bookmarks");
    case UI::Tab::practice: return TR("Practice");
    case UI::Tab::library: return TR("Library");
    case UI::Tab::settings: return TR("Settings");
    case UI::Tab::guide: return TR("Controls Guide");
  }
  return "";
}

// ------------------------------------------------------------------ fonts / scale

bool UI::loadFonts(bool japanese) {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  FontFile latin = findLatinFont(), jp = findJapaneseFont(), sym = findSymbolFont();
  // The primary font sets the metrics: in Japanese the CJK font (its Latin glyphs match its kana
  // and kanji in size), in English the Latin UI font. The others fill in missing glyphs (ImGui
  // 1.92 loads glyphs on demand, falling through merged sources): the CJK font also in English
  // for names typed in Japanese.
  std::vector<FontFile> order;
  if (japanese && jp.found()) order = {jp, latin, sym};
  else order = {latin, jp, sym};
  font_ = nullptr;
  std::string used;
  for (const FontFile& f : order) {
    if (!f.found()) continue;
    ImFontConfig c;
    c.MergeMode = font_ != nullptr;
    c.FontNo = f.index;
    ImFont* r = io.Fonts->AddFontFromFileTTF(f.path.c_str(), 0.0f, &c);
    if (!r) continue;
    if (!font_) font_ = r;
    used += (used.empty() ? "" : " + ") + f.path + (f.index ? " #" + std::to_string(f.index) : std::string());
  }
  if (!font_) font_ = io.Fonts->AddFontDefault();
  std::fprintf(stderr, "fonts: %s\n", used.empty() ? "(ImGui default)" : used.c_str());
  return !japanese || jp.found();
}

void UI::updateScale(int windowHeight) {
  windowHeight_ = std::max(1, windowHeight);
  float s = std::clamp(float(windowHeight_) / 800.0f, 0.6f, 3.0f) * d_.settings->uiScale;
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 8;
  style.ChildRounding = 6;
  style.FrameRounding = 6;
  style.PopupRounding = 8;
  style.GrabRounding = 6;
  style.ScrollbarRounding = 8;
  style.FramePadding = ImVec2(12, 8);
  style.ItemSpacing = ImVec2(10, 8);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.WindowPadding = ImVec2(14, 12);
  style.ScrollbarSize = 18;
  style.GrabMinSize = 18;
  style.TouchExtraPadding = ImVec2(4, 4);
  style.WindowBorderSize = 0;
  style.PopupBorderSize = 1;
  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.97f);
  c[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.13f, 0.16f, 0.6f);
  c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.15f, 0.99f);
  c[ImGuiCol_Button] = ImVec4(0.22f, 0.24f, 0.29f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.34f, 0.42f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.36f, 0.44f, 0.58f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.24f, 0.30f, 0.42f, 1.0f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.38f, 0.52f, 1.0f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.34f, 0.44f, 0.60f, 1.0f);
  c[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
  style.ScaleAllSizes(s);
  style.FontSizeBase = 22.0f * s;
  style.FontScaleMain = 1.0f;
  scale_ = s;
}

// ------------------------------------------------------------------ state

bool UI::interactive() const {
  return menuOpen_ || !hasSession() || !dialogs_.empty() || chooser_.has_value() || rename_.has_value() ||
         !assignElement_.empty() || exportDialog_;
}

bool UI::wantsKeyboard() const { return ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }

void UI::padUsed() {
  if (ImGui::GetCurrentContext()) ImGui::SetNavCursorVisible(true);
}

void UI::setMenu(bool open) {
  if (!hasSession()) open = true;
  if (open == menuOpen_) return;
  menuOpen_ = open;
  if (open) {
    // Nothing reaches the game while the menu is up; the emulation waits paused.
    d_.emu->setRewindHeld(false);
    d_.emu->setFastForwardHeld(false);
    if (hasSession()) {
      d_.emu->setPaused(true);
      tab_ = Tab::playback;  // the hub
      hubFocus_ = Tab::playback;
    }
    focusFirst_ = true;
    menuOpenedFrame_ = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
  } else {
    d_.input->cancelCapture();
    capturingAction_.clear();
  }
}

void UI::openHub() { setMenu(true); }

void UI::closeHubAndResume() {
  setMenu(false);
  if (hasSession() && d_.emu->paused()) d_.emu->togglePause();
  lastPaused_ = false;
}

void UI::toggleMenu() {
  // A popup has the controls: B / Esc close it (handled by the popup itself).
  if (!dialogs_.empty() || chooser_ || rename_ || !assignElement_.empty() || exportDialog_) return;
  if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return;
  // R3 / Guide / Esc / F1: the hub from the game, back to the game from any page.
  if (menuOpen_ && hasSession()) closeHubAndResume();
  else setMenu(true);
}

void UI::selectTab(Tab t) {
  std::vector<Tab> v = tabs();
  if (std::find(v.begin(), v.end(), t) == v.end()) return;
  if (t != tab_) {
    if (t == Tab::guide) previousTab_ = tab_;
    if (t == Tab::playback && tab_ != Tab::playback) hubFocus_ = tab_;  // back on the hub: that page's button
    tab_ = t;
  }
  focusFirst_ = true;
}

void UI::applyTransition(const MenuTransition& t) {
  if (t.closeAndResume) closeHubAndResume();
  else selectTab(t.page);
}

void UI::notice(const std::string& text) {
  notice_ = text;
  noticeTime_ = -1;  // stamped on the next build (time of that frame)
}

void UI::showDialog(Dialog d) {
  if (d.buttons.empty()) d.buttons = {TR("OK")};
  dialogs_.push_back(std::move(d));
}

void UI::showChooser(ChooserRequest r) {
  Chooser c;
  c.req = std::move(r);
  std::error_code ec;
  fs::create_directories(c.req.root, ec);
  c.dir = c.req.startDir.empty() ? c.req.root : c.req.startDir;
  if (!fs::is_directory(c.dir, ec)) c.dir = c.req.root;
  std::snprintf(c.name, sizeof c.name, "%s", c.req.defaultName.c_str());
  chooser_ = std::move(c);
  chooserList();
}

bool UI::answerDialog(int button) {
  if (dialogs_.empty()) return false;
  dialogAnswer_ = button;
  return true;
}

void UI::askRename(const std::string& title, const std::string& initial, std::function<void(const std::string&)> apply) {
  Rename r;
  r.title = title;
  std::snprintf(r.buf, sizeof r.buf, "%s", initial.c_str());
  r.apply = std::move(apply);
  rename_ = std::move(r);
}

void UI::changed() {
  if (onSettingsChanged) onSettingsChanged();
}

// ------------------------------------------------------------------ layout

float UI::dockHeight(bool inMenu) const {
  // lane + strip + labels + transport row + padding
  return S(inMenu ? 146.0f : 176.0f);
}

static float headerHeight(float s) { return 60.0f * s; }

float UI::hubPanelHeight() const { return dockHeight(true) + S(76) + promptBarHeight(); }

GameRect UI::gameRect(int w, int h) const {
  if (!hasSession()) return GameRect{};
  const Settings& st = *d_.settings;
  if (menuOpen_ && tab_ != Tab::playback) return GameRect{};  // a full page covers it
  if (menuOpen_) {
    int top = int(headerHeight(scale_)), bottom = int(hubPanelHeight());
    GameRect r = computeGameRect(w, std::max(1, h - top - bottom), st.integerScale, st.par87, st.hideOverscan);
    r.y += float(top);
    r.crt.y += float(top);
    return r;
  }
  return computeGameRect(w, h, st.integerScale, st.par87, st.hideOverscan);
}

// ------------------------------------------------------------------ frame

void UI::build(double now) {
  if (noticeTime_ < 0) noticeTime_ = now;
  prompts_.clear();
  refreshUpdate();
  // Hold buttons (rewind / fast-forward) set these again while they are held.
  d_.emu->setRewindHeld(false);
  d_.emu->setFastForwardHeld(false);
  if (!hasSession()) {
    menuOpen_ = true;
    std::vector<Tab> v = tabs();
    if (std::find(v.begin(), v.end(), tab_) == v.end()) {
      tab_ = Tab::library;
      focusFirst_ = true;
    }
  } else if (!hadSession_) {
    // A game was started from the start screen: to the game (it runs) or the hub (it opened paused).
    menuOpen_ = false;
    tab_ = Tab::playback;
    lastPaused_ = false;
  }
  hadSession_ = hasSession();
  if (d_.app->takePracticePanelRequest()) practicePanel_ = true;
  const EmuStatus& st = d_.emu->status();
  if (st.practicing) practicePanel_ = true;
  // Pausing (R / Space, the end of a rewind, an opened project, ...) shows the hub.
  bool paused = hasSession() && st.paused;
  if (paused && !lastPaused_ && !menuOpen_ && drag_ == Drag::none) openHub();
  lastPaused_ = paused;

  if (menuOpen_) {
    buildMenu(now);
  } else {
    bool dockVisible = st.paused || now - lastPointer_ < 2.5 || drag_ != Drag::none;
    if (dockVisible) buildDock(false, now);
    if (practicePanel_ || st.practicing) buildPracticeOverlay(false);
  }
  buildOverlays(now);
  pollAddToSteam();
  buildDialogs();
  buildChooser();
  buildRename();
  buildAssignPicker();
  buildExportDialog();
  buildExportProgressPill();
  if (interactive()) drawFocusRing();
  popupLastFrame_ = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  activeLastFrame_ = ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput;
}

void UI::handleMenuGamepad() {
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || popupLastFrame_) return;
  if (!dialogs_.empty() || chooser_ || rename_ || !assignElement_.empty() || exportDialog_) return;
  // B / L1 / ... that ended a text field or a slider edit, or the press that opened the menu.
  if (activeLastFrame_ || ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput) return;
  if (ImGui::GetFrameCount() == menuOpenedFrame_) return;
  const bool session = hasSession();
  auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
  if (pressed(ImGuiKey_GamepadFaceRight)) return applyTransition(menuTransition(tab_, MenuCommand::back, session, previousTab_));
  if (pressed(ImGuiKey_GamepadStart)) return applyTransition(menuTransition(tab_, MenuCommand::menuButton, session, previousTab_));
  if (pressed(ImGuiKey_GamepadBack)) return applyTransition(menuTransition(tab_, MenuCommand::viewButton, session, previousTab_));
  int lr = (pressed(ImGuiKey_GamepadR1) ? 1 : 0) - (pressed(ImGuiKey_GamepadL1) ? 1 : 0);
  if (tab_ == Tab::settings) {
    if (lr != 0) {
      settingsPage_ = (settingsPage_ + lr + 4) % 4;
      focusFirst_ = true;
    }
  } else if (tab_ == Tab::playback && session) {
    // The hub: R resumes (it paused), L toggles slow like in play; the focused timeline uses them
    // for jumps itself. The triggers held: rewind / fast-forward, as assigned for play (L2 / R2).
    if (!timelineFocused_) {
      if (pressed(ImGuiKey_GamepadR1)) return closeHubAndResume();
      if (pressed(ImGuiKey_GamepadL1)) d_.emu->toggleSlow();
    }
    for (bool left : {true, false}) {
      if (!ImGui::IsKeyDown(left ? ImGuiKey_GamepadL2 : ImGuiKey_GamepadR2)) continue;
      std::string a = triggerAction(left);
      if (a == "hk.rewind") d_.emu->setRewindHeld(true);
      else if (a == "hk.fast_forward") d_.emu->setFastForwardHeld(true);
    }
  } else if (lr != 0 && session) {
    selectTab(cyclePage(tab_, lr));
  }
}

void UI::buildHeader() {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float hh = headerHeight(scale_);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(io.DisplaySize.x, hh), IM_COL32(20, 22, 27, 245));
  dl->AddLine(ImVec2(0, hh), ImVec2(io.DisplaySize.x, hh), IM_COL32(255, 255, 255, 30));
  ImGui::SetCursorPos(ImVec2(S(16), S(10)));
  const bool session = hasSession();
  const bool hub = session && tab_ == Tab::playback;
  // Right-aligned buttons: the hub has the project actions (D-pad up from the timeline), a page
  // "Back" (B; mouse / touch only), the start screen its pages + project actions.
  struct HB {
    std::string label;
    int id;
    bool selected;
  };
  std::vector<HB> buttons;
  if (hub) {
    buttons = {{TR("Save"), 2, false}, {std::string(TR("Project")) + "  ▾", 3, false}};
  } else if (session) {
    buttons = {{std::string("◀  ") + TR("Back"), 6, false}};
  } else {
    buttons = {{TR("Library"), 10, tab_ == Tab::library},
               {TR("Settings"), 11, tab_ == Tab::settings},
               {TR("Controls Guide"), 12, tab_ == Tab::guide},
               {TR("Open Project…"), 4, false},
               {TR("Quit"), 5, false}};
  }
  float total = 0;
  for (auto& b : buttons)
    total += ImGui::CalcTextSize(b.label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
  std::string title = !session ? std::string("ReplayNES") : hub ? d_.app->windowTitle() : std::string(tabTitle(tab_));
  float room = io.DisplaySize.x - total - S(40);
  ImGui::PushFont(nullptr, S(26));
  ImGui::AlignTextToFramePadding();
  ImVec2 tp = ImGui::GetCursorScreenPos();
  ImGui::PushClipRect(tp, ImVec2(tp.x + room, tp.y + S(48)), true);
  ImGui::TextUnformatted(title.c_str());
  ImGui::PopClipRect();
  float titleW = ImGui::GetItemRectSize().x;
  ImGui::PopFont();
  if (session && !hub) {
    // Pages: L1 / R1 cycle Practice, Takes, Bookmarks, Guide (Settings uses them for its tabs).
    ImGui::SameLine(0, S(24));
    buildPageTabs();
  } else if (!session) {
    const char* tagline = TR("Made a mistake? Go back and record it again. At the end, export one continuous play video.");
    if (titleW + ImGui::CalcTextSize(tagline).x + S(16) < room) {
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("%s", tagline);
    }
  }
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(12), io.DisplaySize.x - total - S(8)));
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, session && !hub);
  for (size_t i = 0; i < buttons.size(); ++i) {
    if (i) ImGui::SameLine();
    if (buttons[i].selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.42f, 0.62f, 1));
    if (ImGui::Button(buttons[i].label.c_str())) {
      switch (buttons[i].id) {
        case 2: d_.app->save(); break;
        case 3: ImGui::OpenPopup("##project"); break;
        case 4: d_.app->openProjectChooser(); break;
        case 5: if (onQuit) onQuit(); break;
        case 6: selectTab(Tab::playback); break;
        case 10: selectTab(Tab::library); break;
        case 11: selectTab(Tab::settings); break;
        case 12: selectTab(Tab::guide); break;
      }
    }
    if (buttons[i].selected) ImGui::PopStyleColor();
  }
  ImGui::PopItemFlag();
  if (ImGui::BeginPopup("##project")) {
    if (ImGui::Selectable(TR("Save As…"))) d_.app->saveAs();
    if (ImGui::Selectable(TR("Export…"), false, d_.emu->status().takeLength > 0 ? 0 : ImGuiSelectableFlags_Disabled))
      openExportDialog();
    if (ImGui::Selectable(TR("Open Project…"))) d_.app->openProjectChooser();
    ImGui::Separator();
    if (ImGui::Selectable(TR("Reset Project…"))) d_.app->resetProjectPrompt();
    if (ImGui::Selectable(TR("Close Project"))) d_.app->closeProject();
    ImGui::Separator();
    if (ImGui::Selectable(TR("Quit")) && onQuit) onQuit();
    ImGui::EndPopup();
  }
}

void UI::buildPageTabs() {
  static const Tab order[] = {Tab::practice, Tab::takes, Tab::bookmarks, Tab::guide};
  bool cycles = std::find(std::begin(order), std::end(order), tab_) != std::end(order);
  if (!cycles) return;
  rnf_controller_family f = promptFamily();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float gh = ImGui::GetFrameHeight() * 0.8f;
  auto glyphHere = [&](const char* el) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = glyph(dl, ImVec2(p.x, p.y + (ImGui::GetFrameHeight() - gh) * 0.5f), el, f, gh);
    ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
  };
  glyphHere("leftShoulder");
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  for (Tab t : order) {
    ImGui::SameLine(0, S(4));
    bool sel = t == tab_;
    ImGui::PushStyleColor(ImGuiCol_Button, sel ? ImVec4(0.30f, 0.42f, 0.62f, 1) : ImVec4(0.16f, 0.17f, 0.21f, 1));
    if (ImGui::Button(tabTitle(t))) selectTab(t);
    ImGui::PopStyleColor();
  }
  ImGui::PopItemFlag();
  ImGui::SameLine(0, S(4));
  glyphHere("rightShoulder");
}

void UI::buildHubButtons() {
  const EmuStatus& st = d_.emu->status();
  ImGuiIO& io = ImGui::GetIO();
  struct HubButton {
    Tab focusKey;  // which page's button (hub focus when coming back)
    const char* label;
    int id;
    bool enabled;
  };
  std::vector<HubButton> buttons = {
      {Tab::playback, TR("Resume"), 0, true},
      {Tab::library, TR("Back to Library"), 1, true},
      {Tab::settings, TR("Settings"), 2, true},
      {Tab::practice, TR("Practice"), 3, true},
      {Tab::takes, TR("Takes"), 4, true},
      {Tab::bookmarks, TR("Bookmarks"), 5, true},
      {Tab::playback, TR("Export…"), 6, st.takeLength > 0},
      {Tab::playback, TR("Reset…"), 7, true},
      {Tab::guide, TR("Controls Guide"), 8, true},
  };
  // An update waiting (in-app updates): its dialog (Update / Restart).
  std::string updateLabel;
  if (update_.noticeVisible() && (update_.phase == UpdatePhase::available || update_.phase == UpdatePhase::installed))
    buttons.push_back({Tab::playback, update_.phase == UpdatePhase::installed ? TR("Restart to Update") : TR("Update"), 9, true});
  else if (update_.noticeVisible() && update_.busy()) {
    updateLabel = TRF("Updating… %lld%%", {update_.percent});
    buttons.push_back({Tab::playback, updateLabel.c_str(), 9, false});
  }
  const int n = int(buttons.size());
  // Focus a button for two frames: ImGui otherwise restores the hub window's last focused item
  // when the page that was shown goes away.
  if (focusFirst_) hubFocusFrames_ = 2;
  focusFirst_ = false;  // a button chosen below may ask for the next page's focus
  const bool wantFocus = hubFocusFrames_ > 0;
  if (hubFocusFrames_ > 0) --hubFocusFrames_;
  const float gap = S(8), h = S(58), avail = io.DisplaySize.x - S(28);
  // Widths follow the labels; a smaller font when they don't fit in one row.
  float fs = S(21);
  float pad = S(26);
  auto widthAt = [&](float size) {
    float w = 0;
    for (const HubButton& b : buttons) w += std::max(S(92), ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, b.label).x + pad);
    return w + gap * float(n - 1);
  };
  while (fs > S(14) && widthAt(fs) > avail) fs -= S(1);
  float extra = std::max(0.0f, (avail - widthAt(fs)) / float(n));
  ImGui::SetCursorPosX(S(14));
  ImGui::PushFont(nullptr, fs);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(12));
  for (int i = 0; i < n; ++i) {
    const HubButton& b = buttons[i];
    if (i) ImGui::SameLine(0, gap);
    float w = std::max(S(92), ImGui::CalcTextSize(b.label).x + pad) + extra;
    bool focusThis = wantFocus && ((hubFocus_ == Tab::playback && i == 0) || (hubFocus_ != Tab::playback && b.focusKey == hubFocus_));
    bool primary = i == 0;
    if (primary) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.85f, 1));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.52f, 0.92f, 1));
    }
    ImGui::BeginDisabled(!b.enabled);
    bool clicked = ImGui::Button((std::string(b.label) + "##hub" + std::to_string(i)).c_str(), ImVec2(w, h));
    ImGui::EndDisabled();
    if (primary) ImGui::PopStyleColor(2);
    if (focusThis) {
      ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
      ImGui::SetNavCursorVisible(true);
    }
    if (!clicked) continue;
    switch (b.id) {
      case 0: closeHubAndResume(); break;
      case 1: d_.app->closeProject(); break;  // asks to save first when needed; then the Library
      case 2: selectTab(Tab::settings); break;
      case 3: selectTab(Tab::practice); break;
      case 4: selectTab(Tab::takes); break;
      case 5: selectTab(Tab::bookmarks); break;
      case 6: openExportDialog(); break;
      case 7: showResetChoices(); break;
      case 8: selectTab(Tab::guide); break;
      case 9: showUpdateDialog(); break;
    }
  }
  ImGui::PopStyleVar();
  ImGui::PopFont();
}

void UI::showResetChoices() {
  const EmuStatus& st = d_.emu->status();
  Dialog d;
  d.title = TR("Reset");
  d.message = TR("Soft Reset and Power Cycle act on the game (they are recorded like the console’s buttons). Reset Project "
                 "starts the whole recording over.");
  std::vector<int> actions;
  bool resetOK = st.recording || st.practicing;
  if (resetOK) {
    d.buttons.push_back(TR("Soft Reset"));
    actions.push_back(0);
    d.buttons.push_back(TR("Power Cycle (Off and On)"));
    actions.push_back(1);
  }
  d.buttons.push_back(TR("Reset Project…"));
  actions.push_back(2);
  d.buttons.push_back(TR("Cancel"));
  actions.push_back(3);
  d.cancelIndex = int(d.buttons.size()) - 1;
  d.onResult = [this, actions](int b, bool) {
    if (b < 0 || b >= int(actions.size())) return;
    switch (actions[size_t(b)]) {
      case 0: d_.emu->requestEvent(RN_EV_SOFT_RESET); closeHubAndResume(); break;
      case 1: d_.emu->requestEvent(RN_EV_POWER_CYCLE); closeHubAndResume(); break;
      case 2: d_.app->resetProjectPrompt(); break;
      default: break;
    }
  };
  showDialog(std::move(d));
}

void UI::scrollWithRightStick() {
  float v = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue - ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
  if (std::fabs(v) > 0.05f) ImGui::SetScrollY(ImGui::GetScrollY() + v * S(1100) * ImGui::GetIO().DeltaTime);
}

void UI::buildMenu(double now) {
  ImGuiIO& io = ImGui::GetIO();
  handleMenuGamepad();  // first: it may change the page (or close the menu) for this frame
  if (!menuOpen_) return;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  bool hub = tab_ == Tab::playback && hasSession();
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
  if (hub) flags |= ImGuiWindowFlags_NoBackground;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##menu", nullptr, flags);
  ImGui::PopStyleVar();
  buildHeader();
  float hh = headerHeight(scale_);
  float pb = promptBarHeight();
  timelineFocused_ = false;
  if (hub) {
    // Bottom panel: timeline + transport, the hub buttons, the prompts.
    float panel = hubPanelHeight();
    ImVec2 p0(0, io.DisplaySize.y - panel);
    ImGui::GetWindowDrawList()->AddRectFilled(p0, io.DisplaySize, IM_COL32(20, 22, 27, 245));
    ImGui::GetWindowDrawList()->AddLine(p0, ImVec2(io.DisplaySize.x, p0.y), IM_COL32(255, 255, 255, 30));
    ImGui::SetCursorPos(p0);
    buildDock(true, now);
    ImGui::SetCursorPos(ImVec2(0, io.DisplaySize.y - pb - S(72)));
    buildHubButtons();
    if (practicePanel_ || d_.emu->status().practicing) buildPracticeOverlay(false);
    // Prompts (the timeline adds its own while focused).
    if (!timelineFocused_) {
      prompt({"face.south"}, TR("Select"));
      prompt({"face.east", "rightShoulder"}, TR("Resume"));
      for (bool left : {true, false}) {
        std::string a = triggerAction(left);
        if (a == "hk.rewind") prompt({left ? "leftTrigger" : "rightTrigger"}, TR("Rewind (hold)"));
        else if (a == "hk.fast_forward") prompt({left ? "leftTrigger" : "rightTrigger"}, TR("Fast-forward (hold)"));
      }
    }
    prompt({"options"}, TR("Controls Guide"));
  } else {
    ImGui::SetCursorPos(ImVec2(S(16), hh + S(10)));
    ImGui::BeginChild("##page", ImVec2(io.DisplaySize.x - S(32), io.DisplaySize.y - hh - pb - S(14)),
                      ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    if (focusFirst_ && tab_ != Tab::library && tab_ != Tab::settings) {  // these focus their first item themselves
      ui::FocusNextItem();
      focusFirst_ = false;
    }
    switch (tab_) {
      case Tab::takes: buildTakes(); break;
      case Tab::bookmarks: buildBookmarks(); break;
      case Tab::practice: buildPracticeTab(); break;
      case Tab::library: buildLibrary(now); break;
      case Tab::settings: buildSettings(); break;
      case Tab::guide: buildGuide(); break;
      case Tab::playback: break;
    }
    scrollWithRightStick();
    ImGui::EndChild();
    bool session = hasSession();
    if (tab_ != Tab::library) {
      prompt({"face.south"}, TR("Select"));
      prompt({"face.east"}, session ? TR("Back to Menu") : TR("Back to Library"));
      if (tab_ == Tab::settings) prompt({"leftShoulder", "rightShoulder"}, TR("Switch Tabs"));
      else if (session) prompt({"leftShoulder", "rightShoulder"}, TR("Previous / Next Page"));
    }
    if (session) prompt({"menu"}, TR("Back to Menu"));
    else prompt({"menu"}, tab_ == Tab::settings ? TR("Back to Library") : TR("Settings"));
    prompt({"options"}, tab_ == Tab::guide ? TR("Close the Guide") : TR("Controls Guide"));
  }
  buildPromptBar();
  ImGui::End();
}

// ------------------------------------------------------------------ prompts / focus

std::string UI::triggerAction(bool left) const {
  // Pad 1's trigger hotkey (the hub mirrors the in-game assignment; defaults: L2 rewind, R2 FF).
  const char* id = left ? "gc0:leftTrigger" : "gc0:rightTrigger";
  for (const rnf_binding& b : d_.input->bindings())
    if (std::strcmp(b.input, id) == 0 && (std::strcmp(b.action, "hk.rewind") == 0 || std::strcmp(b.action, "hk.fast_forward") == 0))
      return b.action;
  return {};
}

rnf_controller_family UI::promptFamily() const {
  int slot = d_.input->lastSlot();
  if (slot >= 0 && d_.input->pad(slot).pad) return d_.input->pad(slot).family;
  for (int s = 0; s < InputRouter::kSlots; ++s)
    if (d_.input->pad(s).pad) return d_.input->pad(s).family;
  return RNF_FAMILY_STEAM_DECK;
}

void UI::prompt(std::initializer_list<const char*> elements, const std::string& text) {
  Prompt p;
  for (const char* e : elements) p.elements.push_back(e);
  p.text = text;
  prompts_.push_back(std::move(p));
}

float UI::glyph(ImDrawList* dl, ImVec2 p, const std::string& element, rnf_controller_family f, float h, bool draw) const {
  // Face buttons: a round button with its printed label (by position for the family); the D-pad:
  // a cross; others (shoulders, triggers, Menu / View): a rounded key cap with the label.
  ImU32 bg = IM_COL32(236, 238, 244, 255), fg = IM_COL32(24, 26, 32, 255);
  ImFont* font = ImGui::GetFont();
  if (element.rfind("dpad", 0) == 0) {
    float w = h;
    if (draw) {
      float a = h * 0.32f;
      ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
      dl->AddRectFilled(ImVec2(c.x - a * 0.5f, p.y + 1), ImVec2(c.x + a * 0.5f, p.y + h - 1), bg, a * 0.25f);
      dl->AddRectFilled(ImVec2(p.x + 1, c.y - a * 0.5f), ImVec2(p.x + w - 1, c.y + a * 0.5f), bg, a * 0.25f);
      ImU32 hl = IM_COL32(255, 196, 64, 255);
      if (element == "dpad.lr") {
        dl->AddRectFilled(ImVec2(p.x + 1, c.y - a * 0.5f), ImVec2(p.x + a, c.y + a * 0.5f), hl, a * 0.25f);
        dl->AddRectFilled(ImVec2(p.x + w - a, c.y - a * 0.5f), ImVec2(p.x + w - 1, c.y + a * 0.5f), hl, a * 0.25f);
      } else if (element == "dpad.ud") {
        dl->AddRectFilled(ImVec2(c.x - a * 0.5f, p.y + 1), ImVec2(c.x + a * 0.5f, p.y + a), hl, a * 0.25f);
        dl->AddRectFilled(ImVec2(c.x - a * 0.5f, p.y + h - a), ImVec2(c.x + a * 0.5f, p.y + h - 1), hl, a * 0.25f);
      }
    }
    return w;
  }
  std::string label = padGlyph(f, element.c_str());
  if (label.empty()) label = element;
  bool face = element.rfind("face.", 0) == 0;
  float fs = h * (face ? 0.66f : 0.56f);
  ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
  float w = face ? h : std::max(h * 1.3f, ts.x + h * 0.6f);
  if (draw) {
    if (face) dl->AddCircleFilled(ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.5f, bg);
    else dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.3f);
    dl->AddText(font, fs, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), fg, label.c_str());
  }
  return w;
}

void UI::buildPromptBar() {
  if (prompts_.empty()) return;
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float h = promptBarHeight();
  ImVec2 p0(0, io.DisplaySize.y - h);
  dl->AddRectFilled(p0, io.DisplaySize, IM_COL32(12, 13, 16, 250));
  dl->AddLine(p0, ImVec2(io.DisplaySize.x, p0.y), IM_COL32(255, 255, 255, 36));
  rnf_controller_family f = promptFamily();
  float gh = h * 0.62f, fs = S(18);
  ImFont* font = ImGui::GetFont();
  // Measure, then right-align (Steam's convention) and drop entries from the left when too wide.
  std::vector<float> widths;
  float total = 0;
  for (const Prompt& p : prompts_) {
    float w = 0;
    for (const std::string& e : p.elements) w += glyph(dl, ImVec2(), e, f, gh, false) + S(4);
    w += S(4) + font->CalcTextSizeA(fs, FLT_MAX, 0, p.text.c_str()).x + S(22);
    widths.push_back(w);
    total += w;
  }
  size_t first = 0;
  while (first + 1 < prompts_.size() && total > io.DisplaySize.x - S(24)) total -= widths[first++];
  float x = io.DisplaySize.x - S(12) - total;
  for (size_t i = first; i < prompts_.size(); ++i) {
    const Prompt& p = prompts_[i];
    for (const std::string& e : p.elements) x += glyph(dl, ImVec2(x, p0.y + (h - gh) * 0.5f), e, f, gh) + S(4);
    x += S(4);
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, p.text.c_str());
    dl->AddText(font, fs, ImVec2(x, p0.y + (h - ts.y) * 0.5f), IM_COL32(230, 232, 238, 255), p.text.c_str());
    x += ts.x + S(22);
  }
}

void UI::inlinePrompts(const char* backText) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  rnf_controller_family f = promptFamily();
  float gh = S(24), fs = S(17), fh = ImGui::GetFrameHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float x0 = p.x;
  p.y += (fh - gh) * 0.5f;
  ImFont* font = ImGui::GetFont();
  const std::pair<const char*, const char*> items[] = {{"face.south", TR("Select")}, {"face.east", backText}};
  for (const auto& [el, text] : items) {
    p.x += glyph(dl, p, el, f, gh) + S(6);
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, text);
    dl->AddText(font, fs, ImVec2(p.x, p.y + (gh - ts.y) * 0.5f), IM_COL32(200, 204, 212, 255), text);
    p.x += ts.x + S(20);
  }
  ImGui::Dummy(ImVec2(p.x - x0, fh));
}

void UI::drawFocusRing() {
  // A thick, high-contrast ring around the focused item (on top of ImGui's own thin one).
  ImGuiContext& g = *ImGui::GetCurrentContext();
  if (!g.NavCursorVisible || g.NavId == 0 || !g.NavIdIsAlive || !g.NavWindow || g.NavWindow->Hidden) return;
  if (g.IO.WantTextInput) return;
  ImGuiWindow* w = g.NavWindow;
  ImRect r = ImGui::WindowRectRelToAbs(w, w->NavRectRel[g.NavLayer]);
  if (r.GetWidth() <= 0 || r.GetHeight() <= 0) return;
  ImRect clip = w->InnerClipRect;
  if (!clip.Overlaps(r)) return;
  float t = std::max(2.0f, S(3));
  r.Expand(t + S(1));
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->PushClipRect(ImVec2(clip.Min.x - t * 2, clip.Min.y - t * 2), ImVec2(clip.Max.x + t * 2, clip.Max.y + t * 2), true);
  float rounding = ImGui::GetStyle().FrameRounding + t;
  dl->AddRect(ImVec2(r.Min.x - 1, r.Min.y - 1), ImVec2(r.Max.x + 1, r.Max.y + 1), IM_COL32(0, 0, 0, 200), rounding + 1, 0, t + 2);
  dl->AddRect(r.Min, r.Max, IM_COL32(255, 200, 64, 255), rounding, 0, t);
  dl->PopClipRect();
}

// ------------------------------------------------------------------ overlays

void UI::buildOverlays(double now) {
  ImGuiIO& io = ImGui::GetIO();
  bool showGameOverlays = hasSession() && (!menuOpen_ || tab_ == Tab::playback);
  if (showGameOverlays) buildBadges();
  if (!notice_.empty() && now - noticeTime_ < 4.0) {
    ImGui::PushFont(nullptr, S(19));
    ImVec2 ts = ImGui::CalcTextSize(notice_.c_str(), nullptr, false, io.DisplaySize.x * 0.8f);
    float bottom = io.DisplaySize.y - S(24);
    if (hasSession() && menuOpen_ && tab_ == Tab::playback) bottom -= hubPanelHeight();
    else if (hasSession() && !menuOpen_ && d_.emu->status().paused) bottom -= dockHeight(false);
    else if (menuOpen_) bottom -= promptBarHeight();
    ImVec2 pad(S(18), S(10));
    ImVec2 p0((io.DisplaySize.x - ts.x) * 0.5f - pad.x, bottom - ts.y - pad.y * 2);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float alpha = float(std::min(1.0, (4.0 - (now - noticeTime_)) / 0.4));
    dl->AddRectFilled(p0, ImVec2(p0.x + ts.x + pad.x * 2, p0.y + ts.y + pad.y * 2), IM_COL32(30, 32, 38, int(225 * alpha)), S(18));
    dl->AddText(nullptr, 0, ImVec2(p0.x + pad.x, p0.y + pad.y), IM_COL32(255, 255, 255, int(255 * alpha)), notice_.c_str(),
                nullptr, io.DisplaySize.x * 0.8f);
    ImGui::PopFont();
  }
  if (d_.settings->showStats && stats) buildStats();
}

void UI::buildStats() {
  StatsInfo s = stats();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - S(12), S(12)), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGui::Begin("##stats", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PushFont(nullptr, S(16));
  ImGui::Text("%.2f Hz %s  lead %.2f ms  sample->screen %.2f ms", s.refreshHz, s.cadence, s.leadMs, s.latencyMs);
  ImGui::Text("audio ratio %.4f  fill %.1f ms  underruns %llu  present_wait %s", s.audioRatio, s.audioFillMs,
              (unsigned long long)s.underruns, s.presentWait ? "yes" : "no");
  if (!s.crt.empty()) ImGui::TextUnformatted(s.crt.c_str());
  ImGui::PopFont();
  ImGui::End();
}

// ------------------------------------------------------------------ dialogs

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
    ImGui::PushFont(nullptr, S(24));
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
      if (i == 0 && ImGui::IsWindowAppearing()) {
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
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
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
    ImGui::PushFont(nullptr, S(24));
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
                           TRF("“%@” already exists. Replace it? (The existing project is moved to the Trash.)",
                               {fs::path(c.confirmReplace).filename().string()})
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
    ImGui::SameLine(0, S(24));
    inlinePrompts(TR("Back"));
    if (!ImGui::GetIO().WantTextInput && !appearing &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
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
    ImGui::PushFont(nullptr, S(24));
    ImGui::TextUnformatted(r.title.c_str());
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputText("##text", r.buf, sizeof r.buf, ImGuiInputTextFlags_EnterReturnsTrue)) ok = true;
    if (ImGui::Button(TR("OK"), ImVec2(S(120), 0))) ok = true;
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(120), 0))) cancel = true;
    ImGui::TextDisabled("%s", TR("Y: type with the on-screen keyboard · Enter / OK: done"));
    inlinePrompts(TR("Cancel"));
    if (!io.WantTextInput && !ImGui::IsWindowAppearing() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
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
