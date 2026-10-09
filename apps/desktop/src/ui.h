// Dear ImGui UI of the desktop frontend (Linux / Steam Deck, Windows) after
// docs/design/UI_REDESIGN.md: minimal text, no scrolling, fully controller / mouse / touch driven.
//
//  * Library (no game open): a "Continue" hero card and pages of large game cards (ui_library.cpp).
//  * Playing: only the always-visible Menu pill ("☰ Menu L+R", top right; a button too) and short
//    notices / status badges, drawn in the game's own render pass.
//  * Paused (R / Space): the seek bar only (filmstrip + time; A / B resume, L2 / R2 rewind /
//    fast-forward, D-pad steps) - ui_play.cpp.
//  * Quick Menu (L+R / Esc / the pill): six tiles over the dimmed game, their pages, Settings
//    (four pages, L / R) and detail pages - the shared core's menu model (rnf_menu) drawn by
//    ui_menu.cpp; values in ui_settings.cpp. Emulation is paused while it is open.
// Dialogs / file chooser / rename / export / key assignment are modal popups (DialogHost, ImGui
// navigation). A text field that becomes active gets an on-screen keyboard (ui_osk.cpp).
// Frame-loop thread only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dialogs.h"
#include "imgui.h"
#include "mp4_export.h"
#include "osk.h"
#include "renderer.h"
#include "replaynes/frontend.h"
#include "ui_features.h"
#include "ui_layout.h"
#include "ui_logic.h"
#include "update_model.h"
#if RNL_HAVE_STEAM
#include "steam_shortcut.h"
#endif

namespace rnl {

class AppModel;
class EmulationController;
class InputRouter;
class LibraryModel;
class LibraryThumbs;
class ThumbnailManager;
class ThumbRef;
class UpdateService;
struct Settings;
struct EmuStatus;
struct LibraryROM;

struct StatsInfo {
  double refreshHz = 0;
  const char* cadence = "?";
  double leadMs = 0, latencyMs = 0, audioRatio = 1, audioFillMs = 0;
  uint64_t underruns = 0;
  bool presentWait = false;
  std::string crt;  // CRT status line ("" = off)
};

class UI : public DialogHost {
 public:
  struct Deps {
    AppModel* app;
    EmulationController* emu;
    LibraryModel* library;
    InputRouter* input;
    ThumbnailManager* thumbs;
    Renderer* renderer;
    Settings* settings;
    SDL_Window* window;
    UpdateService* updates = nullptr;    // in-app updates (Linux: Flatpak portal; Windows: WinSparkle); null: none
    LibraryThumbs* libraryThumbs = nullptr;
  };
  explicit UI(Deps d);
  ~UI() override;
  void setApp(AppModel* app) { d_.app = app; }

  // Hooks from main.
  std::function<void()> onQuit;              // the user chose Quit
  std::function<void()> onRestart;           // restart into an installed update (quits first)
  std::function<void()> onSettingsChanged;   // save + apply (volume, flash level ...)
  std::function<void(bool)> onFullscreen;
  std::function<bool()> isFullscreen;
  std::function<StatsInfo()> stats;
  /// Settings -> System -> Language changed ("auto" / "ja" / "en"): main applies it between frames.
  std::function<void(const std::string&)> onLanguage;

  /// Fonts (Latin + Japanese when the language is ja and a CJK font exists) with the icon font.
  /// Returns false if Japanese was asked for but no CJK font was found (the caller switches to English).
  bool loadFonts(bool japanese);
  /// Applies the UI scale (window size, Settings.uiScale). Call when either changes.
  void updateScale(int windowWidth, int windowHeight);

  /// Builds this frame's UI (between ImGui::NewFrame and ImGui::Render).
  void build(double now);
  /// Work that may wait (library card pictures): called in the frame loop's slack, never between
  /// the input sample and the present.
  void slack(double now);
  /// Where the game picture goes in a w x h drawable (menus draw over the dimmed game).
  GameRect gameRect(int w, int h) const;

  /// Whether ImGui / the UI have the controller and keyboard (menus, library, seek bar, dialogs).
  bool interactive() const;
  /// Settings > Controls > Confirm Button: the south button confirms (default: east).
  bool southConfirm() const;
  /// Text input active (keys go to the text field, not the game).
  bool wantsKeyboard() const;
  /// hk.menu (L+R, Esc, the pill): the Quick Menu on / off (the library: Settings on / off).
  void toggleMenu();
  void setMenu(bool open);
  bool menuOpen() const { return menuOpen_; }
  /// Id of the menu page shown ("" when the menu is closed); script / test hooks.
  std::string shownMenuPage() const { return menuOpen_ ? menuPageId() : std::string(); }
  /// L / R in the UI (InputRouter::onUiShoulder): previous / next page.
  void shoulder(int dir);
  /// The cancel button tapped on the seek bar (InputRouter::onPausedResume): resume.
  void resumeFromSeekBar();
  /// The paused seek bar's markers (InputRouter::onSeekInput / onMarkerHold / onPausedShoulder).
  void seekInput(int input, bool down);  // InputRouter::SeekInput
  void markerHold(int dir, bool down);
  void pausedShoulder(int dir);
  /// The seek bar's focus for the InputRouter: 0 the bar, 1 a marker, 2 editing.
  int seekFocus() const;
  void pointerMoved(double now) { lastPointer_ = now; }
  /// "Export…": the MP4 export dialog; startExport() = its Export button (scripts).
  void openExportDialog();
  void startExport();
  /// The CRT display parameters of the settings (sanitized).
  CrtSettings crtSettings() const;
  /// A controller button / stick was used (prompts follow the controller).
  void padUsed();
  /// A key / mouse button (not touch) was used: a text field activated by them gets no on-screen
  /// keyboard in Auto outside Gaming Mode.
  void keyboardOrMouseUsed() { lastInputKeyboardOrMouse_ = true; }
  void touchUsed() { lastInputKeyboardOrMouse_ = false; }

  // Scripts (--script) and checks.
  /// Opens the menu on a page of the menu model ("quick", "practice", "settings.system", ...) along
  /// its natural path. False for an unknown page.
  bool openPage(const std::string& pageId);
  /// The Menu pill's rectangle this frame (scripts click it).
  LRect pillRect() const { return pill_; }
  /// Hit rectangle of breadcrumb segment i (ancestors only; w == 0 when there is none).
  LRect crumbRect(size_t i) const { return i < crumbRects_.size() ? crumbRects_[i] : LRect{}; }
  /// Walks every page of the menu model + the library + the seek bar, one per frame, at the window
  /// size and at extra sizes (e.g. 1920x1080 as a layout override), and checks that nothing
  /// overflows (no screen scrolls). Logs "layoutcheck:" lines; done() + failures() when finished.
  void startLayoutCheck(std::vector<std::pair<int, int>> extraSizes);
  bool layoutCheckRunning() const { return !layoutQueue_.empty(); }
  int layoutFailures() const { return layoutFailures_; }

  // On-screen keyboard (ui_osk.cpp).
  bool textEntryOwnsPad() const { return textEntry_ == TextEntry::builtin || textEntry_ == TextEntry::steam; }
  bool textEntryEvent(const SDL_Event& e, double now);
  std::vector<std::string> oskPresses(const std::string& text) const;
  const char* textEntryName() const;

  // DialogHost
  void showDialog(Dialog d) override;
  void showChooser(ChooserRequest r) override;
  void notice(const std::string& text) override;

  /// Script hooks (--script, tests): the visible dialog's button / the chooser's name.
  bool answerDialog(int button);
  bool dialogVisible() const { return !dialogs_.empty(); }
  bool scriptUpdate(const std::string& action);
  /// "setting <key> <value>" (scripts): diagramFamily auto|0..4, southConfirm 0|1, timelineSlot 0..7.
  bool scriptSetting(const std::string& key, const std::string& value);
  std::string updatePhaseName() const;

 private:
  // ---- ui.cpp: frame, overlays, dialogs
  void buildOverlays(double now);
  void buildDialogs();
  void buildChooser();
  void buildRename();
  void buildStats();
  void buildMenuPill(double now);
  void buildNotice(double now);
  void showResetChoices();
  void closeMenuAndResume();
  bool popupOpen() const;
  void askRename(const std::string& title, const std::string& initial, std::function<void(const std::string&)> apply);
  float S(float v) const { return v * scale_; }
  bool hasSession() const;
  void changed();
  void saveLibraryThumb(double now);
  // Prompts (the hint bar): glyphs of controller elements ("face.south", "dpad.lr", ...).
  struct Prompt {
    std::vector<std::string> elements;
    std::string text;
  };
  void prompt(std::initializer_list<const char*> elements, const std::string& text);
  void buildHintBar(const LRect& bar);
  void inlinePrompts(const char* backText);
  rnf_controller_family promptFamily() const;
  bool controllerConnected() const;
  std::string triggerAction(bool left) const;  // "hk.rewind" / "hk.fast_forward" / ""
  float glyph(ImDrawList* dl, ImVec2 p, const std::string& element, rnf_controller_family f, float h, bool draw = true) const;
  /// "ui.confirm" / "ui.cancel" -> the face button of the setting; other elements unchanged.
  std::string uiElement(const std::string& element) const;
  /// ImGui's gamepad keys of the UI confirm / cancel buttons (east / south by default).
  ImGuiKey confirmKey() const;
  ImGuiKey cancelKey() const;

  // ---- ui_menu.cpp: the Quick Menu and its pages (rnf_menu)
  void buildMenu(double now);
  void handleMenuInput(double now);
  void menuEvent(rnf_menu_event e, int dir);
  void activateItem(const std::string& id);
  void adjustItem(const std::string& id, int dir);
  void activateRow(const std::string& page, size_t row);
  void rowAction(const std::string& page, size_t row, char key);  // 'x' / 'y'
  void buildTilesPage(const PageGeometry& g, size_t page, double now);
  void buildSettingsPage(const PageGeometry& g, size_t page, double now);
  void buildCardsPage(const PageGeometry& g, double now);
  void buildListPage(const PageGeometry& g, size_t page, double now);
  void buildControllerPage(const PageGeometry& g, double now);
  void buildBreadcrumb(const LRect& bar);
  void buildPageHeader(const PageGeometry& g, size_t page);
  size_t listCount(const std::string& page) const;
  std::string menuPageId() const;
  std::string focusedItemId() const;
  /// Focus ring animated from the previous focus rectangle (120 ms).
  void drawFocus(const LRect& r, float radius, double now);
  bool itemHit(const char* id, const LRect& r, size_t index);  // hover focuses, click activates
  void syncMenuCounts();

  // ---- ui_settings.cpp: values of the Settings rows, the controller diagram, key assignment
  std::string itemValue(const std::string& id) const;
  bool itemOn(const std::string& id) const;            // TOGGLE state
  float itemFraction(const std::string& id) const;     // SLIDER position 0..1 (-1: none)
  bool itemEnabled(const std::string& id) const;
  void buildDiagram(rnf_controller_family family, int slot, const LRect& area, double now);
  void openAssign(const std::string& physicalId);  // the action picker page of a controller button
  void startAddToSteam();
  void pollAddToSteam();
  rnf_controller_family diagramFamily(int slot) const;

  // ---- ui_library.cpp
  void buildLibrary(double now);
  void handleLibraryInput(double now, size_t cardCount, size_t pages, bool hero);
  void buildLibraryBar(ImDrawList* dl, const LRect& bar, double now);
  void libraryBarAction(int item);
  void buildSearchField(const LRect& r);  // a filter chip, the sort (cycles) or the search
  std::vector<const LibraryROM*> libraryRoms() const;
  const LibraryROM* projectsRom() const;

  // ---- ui_play.cpp: seek bar, timeline, practice pill, badges
  void buildSeekBar(double now);
  void buildTimeline(const LRect& strip, const LRect& lane, double now);
  bool markersActive() const;  // paused on the seek bar, not practicing, no menu / popup
  void updateMarkers(double now);  // sync with the selected slot, D-pad repeat, L / R hold
  void applyMarkers(const rnf_markers_result& r);
  void buildPracticePill();
  void buildBadges();
  bool thumbImage(ThumbRef& img, ImTextureID* tex, ImVec2* uv0, ImVec2* uv1);

  // ---- ui_osk.cpp
  void updateTextEntry(double now);
  void buildOsk();
  void buildSteamKeyboardHint();
  void applyOsk(const OskAction& a);
  void requestSteamKeyboard(bool show);
  void sendKey(ImGuiKey k);
  bool textFieldEmpty() const;
  // ---- ui_export.cpp
  void buildExportDialog();
  void buildExportProgressPill();
  // ---- ui_update.cpp
  void refreshUpdate();
  void buildUpdateNotice(const LRect& r);
  std::string updateStatusText() const;
  std::string updateErrorText() const;

  // ---- layout check
  void layoutCheckStep();
  void layoutCheckEndFrame();
  std::string scrollingWindows() const;
  void layoutRecord(const LRect& r);  // content drawn this frame (must stay inside the screen / content)

  Deps d_;
  float scale_ = 1.0f;
  float textScale_ = 1.0f;  // UiMetrics::text of the fonts loaded
  UiMetrics metrics_;
  ImFont* font_ = nullptr;
  bool menuOpen_ = false;
  rnf_menu* menu_ = nullptr;
  uint32_t menuFeatures_ = 0;
  bool hadSession_ = false;
  bool lastPaused_ = false;
  // Animations.
  double menuOpenedAt_ = -10, pageChangedAt_ = -10, focusChangedAt_ = -10;
  LRect focusFrom_, focusTo_;
  std::string focusKey_;  // page + item of the last focus (animates when it changes)
  int menuInputFrame_ = -1;  // the frame the menu opened (its press is not a confirm)
  // Prompts / description.
  std::vector<Prompt> prompts_;
  std::string description_;
  // Pill.
  LRect pill_;
  std::vector<LRect> crumbRects_;  // the breadcrumb's clickable segments (last frame)
  double lastPointer_ = -10, lastActivity_ = 0, pulseStart_ = -1;
  // Notices.
  std::string notice_;
  double noticeTime_ = 0;
  // Library.
  int libFocus_ = -1;  // -1: the Continue card, -3: the filter / sort / search row, else a game card index (all pages)
  int libBar_ = 0;     // focused item of that row
  int libPage_ = 0;
  bool libMoved_ = false;  // the user moved the focus since the library showed
  std::string projectsRomPath_;  // the projects page's ROM
  char search_[128] = {0};
  bool focusSearch_ = false;
  bool searchShown_ = false;
  double lastThumbSave_ = 0;
  bool thumbSaveWanted_ = false;
  // Settings.
  int diagramFocus_ = 0;
  std::vector<std::string> diagramIds_;  // this frame's diagram elements (physical ids) ...
  std::vector<ImVec2> diagramCenters_;   // ... and where they are (D-pad navigation)
  std::string assignElement_;  // physical id whose action is picked on "controls.assign"
  std::string capturingAction_;
#if RNL_HAVE_STEAM
  std::future<steam::Report> steamJob_;
#endif
  // In-app updates.
  UpdateModel update_;
  uint64_t updateSerial_ = 0;
  // Dialogs.
  std::deque<Dialog> dialogs_;
  bool dialogOpened_ = false;
  int dialogAnswer_ = -1;
  struct Chooser {
    ChooserRequest req;
    std::string dir;
    std::vector<std::pair<std::string, bool>> entries;  // name, isDirectory
    char name[256] = {0};
    bool opened = false;
    std::string confirmReplace;
    bool focusName = false;
  };
  std::optional<Chooser> chooser_;
  void chooserList();
  struct Rename {
    std::string title;
    char buf[256] = {0};
    std::function<void(const std::string&)> apply;
    bool opened = false;
  };
  std::optional<Rename> rename_;
  bool popupLastFrame_ = false;
  // Seek bar markers (rnf_markers: the selected slot's A / B from the controller).
  rnf_markers* markers_ = nullptr;
  rnf_step_repeater* markerRepeat_ = nullptr;  // D-pad left / right while editing
  int markerHoldDir_ = 0, markerHoldTicks_ = 0;  // L / R held while editing (rnf_hold_speed)
  double markerHintUntil_ = 0, markerNow_ = 0;
  std::string markerHint_;
  // Timeline (seek bar).
  enum class Drag { none, scrub, select, handle, body, ignore };
  Drag drag_ = Drag::none;
  int dragSlot_ = 0;
  int dragHandle_ = 0;
  uint64_t dragA_ = 0, dragB_ = 0;
  float dragStartX_ = 0;
  std::optional<uint64_t> scrubFrame_;
  double scrubHoldUntil_ = 0;
  bool hasPreview_ = false;
  rnf_timeline_range preview_{};
  double previewUntil_ = 0;
  // On-screen keyboard (ui_osk.cpp).
  OskModel osk_;
  OskRepeater oskRepeat_;
  TextEntry textEntry_ = TextEntry::none;
  ImGuiID textEntryId_ = 0;
  int steamClosingFrames_ = 0;
  int sdlOskHint_ = -1;
  bool lastInputKeyboardOrMouse_ = false;
  float oskX0_ = 0, oskY0_ = 0, oskX1_ = 0, oskY1_ = 0;
  struct OskKeyRect {
    int row, col;
    float x0, y0, x1, y1;
  };
  std::vector<OskKeyRect> oskKeys_;
  bool oskTapDown_ = false;
  bool oskStick_[4] = {false, false, false, false};
  // MP4 export.
  ExportJob exportJob_;
  bool exportDialog_ = false;
  bool exportOpened_ = false;
  bool exportStarted_ = false;
  bool exportCropOverscan_ = true;
  bool exportPar87_ = false;
  bool exportApplyFlash_ = true;
  bool exportApplyCRT_ = false;
  bool exportWholeTake_ = true;
  int exportStart_ = 0, exportEnd_ = 0;
  int exportPreset_ = -1;
  int exportEncoder_ = 0;
  char exportName_[200] = {0};
  std::string exportError_;
  // Layout check.
  struct CheckStep {
    std::string what;  // page id, "library", "seek"
    int w = 0, h = 0;  // layout override (0: the window)
    int frames = 0;
  };
  std::deque<CheckStep> layoutQueue_;
  int layoutFailures_ = 0, layoutChecked_ = 0;
  bool layoutOverride_ = false;
  LRect layoutBounds_;      // where content may go this frame
  bool layoutOverflow_ = false;
  bool layoutPrevOverflow_ = false;
  std::string layoutPrevScroll_;
  int layoutPrevTrunc_ = 0;
  int realW_ = 0, realH_ = 0;
};

}  // namespace rnl
