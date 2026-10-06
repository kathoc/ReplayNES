// Dear ImGui UI of the Linux frontend, fully gamepad navigable (Gaming Mode has no keyboard) and
// mouse / touch friendly at 1280x800 (scaled with the window and Settings -> UI scale).
//
//  * No session: the menu is always open on the Library (start screen).
//  * Playing: nothing is drawn over the game except short notices, status badges and the
//    practice pill (all in the same Vulkan pass as the picture). Paused (or the pointer moved):
//    the dock (transport + filmstrip timeline) appears; mouse / touch only - the controller keeps
//    stepping frames with the D-pad.
//  * R3 / Guide / Esc opens the menu: tabs Playback (the dock, navigable, with the game above it),
//    Takes, Bookmarks, Practice, Library, Settings, Guide; L1 / R1 switch tabs, B closes the menu.
//    Emulation is paused while the menu is open and nothing reaches the game.
// Dialogs / file chooser / rename are modal popups (DialogHost); text fields start SDL text
// input, which shows Steam's on-screen keyboard in Gaming Mode.
// Frame-loop thread only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dialogs.h"
#include "export_hook.h"
#include "imgui.h"
#include "replaynes/frontend.h"
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
};

class UI : public DialogHost {
 public:
  enum class Tab { playback, takes, bookmarks, practice, library, settings, guide };

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
  /// The timeline has the D-pad (gamepad scrub mode): ImGui must not navigate with it.
  bool navGamepadSuspended() const { return timelinePad_; }

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
  void askRename(const std::string& title, const std::string& initial, std::function<void(const std::string&)> apply);
  float S(float v) const { return v * scale_; }
  bool hasSession() const;
  std::vector<Tab> tabs() const;
  ExportHost exportHost() const;
  // ui_play.cpp
  void buildDock(bool inMenu, double now);
  void buildTransport(bool inMenu);
  void buildTimeline(float width, bool inMenu, double now);
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
  void changed();

  Deps d_;
  float scale_ = 1.0f;
  int windowHeight_ = 800;
  ImFont* font_ = nullptr;
  bool menuOpen_ = false;
  Tab tab_ = Tab::library;
  bool focusFirst_ = false;  // gamepad focus on the first item of the page (next frame)
  double lastPointer_ = -10;
  bool practicePanel_ = false;
  // Notices.
  std::string notice_;
  double noticeTime_ = 0;
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
  bool timelinePad_ = false;  // gamepad scrub mode on the focused timeline
  // Library.
  char search_[128] = {0};
  std::string selectedRom_;
  bool focusPlay_ = false;
  // Settings.
  int settingsPage_ = 0;
  std::string assignElement_;  // physical id whose action is being picked
  std::string capturingAction_;
};

}  // namespace rnl
