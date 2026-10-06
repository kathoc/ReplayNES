// Dear ImGui UI of the Linux frontend, fully gamepad navigable (Gaming Mode has no keyboard) and
// mouse / touch friendly at 1280x800 (scaled with the window and Settings -> UI scale).
//
//  * No session: the menu is always open on the Library (start screen; Settings and the Guide
//    from its header, Menu (≡) / View (⧉)).
//  * Playing: nothing is drawn over the game except short notices, status badges and the
//    practice pill (all in the same Vulkan pass as the picture). A click / tap shows the dock
//    (transport + filmstrip timeline) for a moment.
//  * Paused (R / Space, the end of a rewind, ...) or R3 / Guide / Esc: the hub - the game with the
//    timeline, the transport and a row of large buttons below it (Resume, Back to Library,
//    Settings, Practice, Takes, Bookmarks, Export…, Reset…, Guide), all controller navigable.
//    Its buttons open pages (Settings, Practice, Takes, Bookmarks, Guide); B goes back to the hub,
//    B / R / Menu (≡) on the hub resume play. Emulation is paused while the menu is open and
//    nothing reaches the game. Controller conventions: ui_logic.h; button prompts for the
//    controller in use are shown at the bottom of every screen.
// Dialogs / file chooser / rename are modal popups (DialogHost); text fields start SDL text
// input, which shows Steam's on-screen keyboard in Gaming Mode.
// Frame-loop thread only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "dialogs.h"
#include "mp4_export.h"
#include "imgui.h"
#include "replaynes/frontend.h"
#include "steam_shortcut.h"
#include "ui_logic.h"
#include "vk_renderer.h"

namespace rnl {

class AppModel;
class EmulationController;
class InputRouter;
class LibraryModel;
class ThumbnailManager;
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
  using Tab = MenuPage;  // playback = the hub

  struct Deps {
    AppModel* app;
    EmulationController* emu;
    LibraryModel* library;
    InputRouter* input;
    ThumbnailManager* thumbs;
    VkRenderer* renderer;
    Settings* settings;
    SDL_Window* window;
  };
  explicit UI(Deps d);
  void setApp(AppModel* app) { d_.app = app; }

  // Hooks from main.
  std::function<void()> onQuit;              // the user chose Quit
  std::function<void()> onSettingsChanged;   // save + apply (volume, flash level ...)
  std::function<void(bool)> onFullscreen;
  std::function<bool()> isFullscreen;
  std::function<StatsInfo()> stats;

  /// Fonts (Latin + Japanese when the language is ja and a CJK font exists). Returns false if
  /// Japanese was asked for but no CJK font was found (the caller switches to English).
  bool loadFonts(bool japanese);
  /// Applies the UI scale (window height / 800 * Settings.uiScale). Call when either changes.
  void updateScale(int windowHeight);

  /// Builds this frame's UI (between ImGui::NewFrame and ImGui::Render).
  void build(double now);
  /// Where the game picture goes in a w x h drawable (leaves room for the menu's dock).
  GameRect gameRect(int w, int h) const;

  /// Whether ImGui should navigate with the gamepad / keyboard (menu, library, dialogs).
  bool interactive() const;
  /// Text input active (keys go to the text field, not the game).
  bool wantsKeyboard() const;
  void toggleMenu();
  void setMenu(bool open);
  bool menuOpen() const { return menuOpen_; }
  void selectTab(Tab t);
  Tab tab() const { return tab_; }
  void pointerMoved(double now) { lastPointer_ = now; }
  /// The practice panel (A/B slots) over the game.
  void showPracticePanel(bool on) { practicePanel_ = on; }
  void setSettingsPage(int p) { settingsPage_ = p; }
  /// "Export…": the MP4 export dialog; startExport() = its Export button (scripts).
  void openExportDialog();
  void startExport();
  /// The CRT display parameters of the settings (sanitized).
  CrtSettings crtSettings() const;
  /// A controller button / stick was used (the focus ring shows).
  void padUsed();

  // DialogHost
  void showDialog(Dialog d) override;
  void showChooser(ChooserRequest r) override;
  void notice(const std::string& text) override;

  /// Script hooks (--script, tests): the visible dialog's button / the chooser's name.
  bool answerDialog(int button);
  bool dialogVisible() const { return !dialogs_.empty(); }

 private:
  // ui.cpp
  void buildMenu(double now);
  void buildHeader();
  void buildOverlays(double now);
  void buildDialogs();
  void buildChooser();
  void buildRename();
  void buildStats();
  void handleMenuGamepad();
  void applyTransition(const MenuTransition& t);
  void openHub();
  void closeHubAndResume();
  void buildHubButtons();
  void showResetChoices();
  void buildPageTabs();
  void scrollWithRightStick();
  void drawFocusRing();
  // Button prompts (bottom bar): glyphs of controller elements ("face.south", "dpad.lr", ...).
  struct Prompt {
    std::vector<std::string> elements;
    std::string text;
  };
  void prompt(std::initializer_list<const char*> elements, const std::string& text);
  void buildPromptBar();
  /// A / B prompts on one line inside a dialog (its cursor position).
  void inlinePrompts(const char* backText);
  float promptBarHeight() const { return S(40); }
  rnf_controller_family promptFamily() const;
  std::string triggerAction(bool left) const;  // "hk.rewind" / "hk.fast_forward" / ""
  float glyph(ImDrawList* dl, ImVec2 p, const std::string& element, rnf_controller_family f, float h, bool draw = true) const;
  void askRename(const std::string& title, const std::string& initial, std::function<void(const std::string&)> apply);
  float S(float v) const { return v * scale_; }
  bool hasSession() const;
  std::vector<Tab> tabs() const;
  // ui_export.cpp
  void buildExportDialog();
  void buildExportProgressPill();
  // ui_play.cpp
  void buildDock(bool inMenu, double now);
  float hubPanelHeight() const;
  void buildTransport(bool inMenu);
  void buildTimeline(float width, bool inMenu, double now, float navLeft = 0, float navRight = 0);
  void buildPracticeOverlay(bool interactiveNav);
  void buildPracticeRows(bool compactButtons);
  void buildBadges();
  float dockHeight(bool inMenu) const;
  // ui_pages.cpp
  void buildTakes();
  void buildBookmarks();
  void buildPracticeTab();
  void buildLibrary(double now);
  void buildGuide();
  // ui_settings.cpp
  void buildSettings();
  void buildDisplaySettings();
  void buildAudioControlSettings();
  void buildControllerSettings();
  void buildInputSettings();
  void buildDiagram(rnf_controller_family family, int slot, float width);
  void buildAssignPicker();
  void buildCrtSettings();
  void startAddToSteam();
  void pollAddToSteam();
  void changed();

  Deps d_;
  float scale_ = 1.0f;
  int windowHeight_ = 800;
  ImFont* font_ = nullptr;
  bool menuOpen_ = false;
  Tab tab_ = Tab::library;
  Tab previousTab_ = Tab::playback;  // the page the guide was opened from (View toggles back)
  Tab hubFocus_ = Tab::playback;     // hub button focused when the hub shows again
  bool focusFirst_ = false;  // gamepad focus on the first item of the page (next frame)
  bool lastPaused_ = false;  // paused in the previous frame (a pause opens the hub)
  bool hadSession_ = false;
  int hubFocusFrames_ = 0;
  int menuOpenedFrame_ = -1;
  bool popupLastFrame_ = false;   // a popup / active item had B last frame (it closed itself)
  bool activeLastFrame_ = false;
  bool timelineFocused_ = false;  // the hub's timeline had the focus last frame
  std::vector<Prompt> prompts_;
  double lastPointer_ = -10;
  bool practicePanel_ = false;
  // Notices.
  std::string notice_;
  double noticeTime_ = 0;
  // Settings -> Audio & Controls -> System -> Add to Steam (file I/O on a worker thread).
  std::future<steam::Report> steamJob_;
  // Dialogs.
  std::deque<Dialog> dialogs_;
  bool dialogOpened_ = false;
  int dialogAnswer_ = -1;
  // File chooser.
  struct Chooser {
    ChooserRequest req;
    std::string dir;
    std::vector<std::pair<std::string, bool>> entries;  // name, isDirectory
    char name[256] = {0};
    bool opened = false;
    std::string confirmReplace;  // path waiting for "Replace?"
    bool focusName = false;
  };
  std::optional<Chooser> chooser_;
  void chooserList();
  // Rename.
  struct Rename {
    std::string title;
    char buf[256] = {0};
    std::function<void(const std::string&)> apply;
    bool opened = false;
  };
  std::optional<Rename> rename_;
  // Timeline.
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
  // Library.
  char search_[128] = {0};
  std::string selectedRom_;
  bool focusPlay_ = false;
  bool focusSearch_ = false;
  // Settings.
  int settingsPage_ = 0;
  std::string assignElement_;  // physical id whose action is being picked
  std::string capturingAction_;
  // MP4 export.
  ExportJob exportJob_;
  bool exportDialog_ = false;   // the dialog is up (settings or progress)
  bool exportOpened_ = false;
  bool exportStarted_ = false;  // the dialog shows this job's progress / result
  bool exportCropOverscan_ = true;
  bool exportPar87_ = false;
  bool exportApplyFlash_ = true;
  bool exportApplyCRT_ = false;
  bool exportWholeTake_ = true;
  int exportStart_ = 0, exportEnd_ = 0;
  int exportPreset_ = -1;  // index into rnf_export_preset_get (-1: the default canvas)
  int exportEncoder_ = 0;  // 0 auto, then a named H.264 encoder
  char exportName_[200] = {0};
  std::string exportError_;
};

}  // namespace rnl
