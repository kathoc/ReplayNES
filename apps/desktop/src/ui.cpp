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
#include "icons.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "library.h"
#include "library_thumbs.h"
#include "pad_nav.h"
#include "paths.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui_theme.h"
#include "ui_widgets.h"

namespace fs = std::filesystem;

namespace rnl {

using namespace theme;

// ------------------------------------------------------------------ theme helpers

namespace theme {
namespace {
int gTruncations = 0;
// Byte length of the longest prefix of `s` (whole UTF-8 characters) not wider than maxW.
size_t fitPrefix(float size, const std::string& s, float maxW) {
  ImFont* f = ImGui::GetFont();
  size_t lo = 0, i = 0;
  while (i < s.size()) {
    size_t n = 1;
    unsigned char c = (unsigned char)s[i];
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    if (f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str(), s.c_str() + i + n).x > maxW) break;
    i += n;
    lo = i;
  }
  return lo;
}
}  // namespace

bool textFit(ImDrawList* dl, float size, ImVec2 pos, float maxW, ImU32 col, const std::string& text) {
  ImFont* f = ImGui::GetFont();
  if (f->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= maxW + 0.5f) {
    dl->AddText(f, size, pos, col, text.c_str());
    return false;
  }
  const char* ell = "…";
  float ew = f->CalcTextSizeA(size, FLT_MAX, 0, ell).x;
  std::string cut = text.substr(0, fitPrefix(size, text, std::max(0.0f, maxW - ew))) + ell;
  dl->AddText(f, size, pos, col, cut.c_str());
  ++gTruncations;
  return true;
}

bool textCentered(ImDrawList* dl, float size, const LRect& r, float y, ImU32 col, const std::string& text) {
  float w = measure(size, text.c_str()).x;
  if (w <= r.w) {
    dl->AddText(ImGui::GetFont(), size, ImVec2(r.x + (r.w - w) / 2, y), col, text.c_str());
    return false;
  }
  return textFit(dl, size, ImVec2(r.x, y), r.w, col, text);
}

int takeTruncations() {
  int n = gTruncations;
  gTruncations = 0;
  return n;
}

float keycap(ImDrawList* dl, ImVec2 p, float h, const std::string& label, bool round, bool draw) {
  ImFont* font = ImGui::GetFont();
  float fs = h * (round ? 0.66f : 0.56f);
  ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
  float w = round ? h : std::max(h * 1.25f, ts.x + h * 0.55f);
  if (draw) {
    if (round) dl->AddCircleFilled(ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.5f, kKeycap);
    else dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kKeycap, h * 0.28f);
    dl->AddText(font, fs, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), kKeycapText, label.c_str());
  }
  return w;
}
}  // namespace theme

// ------------------------------------------------------------------ setup

UI::UI(Deps d) : d_(d) {
  menuFeatures_ = RNF_MENU_FEATURE_QUIT | RNF_MENU_FEATURE_FULLSCREEN | RNF_MENU_FEATURE_UI_SCALE;
  if (RNL_HAVE_MP4_EXPORT) menuFeatures_ |= RNF_MENU_FEATURE_EXPORT;
  if (RNL_HAVE_CRT) menuFeatures_ |= RNF_MENU_FEATURE_CRT;
  if (RNL_HAVE_STEAM) menuFeatures_ |= RNF_MENU_FEATURE_STEAM | RNF_MENU_FEATURE_OSK;
  if (d_.updates) menuFeatures_ |= RNF_MENU_FEATURE_UPDATES;
  menu_ = rnf_menu_new(menuFeatures_);
}

UI::~UI() { rnf_menu_free(menu_); }

bool UI::hasSession() const { return d_.emu->session() != nullptr; }

bool UI::loadFonts(bool japanese) {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  FontFile latin = findLatinFont(), jp = findJapaneseFont(), sym = findSymbolFont();
  // The primary font sets the metrics: in Japanese the CJK font (its Latin glyphs match its kana
  // and kanji in size), in English the Latin UI font. The others fill in missing glyphs (ImGui
  // 1.92 loads glyphs on demand, falling through merged sources): the icon font right after the
  // primary one, the CJK font also in English for names typed in Japanese.
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
    if (!font_) {
      font_ = r;
      icons::addToAtlas(io.Fonts);
    }
    used += (used.empty() ? "" : " + ") + f.path + (f.index ? " #" + std::to_string(f.index) : std::string());
  }
  if (!font_) {
    font_ = io.Fonts->AddFontDefault();
    icons::addToAtlas(io.Fonts);
  }
  std::fprintf(stderr, "fonts: %s + icons\n", used.empty() ? "(ImGui default)" : used.c_str());
  textScale_ = japanese && jp.found() ? 1.18f : 1.0f;  // CJK primary font: smaller glyphs per pixel size
  return !japanese || jp.found();
}

void UI::updateScale(int windowWidth, int windowHeight) {
  metrics_ = UiMetrics::make(float(windowWidth), float(windowHeight), d_.settings->uiScale, textScale_);
  float s = metrics_.s;
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  ImGui::StyleColorsDark(&style);
  // Popups (dialogs, chooser, export) in the same language as the menus: dark panels, radius 16 / 10.
  style.WindowRounding = 16;
  style.ChildRounding = 12;
  style.FrameRounding = 10;
  style.PopupRounding = 12;
  style.GrabRounding = 10;
  style.ScrollbarRounding = 8;
  style.FramePadding = ImVec2(14, 9);
  style.ItemSpacing = ImVec2(10, 10);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.WindowPadding = ImVec2(22, 18);
  style.ScrollbarSize = 16;
  style.GrabMinSize = 18;
  style.TouchExtraPadding = ImVec2(4, 4);
  style.WindowBorderSize = 0;
  style.PopupBorderSize = 0;
  style.FrameBorderSize = 0;
  ImVec4* c = style.Colors;
  auto col = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
  c[ImGuiCol_WindowBg] = col(IM_COL32(20, 20, 22, 245));
  c[ImGuiCol_ChildBg] = col(IM_COL32(255, 255, 255, 8));
  c[ImGuiCol_PopupBg] = col(IM_COL32(26, 26, 30, 250));
  c[ImGuiCol_Text] = col(kText);
  c[ImGuiCol_TextDisabled] = col(kTextDim);
  c[ImGuiCol_FrameBg] = col(IM_COL32(255, 255, 255, 18));
  c[ImGuiCol_FrameBgHovered] = col(IM_COL32(255, 255, 255, 30));
  c[ImGuiCol_FrameBgActive] = col(IM_COL32(255, 255, 255, 40));
  c[ImGuiCol_Button] = col(IM_COL32(255, 255, 255, 22));
  c[ImGuiCol_ButtonHovered] = col(IM_COL32(255, 255, 255, 38));
  c[ImGuiCol_ButtonActive] = col(IM_COL32(255, 59, 59, 120));
  c[ImGuiCol_Header] = col(IM_COL32(255, 255, 255, 26));
  c[ImGuiCol_HeaderHovered] = col(IM_COL32(255, 255, 255, 38));
  c[ImGuiCol_HeaderActive] = col(IM_COL32(255, 59, 59, 110));
  c[ImGuiCol_CheckMark] = col(kBrand);
  c[ImGuiCol_SliderGrab] = col(kBrand);
  c[ImGuiCol_SliderGrabActive] = col(kBrand);
  c[ImGuiCol_Separator] = col(kHairline);
  c[ImGuiCol_PlotHistogram] = col(kBrand);
  c[ImGuiCol_NavCursor] = col(kBrand);
  c[ImGuiCol_TextSelectedBg] = col(IM_COL32(255, 59, 59, 90));
  c[ImGuiCol_ModalWindowDimBg] = col(IM_COL32(0, 0, 0, 150));
  style.ScaleAllSizes(s);
  style.FontSizeBase = metrics_.label();
  style.FontScaleMain = 1.0f;
  scale_ = s;
}

// ------------------------------------------------------------------ state

bool UI::interactive() const {
  return menuOpen_ || !hasSession() || !dialogs_.empty() || chooser_.has_value() || rename_.has_value() ||
         !assignElement_.empty() || exportDialog_;
}

bool UI::wantsKeyboard() const { return ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }

bool UI::popupOpen() const {
  return !dialogs_.empty() || chooser_ || rename_ || !assignElement_.empty() || exportDialog_ ||
         ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

void UI::padUsed() {
  lastInputKeyboardOrMouse_ = false;
  if (ImGui::GetCurrentContext()) ImGui::SetNavCursorVisible(true);
}

void UI::setMenu(bool open) {
  if (open == menuOpen_) return;
  menuOpen_ = open;
  double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0;
  (void)now;
  if (open) {
    // Nothing reaches the game while the menu is up; the emulation waits paused.
    d_.emu->setRewindHeld(false);
    d_.emu->setFastForwardHeld(false);
    if (hasSession()) {
      d_.emu->setPaused(true);
      rnf_menu_open(menu_, "quick");
      thumbSaveWanted_ = true;  // the library card picture (saved in the slack)
    } else {
      rnf_menu_open(menu_, "settings.display");
    }
    menuOpenedAt_ = -1;  // stamped on the next build
    pageChangedAt_ = -1;
    focusKey_.clear();
    menuInputFrame_ = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
  } else {
    rnf_menu_close(menu_);
    d_.input->cancelCapture();
    capturingAction_.clear();
  }
}

void UI::closeMenuAndResume() {
  setMenu(false);
  if (hasSession() && d_.emu->paused()) d_.emu->togglePause();
  lastPaused_ = false;
}

void UI::toggleMenu() {
  // A popup has the controls: B / Esc close it (handled by the popup itself).
  if (!dialogs_.empty() || chooser_ || rename_ || !assignElement_.empty() || exportDialog_) return;
  if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return;
  if (!capturingAction_.empty()) return;  // waiting for a key (Esc cancels it)
  lastActivity_ = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0;
  if (menuOpen_) {
    if (hasSession()) closeMenuAndResume();
    else setMenu(false);
  } else {
    setMenu(true);
  }
}

void UI::shoulder(int dir) {
  if (popupOpen() || (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput)) return;
  if (menuOpen_) {
    menuEvent(rnf_menu_switch(menu_, dir), 0);
  } else if (!hasSession()) {
    libPage_ += dir;  // clamped by the library
    libMoved_ = true;
    libFocus_ = -2;   // the first card of the new page (resolved by the library)
  }
}

void UI::resumeFromSeekBar() {
  if (hasSession() && !menuOpen_ && d_.emu->paused() && !interactive()) d_.emu->togglePause();
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

GameRect UI::gameRect(int w, int h) const {
  if (!hasSession()) return GameRect{};
  const Settings& st = *d_.settings;
  return computeGameRect(w, h, st.integerScale, st.par87, st.hideOverscan);
}

void UI::slack(double now) {
  // Off the input -> screen path: the frame loop calls this in the slack before the next sample.
  if (!hasSession()) return;
  bool playing = !d_.emu->status().paused;
  if (thumbSaveWanted_ || (playing && now - lastThumbSave_ >= 30)) saveLibraryThumb(now);
}

void UI::saveLibraryThumb(double now) {
  thumbSaveWanted_ = false;
  if (!d_.libraryThumbs || !hasSession()) return;
  const EmuStatus& st = d_.emu->status();
  if (st.frame == 0 && st.takeLength == 0) return;  // nothing shown yet
  lastThumbSave_ = now;
  d_.libraryThumbs->save(d_.app->isTempSession() ? std::string() : st.projectPath, st.romPath, d_.emu->picture());
}

void UI::showResetChoices() {
  const EmuStatus& st = d_.emu->status();
  Dialog d;
  d.title = TR("Reset");
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
      case 0: d_.emu->requestEvent(RN_EV_SOFT_RESET); closeMenuAndResume(); break;
      case 1: d_.emu->requestEvent(RN_EV_POWER_CYCLE); closeMenuAndResume(); break;
      case 2: d_.app->resetProjectPrompt(); break;
      default: break;
    }
  };
  showDialog(std::move(d));
}

// ------------------------------------------------------------------ frame

void UI::build(double now) {
  if (noticeTime_ < 0) noticeTime_ = now;
  if (menuOpenedAt_ < 0) menuOpenedAt_ = now;
  if (pageChangedAt_ < 0) pageChangedAt_ = now;
  prompts_.clear();
  description_.clear();
  refreshUpdate();
  layoutBounds_ = LRect{0, 0, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y};
  layoutOverflow_ = false;
  if (!layoutQueue_.empty()) layoutCheckStep();
  // Hold buttons (rewind / fast-forward) set these again while they are held.
  d_.emu->setRewindHeld(false);
  d_.emu->setFastForwardHeld(false);
  if (hasSession() && !hadSession_) {
    // A game was started from the library: to the game.
    menuOpen_ = false;
    rnf_menu_close(menu_);
    lastPaused_ = false;
    lastActivity_ = now;
  } else if (!hasSession() && hadSession_) {
    if (menuOpen_) setMenu(false);
    libFocus_ = -1;
    libMoved_ = false;
  }
  hadSession_ = hasSession();
  const EmuStatus& st = d_.emu->status();
  bool paused = hasSession() && st.paused;
  if (paused != lastPaused_) {
    lastActivity_ = now;
    if (paused) thumbSaveWanted_ = true;
  }
  lastPaused_ = paused;
  if (!d_.settings->menuHintShown && pulseStart_ < 0) pulseStart_ = now;

  if (!hasSession()) buildLibrary(now);
  else if (!menuOpen_ && paused) buildSeekBar(now);
  if (menuOpen_) buildMenu(now);
  buildOverlays(now);
  pollAddToSteam();
  buildDialogs();
  buildChooser();
  buildRename();
  buildAssignPicker();
  buildExportDialog();
  buildExportProgressPill();
  updateTextEntry(now);  // after every text field of the frame (ui_osk.cpp)
  popupLastFrame_ = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  layoutCheckEndFrame();
}

// ------------------------------------------------------------------ overlays

void UI::buildOverlays(double now) {
  bool session = hasSession();
  if (session && !menuOpen_) {
    buildBadges();
    if (d_.emu->status().practicing && !d_.emu->status().paused) buildPracticePill();
  }
  buildNotice(now);
  buildMenuPill(now);
  if (d_.settings->showStats && stats) buildStats();
}

void UI::buildNotice(double now) {
  if (notice_.empty() || now - noticeTime_ >= 4.0) return;
  ImGuiIO& io = ImGui::GetIO();
  const UiMetrics& m = metrics_;
  float fs = m.label(), wrap = io.DisplaySize.x * 0.7f;
  ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, wrap, notice_.c_str());
  float bottom = m.bottomBar().y - S(10);
  if (hasSession() && !menuOpen_ && d_.emu->status().paused) bottom = layoutSeek(m).panel.y - S(12);
  ImVec2 pad(S(20), S(12));
  ImVec2 p0((io.DisplaySize.x - ts.x) * 0.5f - pad.x, bottom - ts.y - pad.y * 2);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float a = float(std::min({1.0, (4.0 - (now - noticeTime_)) / 0.3, (now - noticeTime_) / kAnim}));
  dl->AddRectFilled(p0, ImVec2(p0.x + ts.x + pad.x * 2, p0.y + ts.y + pad.y * 2), alpha(IM_COL32(28, 28, 32, 240), a), m.tileRadius());
  dl->AddText(ImGui::GetFont(), fs, ImVec2(p0.x + pad.x, p0.y + pad.y), alpha(kText, a), notice_.c_str(), nullptr, wrap);
}

void UI::buildMenuPill(double now) {
  // "☰ Menu  L+R": always on screen, a button too (the rescue without a controller).
  ImGuiIO& io = ImGui::GetIO();
  const UiMetrics& m = metrics_;
  rnf_controller_family fam = promptFamily();
  std::string chord = menuChordGlyph(fam, controllerConnected());
  std::string icon = icons::glyph("menu-2");
  std::string label = TR("Menu");
  float fs = m.label(), hs = m.hint();
  float iconW = measure(fs, icon.c_str()).x, labelW = measure(fs, label.c_str()).x;
  float capH = fs * 1.05f;
  // The chord as key caps: "L" + "R" (or "Esc").
  std::vector<std::string> caps;
  for (size_t a = 0, b; a <= chord.size(); a = b + 1) {
    b = chord.find('+', a);
    if (b == std::string::npos) b = chord.size();
    caps.push_back(chord.substr(a, b - a));
  }
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  float capsW = 0;
  for (size_t i = 0; i < caps.size(); ++i) capsW += keycap(fg, ImVec2(), capH, caps[i], false, false) + (i ? hs * 1.1f : 0);
  float padX = S(14);
  float w = padX + iconW + S(8) + labelW + S(14) + capsW + padX;
  GameRect gr = gameRect(int(io.DisplaySize.x), int(io.DisplaySize.y));
  LRect pic = gr.visible ? LRect{gr.x, gr.y, gr.w, gr.h} : LRect{};
  PillGeometry pg = layoutPill(m, w, pic);
  pill_ = pg.rect;
  bool playing = hasSession() && !menuOpen_ && !d_.emu->status().paused;
  if (!playing) lastActivity_ = now;  // paused / in a menu: full contrast, the fade starts on resume
  float a = 1.0f;
  if (playing && pg.overPicture) {
    double idle = now - std::max(lastActivity_, lastPointer_);
    a = idle < 3.0 ? 1.0f : std::max(0.3f, 1.0f - float(idle - 3.0) / 0.4f * 0.7f);
  }
  // On top of everything (the foreground list, the game's render pass); a button for mouse / touch,
  // hit-tested here so no window (the menu's dimming) can cover it.
  ImVec2 mp = io.MousePos;
  bool inside = mp.x >= pill_.x && mp.x < pill_.right() && mp.y >= pill_.y && mp.y < pill_.bottom();
  bool blocked = popupOpen() || io.WantTextInput;
  bool hovered = inside && !blocked;
  bool clicked = hovered && ImGui::IsMouseReleased(0) && io.MouseClickedPos[0].x >= pill_.x && io.MouseClickedPos[0].x < pill_.right() &&
                 io.MouseClickedPos[0].y >= pill_.y && io.MouseClickedPos[0].y < pill_.bottom();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = fg;
  ImU32 bg = pg.overPicture && playing ? IM_COL32(12, 12, 14, 150) : IM_COL32(22, 22, 25, 225);
  if (hovered) bg = IM_COL32(52, 52, 60, 240);
  float r = pill_.h / 2;
  dl->AddRectFilled(tl(pill_), br(pill_), alpha(bg, a), r);
  dl->AddRect(tl(pill_), br(pill_), alpha(IM_COL32(255, 255, 255, 40), a), r, 0, 1.0f);
  float x = pill_.x + padX, cy = pill_.y + pill_.h / 2;
  dl->AddText(ImGui::GetFont(), fs, ImVec2(x, cy - fs / 2), alpha(kText, a), icon.c_str());
  x += iconW + S(8);
  dl->AddText(ImGui::GetFont(), fs, ImVec2(x, cy - fs / 2), alpha(kText, a), label.c_str());
  x += labelW + S(14);
  for (size_t i = 0; i < caps.size(); ++i) {
    if (i) {
      dl->AddText(ImGui::GetFont(), hs, ImVec2(x + hs * 0.25f, cy - hs / 2), alpha(kTextDim, a), "+");
      x += hs * 1.1f;
    }
    float cw = keycap(dl, ImVec2(x, cy - capH / 2), capH, caps[i], false, false);
    dl->AddRectFilled(ImVec2(x, cy - capH / 2), ImVec2(x + cw, cy + capH / 2), alpha(IM_COL32(255, 255, 255, 34), a), capH * 0.28f);
    ImVec2 ts = measure(capH * 0.56f, caps[i].c_str());
    dl->AddText(ImGui::GetFont(), capH * 0.56f, ImVec2(x + (cw - ts.x) / 2, cy - ts.y / 2), alpha(kText, a), caps[i].c_str());
    x += cw;
  }
  // First launch: two pulses, the only onboarding there is.
  if (pulseStart_ >= 0) {
    double t = now - pulseStart_;
    if (t < 2.4) {
      float ph = float(std::fmod(t, 1.2) / 1.2);
      float grow = S(14) * ph;
      dl->AddRect(ImVec2(pill_.x - grow, pill_.y - grow), ImVec2(pill_.right() + grow, pill_.bottom() + grow),
                  alpha(kBrand, 1.0f - ph), r + grow, 0, m.ring() * 1.5f);
    } else {
      pulseStart_ = -1;
      if (!d_.settings->menuHintShown) {
        d_.settings->menuHintShown = true;
        changed();
      }
    }
  }
  if (clicked) {
    lastActivity_ = now;
    toggleMenu();
  }
}

void UI::buildStats() {
  StatsInfo s = stats();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - S(12), pill_.bottom() + S(8)), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGui::Begin("##stats", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PushFont(nullptr, metrics_.hint());
  ImGui::Text("%.2f Hz %s  lead %.2f ms  sample->screen %.2f ms", s.refreshHz, s.cadence, s.leadMs, s.latencyMs);
  ImGui::Text("audio ratio %.4f  fill %.1f ms  underruns %llu  present_wait %s", s.audioRatio, s.audioFillMs,
              (unsigned long long)s.underruns, s.presentWait ? "yes" : "no");
  if (!s.crt.empty()) ImGui::TextUnformatted(s.crt.c_str());
  ImGui::PopFont();
  ImGui::End();
}

// ------------------------------------------------------------------ prompts

std::string UI::triggerAction(bool left) const {
  // Pad 1's trigger hotkey (menus mirror the in-game assignment; defaults: L2 rewind, R2 FF).
  const char* id = left ? "gc0:leftTrigger" : "gc0:rightTrigger";
  for (const rnf_binding& b : d_.input->bindings())
    if (std::strcmp(b.input, id) == 0 && (std::strcmp(b.action, "hk.rewind") == 0 || std::strcmp(b.action, "hk.fast_forward") == 0))
      return b.action;
  return {};
}

bool UI::controllerConnected() const {
  for (int s = 0; s < InputRouter::kSlots; ++s)
    if (d_.input->pad(s).pad) return true;
  return false;
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
  // a cross; others (shoulders, triggers, Menu / View): a key cap with the label. Without a
  // controller: the keyboard's keys.
  if (!controllerConnected()) {
    std::string k = element == "face.south" ? "Enter" : element == "face.east" ? "Bksp" : element == "face.west" ? "Del"
                    : element == "face.north" ? "F2" : element == "leftShoulder" ? "PgUp" : element == "rightShoulder" ? "PgDn"
                    : element == "menu" ? "Esc" : element == "dpad.lr" ? "\xE2\x86\x90 \xE2\x86\x92"
                    : element == "dpad.ud" ? "\xE2\x86\x91 \xE2\x86\x93" : "";
    if (!k.empty()) return keycap(dl, p, h, k, false, draw);
  }
  if (element.rfind("dpad", 0) == 0) {
    float w = h;
    if (draw) {
      float a = h * 0.32f;
      ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
      dl->AddRectFilled(ImVec2(c.x - a * 0.5f, p.y + 1), ImVec2(c.x + a * 0.5f, p.y + h - 1), kKeycap, a * 0.25f);
      dl->AddRectFilled(ImVec2(p.x + 1, c.y - a * 0.5f), ImVec2(p.x + w - 1, c.y + a * 0.5f), kKeycap, a * 0.25f);
      if (element == "dpad.lr") {
        dl->AddRectFilled(ImVec2(p.x + 1, c.y - a * 0.5f), ImVec2(p.x + a, c.y + a * 0.5f), kBrand, a * 0.25f);
        dl->AddRectFilled(ImVec2(p.x + w - a, c.y - a * 0.5f), ImVec2(p.x + w - 1, c.y + a * 0.5f), kBrand, a * 0.25f);
      } else if (element == "dpad.ud") {
        dl->AddRectFilled(ImVec2(c.x - a * 0.5f, p.y + 1), ImVec2(c.x + a * 0.5f, p.y + a), kBrand, a * 0.25f);
        dl->AddRectFilled(ImVec2(c.x - a * 0.5f, p.y + h - a), ImVec2(c.x + a * 0.5f, p.y + h - 1), kBrand, a * 0.25f);
      }
    }
    return w;
  }
  std::string label = padGlyph(f, element.c_str());
  if (label.empty()) label = element;
  return keycap(dl, p, h, label, element.rfind("face.", 0) == 0, draw);
}

void UI::buildHintBar(const LRect& bar) {
  // Bottom right: glyphs of the controller in use + at most a few verbs; the focused item's one
  // line on the left.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const UiMetrics& m = metrics_;
  rnf_controller_family f = promptFamily();
  float gh = m.hint() * 1.55f, fs = m.hint();
  std::vector<float> widths;
  float total = 0;
  for (const Prompt& p : prompts_) {
    float w = 0;
    for (const std::string& e : p.elements) w += glyph(dl, ImVec2(), e, f, gh, false) + S(4);
    w += S(4) + measure(fs, p.text.c_str()).x + S(20);
    widths.push_back(w);
    total += w;
  }
  float x = bar.right() - total + S(20);
  float cy = bar.y + bar.h / 2;
  for (size_t i = 0; i < prompts_.size(); ++i) {
    const Prompt& p = prompts_[i];
    for (const std::string& e : p.elements) x += glyph(dl, ImVec2(x, cy - gh / 2), e, f, gh) + S(4);
    x += S(4);
    ImVec2 ts = measure(fs, p.text.c_str());
    dl->AddText(ImGui::GetFont(), fs, ImVec2(x, cy - ts.y / 2), kTextDim, p.text.c_str());
    x += ts.x + S(20);
  }
  if (!description_.empty()) {
    float maxW = std::max(0.0f, bar.w - total - S(24));
    textFit(dl, m.label() * 0.92f, ImVec2(bar.x, cy - m.label() * 0.46f), maxW, kTextDim, description_);
  }
}

void UI::inlinePrompts(const char* backText) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  rnf_controller_family f = promptFamily();
  float gh = metrics_.hint() * 1.5f, fs = metrics_.hint(), fh = ImGui::GetFrameHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float x0 = p.x;
  p.y += (fh - gh) * 0.5f;
  const std::pair<const char*, const char*> items[] = {{"face.south", TR("Select")}, {"face.east", backText}};
  for (const auto& [el, text] : items) {
    p.x += glyph(dl, p, el, f, gh) + S(6);
    ImVec2 ts = measure(fs, text);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x, p.y + (gh - ts.y) * 0.5f), kTextDim, text);
    p.x += ts.x + S(20);
  }
  ImGui::Dummy(ImVec2(p.x - x0, fh));
}

// ------------------------------------------------------------------ layout check

void UI::startLayoutCheck(std::vector<std::pair<int, int>> extraSizes) {
  layoutQueue_.clear();
  layoutFailures_ = layoutChecked_ = 0;
  realW_ = int(metrics_.W);
  realH_ = int(metrics_.H);
  std::vector<std::pair<int, int>> sizes = {{0, 0}};
  sizes.insert(sizes.end(), extraSizes.begin(), extraSizes.end());
  for (auto [w, h] : sizes) {
    layoutQueue_.push_back({"library", w, h, 0});
    if (hasSession()) {
      layoutQueue_.push_back({"seek", w, h, 0});
      for (size_t p = 0; p < rnf_menu_page_count(menu_); ++p) {
        rnf_menu_page_info pi{};
        rnf_menu_page_get(menu_, p, &pi);
        if (std::strcmp(pi.id, "library.projects") == 0) continue;
        layoutQueue_.push_back({pi.id, w, h, 0});
      }
    }
  }
}

void UI::layoutRecord(const LRect& r) {
  if (!layoutOverride_ && layoutQueue_.empty()) return;
  if (!layoutBounds_.contains(r, 1.0f)) layoutOverflow_ = true;
}

std::string UI::scrollingWindows() const {
  // Any ImGui window of this frame that would scroll (popups, the export dialog ...).
  std::string out;
  for (ImGuiWindow* w : GImGui->Windows)
    if (w->Active && !w->Hidden && (w->ScrollMax.y > 0.5f || w->ScrollMax.x > 0.5f)) out += std::string(out.empty() ? "" : ", ") + w->Name;
  return out;
}

void UI::layoutCheckEndFrame() {
  if (layoutQueue_.empty()) return;
  layoutPrevOverflow_ = layoutOverflow_;
  layoutPrevScroll_ = scrollingWindows();
  layoutPrevTrunc_ = takeTruncations();
}

void UI::layoutCheckStep() {
  ImGuiIO& io = ImGui::GetIO();
  if (layoutQueue_.front().frames >= 3) {
    // The previous frame showed the step settled (setup, one frame to settle, one checked).
    CheckStep c = layoutQueue_.front();
    layoutQueue_.pop_front();
    bool ok = !layoutPrevOverflow_ && layoutPrevScroll_.empty();
    std::string size = c.w > 0 ? std::to_string(c.w) + "x" + std::to_string(c.h) : std::to_string(realW_) + "x" + std::to_string(realH_);
    std::fprintf(stderr, "layoutcheck: %-22s %-9s %s%s%s%s\n", c.what.c_str(), size.c_str(), ok ? "ok" : "FAIL",
                 layoutPrevOverflow_ ? " (content outside its area)" : "",
                 layoutPrevScroll_.empty() ? "" : (" (scrolls: " + layoutPrevScroll_ + ")").c_str(),
                 layoutPrevTrunc_ ? (" [" + std::to_string(layoutPrevTrunc_) + " text(s) shortened]").c_str() : "");
    ++layoutChecked_;
    if (!ok) ++layoutFailures_;
    if (layoutQueue_.empty()) {
      std::fprintf(stderr, "layoutcheck: done, %d screen(s), %d failure(s)\n", layoutChecked_, layoutFailures_);
      layoutOverride_ = false;
      updateScale(realW_, realH_);
      if (menuOpen_) setMenu(false);
      return;
    }
  }
  CheckStep& c = layoutQueue_.front();
  if (c.w > 0) {
    // A layout override: this frame is laid out for c.w x c.h (drawn clipped to the window).
    io.DisplaySize = ImVec2(float(c.w), float(c.h));
    updateScale(c.w, c.h);
    layoutOverride_ = true;
  } else if (layoutOverride_) {
    layoutOverride_ = false;
    updateScale(realW_, realH_);
  }
  layoutBounds_ = LRect{0, 0, io.DisplaySize.x, io.DisplaySize.y};
  if (c.frames == 0) {
    // Set the screen up.
    if (c.what == "library") {
      if (menuOpen_) setMenu(false);
    } else if (c.what == "seek") {
      if (menuOpen_) setMenu(false);
      d_.emu->setPaused(true);
    } else {
      openPage(c.what);
    }
    menuOpenedAt_ = pageChangedAt_ = -100;  // no animation
  }
  ++c.frames;
}

}  // namespace rnl
