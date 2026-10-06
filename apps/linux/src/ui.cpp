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

namespace fs = std::filesystem;

namespace rnl {

UI::UI(Deps d) : d_(d) {}

bool UI::hasSession() const { return d_.emu->session() != nullptr; }

std::vector<UI::Tab> UI::tabs() const {
  if (!hasSession()) return {Tab::library, Tab::settings, Tab::guide};
  return {Tab::playback, Tab::takes, Tab::bookmarks, Tab::practice, Tab::library, Tab::settings, Tab::guide};
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

void UI::setMenu(bool open) {
  if (!hasSession()) open = true;
  if (open == menuOpen_) return;
  menuOpen_ = open;
  timelinePad_ = false;
  if (open) {
    // Nothing reaches the game while the menu is up; the emulation waits paused.
    d_.emu->setRewindHeld(false);
    d_.emu->setFastForwardHeld(false);
    if (hasSession()) {
      d_.emu->setPaused(true);
      if (tab_ == Tab::library || tab_ == Tab::settings || tab_ == Tab::guide) tab_ = Tab::playback;
    }
    focusFirst_ = true;
  } else {
    d_.input->cancelCapture();
    capturingAction_.clear();
  }
}

void UI::toggleMenu() {
  // A popup has the controls: B / Esc close it (handled by the popup itself).
  if (!dialogs_.empty() || chooser_ || rename_ || !assignElement_.empty() || exportDialog_) return;
  if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return;
  if (timelinePad_) {
    timelinePad_ = false;
    return;
  }
  setMenu(!menuOpen_);
}

void UI::selectTab(Tab t) {
  std::vector<Tab> v = tabs();
  if (std::find(v.begin(), v.end(), t) == v.end()) return;
  if (t != tab_) {
    tab_ = t;
    timelinePad_ = false;
  }
  focusFirst_ = true;
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
  // lane + strip + labels + transport row (+ the A/B tools row in the menu) + padding
  return S(inMenu ? 236.0f : 176.0f);
}

static float headerHeight(float s) { return 108.0f * s; }

GameRect UI::gameRect(int w, int h) const {
  if (!hasSession()) return GameRect{};
  const Settings& st = *d_.settings;
  if (menuOpen_ && tab_ != Tab::playback) return GameRect{};  // a full page covers it
  if (menuOpen_) {
    int top = int(headerHeight(scale_)), bottom = int(dockHeight(true));
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
  }
  if (d_.app->takePracticePanelRequest()) practicePanel_ = true;
  const EmuStatus& st = d_.emu->status();
  if (st.practicing) practicePanel_ = true;

  if (menuOpen_) {
    buildMenu(now);
  } else {
    bool dockVisible = st.paused || now - lastPointer_ < 2.5 || drag_ != Drag::none;
    if (dockVisible) buildDock(false, now);
    if (practicePanel_ || st.practicing) buildPracticeOverlay(false);
  }
  buildOverlays(now);
  buildDialogs();
  buildChooser();
  buildRename();
  buildAssignPicker();
  buildExportDialog();
  buildExportProgressPill();
}

void UI::handleMenuGamepad() {
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
  if (timelinePad_) return;
  std::vector<Tab> v = tabs();
  auto idx = size_t(std::find(v.begin(), v.end(), tab_) - v.begin());
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) selectTab(v[(idx + v.size() - 1) % v.size()]);
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) selectTab(v[(idx + 1) % v.size()]);
  if (hasSession() && !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput &&
      (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)))
    setMenu(false);
}

void UI::buildHeader() {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float hh = headerHeight(scale_);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(io.DisplaySize.x, hh), IM_COL32(20, 22, 27, 245));
  dl->AddLine(ImVec2(0, hh), ImVec2(io.DisplaySize.x, hh), IM_COL32(255, 255, 255, 30));
  ImGui::SetCursorPos(ImVec2(S(16), S(10)));
  // Row 1: title + project actions (right-aligned).
  std::vector<std::pair<std::string, int>> buttons;
  if (hasSession()) {
    buttons = {{std::string("▶  ") + TR("Resume"), 1}, {TR("Save"), 2}, {std::string(TR("Project")) + "  ▾", 3}};
  } else {
    buttons = {{TR("Open Project…"), 4}, {TR("Quit"), 5}};
  }
  float total = 0;
  for (auto& b : buttons) total += ImGui::CalcTextSize(b.first.c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
  std::string title = hasSession() ? d_.app->windowTitle() : std::string("ReplayNES");
  float room = io.DisplaySize.x - total - S(40);
  ImGui::PushFont(nullptr, S(26));
  ImGui::AlignTextToFramePadding();
  ImVec2 tp = ImGui::GetCursorScreenPos();
  ImGui::PushClipRect(tp, ImVec2(tp.x + room, tp.y + S(48)), true);
  ImGui::TextUnformatted(title.c_str());
  ImGui::PopClipRect();
  float titleW = ImGui::GetItemRectSize().x;
  ImGui::PopFont();
  const char* tagline = TR("Made a mistake? Go back and record it again. At the end, export one continuous play video.");
  if (!hasSession() && titleW + ImGui::CalcTextSize(tagline).x + S(16) < room) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tagline);
  }
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(12), io.DisplaySize.x - total - S(8)));
  for (size_t i = 0; i < buttons.size(); ++i) {
    if (i) ImGui::SameLine();
    if (ImGui::Button(buttons[i].first.c_str())) {
      switch (buttons[i].second) {
        case 1:  // back to the game, playing
          setMenu(false);
          if (d_.emu->paused()) d_.emu->togglePause();
          break;
        case 2: d_.app->save(); break;
        case 3: ImGui::OpenPopup("##project"); break;
        case 4: d_.app->openProjectChooser(); break;
        case 5: if (onQuit) onQuit(); break;
      }
    }
  }
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
  // Row 2: tabs (L1 / R1).
  ImGui::SetCursorPos(ImVec2(S(12), S(58)));
  std::vector<Tab> v = tabs();
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) ImGui::SameLine(0, S(4));
    bool sel = v[i] == tab_;
    ImGui::PushStyleColor(ImGuiCol_Button, sel ? ImVec4(0.30f, 0.42f, 0.62f, 1) : ImVec4(0.16f, 0.17f, 0.21f, 1));
    if (ImGui::Button(tabTitle(v[i]))) selectTab(v[i]);
    ImGui::PopStyleColor();
  }
  ImGui::SameLine(0, S(16));
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("L1 / R1");
}

void UI::buildMenu(double now) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  bool playback = tab_ == Tab::playback && hasSession();
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
  if (playback) flags |= ImGuiWindowFlags_NoBackground;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##menu", nullptr, flags);
  ImGui::PopStyleVar();
  handleMenuGamepad();
  buildHeader();
  float hh = headerHeight(scale_);
  if (playback) {
    float dh = dockHeight(true);
    ImGui::SetCursorPos(ImVec2(0, io.DisplaySize.y - dh));
    buildDock(true, now);
    if (practicePanel_ || d_.emu->status().practicing) buildPracticeOverlay(false);
  } else {
    ImGui::SetCursorPos(ImVec2(S(16), hh + S(10)));
    ImGui::BeginChild("##page", ImVec2(io.DisplaySize.x - S(32), io.DisplaySize.y - hh - S(20)),
                      ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    if (focusFirst_ && tab_ != Tab::library) {  // the library focuses its first ROM itself
      ImGui::SetKeyboardFocusHere(0);
      ImGui::SetNavCursorVisible(true);
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
    ImGui::EndChild();
  }
  ImGui::End();
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
    if (hasSession() && (menuOpen_ ? tab_ == Tab::playback : d_.emu->status().paused)) bottom -= dockHeight(menuOpen_);
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
