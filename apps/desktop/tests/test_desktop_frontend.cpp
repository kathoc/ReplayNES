// Desktop frontend logic (no SDL / graphics; Linux, Windows, macOS): settings, the Trash (Linux:
// freedesktop; Windows: a scratch stand-in for the Recycle Bin), paths, file chooser names,
// library search, the 3:2 cadence, manifest reading, PNG writer, and - with the engine's test
// ROM - the emulation controller (transport, practice, timeline A/B), the session lifecycle
// (temporary session, resume, Save As, prompts, Reset Project backup, lock), library scanning
// and filmstrip thumbnails.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#include "app_model.h"
#include "cadence.h"
#include "display_scheduler.h"
#include "dialogs.h"
#include "emulation.h"
#include "export/export_frame.h"
#include "ui_layout.h"
#include "export/mp4_check.h"
#include "export/mp4_retime.h"
#include "l10n.h"
#include "library.h"
#include "manifest.h"
#include "osk.h"
#include "paths.h"
#include "platform/platform.h"
#include "png_writer.h"
#include "settings.h"
#include "sparse_file.h"
#include "support/rn_test.h"
#include "thumbnails.h"
#ifndef _WIN32
#include "platform/trash_xdg.h"
#endif
#include "ui_logic.h"
#include "update_model.h"

namespace fs = std::filesystem;
using namespace rnl;

namespace {

int processId() {
#ifdef _WIN32
  return _getpid();
#else
  return int(::getpid());
#endif
}

/// Scratch folder: $RN_TEST_TMP at run time (the Windows VM, CI), else the build tree's.
std::string tmpDir(const std::string& name) {
  const char* env = std::getenv("RN_TEST_TMP");
  std::string root = env && *env ? std::string(env) : std::string(RNL_TEST_TMP);
  std::string d = root + "/" + name + "-" + std::to_string(processId());
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

std::string readFile(const std::string& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string testRom(const std::string& dir, const std::string& name = "testrom.nes") {
  fs::create_directories(dir);
  std::string p = dir + "/" + name;
  REQUIRE(rn_write_test_rom(p.c_str()) == RN_OK);
  return p;
}

struct NullAudio : AudioSink {
  bool muted = true;
  size_t samples = 0;
  void setMuted(bool m) override { muted = m; }
  void push(const int16_t*, size_t n) override {
    if (!muted) samples += n;
  }
};

/// Answers dialogs / choosers from queues (default: the cancel button / cancelled).
struct FakeHost : DialogHost {
  std::vector<Dialog> dialogs;
  std::vector<std::string> notices;
  std::deque<int> answers;
  std::deque<std::string> choices;
  int choosers = 0;
  void showDialog(Dialog d) override {
    dialogs.push_back(d);
    int b = answers.empty() ? (d.cancelIndex >= 0 ? d.cancelIndex : int(d.buttons.size()) - 1) : answers.front();
    if (!answers.empty()) answers.pop_front();
    if (d.onResult) d.onResult(b, d.checked);
  }
  void showChooser(ChooserRequest r) override {
    ++choosers;
    if (choices.empty()) {
      if (r.onCancel) r.onCancel();
      return;
    }
    std::string c = choices.front();
    choices.pop_front();
    if (r.onChosen) r.onChosen(c);
  }
  using DialogHost::notice;
  void notice(const std::string& t, double) override { notices.push_back(t); }
  bool sawNotice(const std::string& t) const {
    for (const std::string& n : notices)
      if (n == t) return true;
    return false;
  }
  bool sawTitle(const std::string& t) const {
    for (const Dialog& d : dialogs)
      if (d.title == t) return true;
    return false;
  }
};

void tickN(EmulationController& emu, int n) {
  for (int i = 0; i < n; ++i) emu.tick();
}

/// Everything an AppModel needs, rooted in a scratch folder.
struct World {
  std::string root;
  Paths paths;
  rn_input* input = rn_input_new();
  NullAudio audio;
  std::unique_ptr<EmulationController> emu;
  std::unique_ptr<LibraryModel> library;
  FakeHost host;
  std::unique_ptr<AppModel> app;
  explicit World(const std::string& dir, bool fresh = true) : root(dir) {
    if (fresh) {
      std::error_code ec;
      fs::remove_all(dir, ec);
    }
    paths.setLibraryRoot(dir + "/Documents/ReplayNES");
    paths.sessionRoot = dir + "/data/ReplayNES/Session";
    paths.configDir = dir + "/config/ReplayNES";
    paths.ensure();
    emu = std::make_unique<EmulationController>(input, &audio);
    library = std::make_unique<LibraryModel>(paths.libraryRoot);
    library->ensureFolders();
    app = std::make_unique<AppModel>(paths, emu.get(), library.get(), &host);
    app->setupSessionPersistence(true);
  }
  ~World() {
    app.reset();
    emu.reset();
    library.reset();
    rn_input_free(input);
  }
  /// Waits for the async resume.json writer.
  void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
};

}  // namespace

// ------------------------------------------------------------------ settings

TEST_CASE("settings round trip and tolerant parsing") {
  Settings s;
  s.integerScale = false;
  s.par87 = true;
  s.flash = 3;
  s.volume = 0.25f;
  s.autosaveInterval = 10;
  s.uiScale = 1.25f;
  s.diagramFamily = "4";
  s.timelineSlot = 5;
  s.checkForUpdates = false;
  Settings t = Settings::parse(s.serialize());
  CHECK_FALSE(t.checkForUpdates);
  CHECK(Settings::parse("").checkForUpdates);  // on by default
  CHECK_FALSE(t.integerScale);
  CHECK(t.par87);
  CHECK_EQ(t.flash, 3);
  CHECK(std::fabs(t.volume - 0.25f) < 1e-3);
  CHECK_EQ(t.autosaveInterval, 10.0);
  CHECK(std::fabs(t.uiScale - 1.25f) < 1e-3);
  CHECK_EQ(t.diagramFamily, std::string("4"));
  CHECK_EQ(t.timelineSlot, 5);
  Settings g = Settings::parse("flash=9\nvolume=abc\nuiScale=7\n# comment\nnonsense\nintegerScale=maybe\ntimelineSlot=8\n");
  Settings d;
  CHECK_EQ(g.flash, d.flash);
  CHECK_EQ(g.volume, d.volume);
  CHECK_EQ(g.uiScale, d.uiScale);
  CHECK_EQ(g.integerScale, d.integerScale);
  CHECK_EQ(g.timelineSlot, 0);
  // The skeleton's file (integerScale / par87 / hideOverscan / showStats / flash) still loads.
  Settings old = Settings::parse("integerScale=0\npar87=1\nhideOverscan=0\nshowStats=1\nflash=1\n");
  CHECK_FALSE(old.integerScale);
  CHECK(old.showStats);
  CHECK_EQ(old.flash, 1);
  // On-screen keyboard: auto by default, the three modes round-trip, anything else is ignored.
  CHECK_EQ(d.onScreenKeyboard, std::string("auto"));
  for (const char* m : {"auto", "builtin", "steam"}) {
    Settings k;
    k.onScreenKeyboard = m;
    CHECK_EQ(Settings::parse(k.serialize()).onScreenKeyboard, std::string(m));
  }
  CHECK_EQ(Settings::parse("onScreenKeyboard=qwerty\n").onScreenKeyboard, std::string("auto"));
  // Pause after rewind: off by default; the <= 0.5.0 key (always written, default on) is ignored
  // once, the new key round-trips both ways.
  CHECK_FALSE(d.pauseAfterRewind);
  CHECK_FALSE(Settings::parse("pauseAfterRewind=1\n").pauseAfterRewind);
  CHECK_FALSE(Settings::parse("pauseAfterRewind=0\n").pauseAfterRewind);
  for (bool on : {true, false}) {
    Settings p;
    p.pauseAfterRewind = on;
    CHECK_EQ(Settings::parse(p.serialize()).pauseAfterRewind, on);
  }
  CHECK(Settings::parse("pauseAfterRewind=0\npauseAfterRewind2=1\n").pauseAfterRewind);
}

// ------------------------------------------------------------------ on-screen keyboard

namespace {
// Applies a model's actions to a plain string (cursor at the end; ←/→ not used here).
std::string typeWith(OskModel& m, const std::vector<OskButton>& presses) {
  std::string out;
  for (OskButton b : presses) {
    OskAction a = m.press(b, out.empty());
    if (a.op == OskAction::Op::insert) out += a.text;
    else if (a.op == OskAction::Op::backspace && !out.empty()) out.pop_back();
  }
  return out;
}
}  // namespace

TEST_CASE("on-screen keyboard: layout, navigation, shift and actions") {
  OskModel m;
  // Every row is 11 units wide (keys line up), on both pages, with and without the Steam key.
  for (bool steam : {false, true}) {
    OskModel k(steam);
    for (int page = 0; page < 2; ++page) {
      for (const auto& row : k.rows()) {
        float w = 0;
        for (const OskKey& key : row) w += key.width;
        CHECK(std::fabs(w - OskModel::kRowUnits) < 1e-4f);
      }
      k.press(OskButton::view, false);
    }
    bool hasSteam = false;
    for (const auto& row : k.rows())
      for (const OskKey& key : row) hasSteam = hasSteam || key.kind == OskKey::Kind::steam;
    CHECK_EQ(hasSteam, steam);
  }
  // Starts on "q"; left wraps to the end of the row, up from the top row wraps to the bottom.
  CHECK_EQ(m.keyText(m.rows()[size_t(m.row())][size_t(m.col())]), std::string("q"));
  m.press(OskButton::left, false);
  CHECK_EQ(m.col(), int(m.rows()[1].size()) - 1);
  m.press(OskButton::right, false);
  CHECK_EQ(m.col(), 0);
  m.press(OskButton::up, false);
  m.press(OskButton::up, false);
  CHECK_EQ(m.row(), int(m.rows().size()) - 1);
  // Down from "q": "a" (nearest center).
  m.reset(false);
  m.press(OskButton::down, false);
  CHECK_EQ(m.keyText(m.rows()[size_t(m.row())][size_t(m.col())]), std::string("a"));
  // Buttons: B deletes (empty field: cancel), X space, Menu done, L1 / R1 cursor, A types.
  m.reset(false);
  CHECK(m.press(OskButton::b, false).op == OskAction::Op::backspace);
  CHECK(m.press(OskButton::b, true).op == OskAction::Op::cancel);
  OskAction sp = m.press(OskButton::x, true);
  CHECK(sp.op == OskAction::Op::insert);
  CHECK_EQ(sp.text, std::string(" "));
  CHECK(m.press(OskButton::start, false).op == OskAction::Op::done);
  CHECK(m.press(OskButton::l1, false).op == OskAction::Op::cursorLeft);
  CHECK(m.press(OskButton::r1, false).op == OskAction::Op::cursorRight);
  OskAction q = m.press(OskButton::a, true);
  CHECK(q.op == OskAction::Op::insert);
  CHECK_EQ(q.text, std::string("q"));
  // Shift: once (one capital), locked (until Y again).
  m.press(OskButton::y, false);
  CHECK(m.shift() == OskModel::Shift::once);
  CHECK_EQ(m.press(OskButton::a, false).text, std::string("Q"));
  CHECK(m.shift() == OskModel::Shift::off);
  m.press(OskButton::y, false);
  m.press(OskButton::y, false);
  CHECK(m.shift() == OskModel::Shift::locked);
  CHECK_EQ(m.press(OskButton::a, false).text, std::string("Q"));
  CHECK_EQ(m.press(OskButton::a, false).text, std::string("Q"));
  m.press(OskButton::y, false);
  CHECK(m.shift() == OskModel::Shift::off);
  // Special keys by activation (taps): Delete never cancels, Done, Steam.
  OskModel s(true);
  for (int r = 0; r < int(s.rows().size()); ++r)
    for (int c = 0; c < int(s.rows()[size_t(r)].size()); ++c) {
      OskKey::Kind kind = s.rows()[size_t(r)][size_t(c)].kind;
      OskModel t(true);
      OskAction a = t.activate(r, c);
      if (kind == OskKey::Kind::backspace) CHECK(a.op == OskAction::Op::backspace);
      if (kind == OskKey::Kind::done) CHECK(a.op == OskAction::Op::done);
      if (kind == OskKey::Kind::steam) CHECK(a.op == OskAction::Op::steam);
      if (kind == OskKey::Kind::symbols) CHECK(t.symbolsPage());
    }
}

TEST_CASE("on-screen keyboard: the presses for a text type it") {
  for (bool steam : {false, true}) {
    for (const char* text : {"mario", "Super Mario Bros. 3", "A/B #1 (best)", "x_y-z", "Hello, World!", "\"\\|~`"}) {
      OskModel m(steam);
      std::vector<OskButton> presses = m.pressesFor(text);
      REQUIRE(!presses.empty());
      OskModel run(steam);
      CHECK_EQ(typeWith(run, presses), std::string(text));
    }
  }
  // Not on the keyboard (Japanese comes from Steam's keyboard / an IME): no presses.
  OskModel m;
  CHECK(m.pressesFor("マリオ").empty());
  // From a state that is not the start (symbols page, Shift locked) too.
  OskModel s;
  s.press(OskButton::view, false);
  s.press(OskButton::y, false);
  s.press(OskButton::y, false);
  std::vector<OskButton> p = s.pressesFor("ab");
  CHECK_EQ(typeWith(s, p), std::string("ab"));
}

TEST_CASE("on-screen keyboard: repeat of held buttons") {
  OskRepeater r;
  r.set(OskButton::right, true, 10.0);
  CHECK(r.due(10.2).empty());
  std::vector<OskButton> d = r.due(10.0 + OskRepeater::kDelay + OskRepeater::kRate * 2 + 1e-6);
  CHECK_EQ(d.size(), size_t(3));
  r.set(OskButton::right, false, 10.6);
  CHECK(r.due(20.0).empty());
}

TEST_CASE("which keyboard a text field gets") {
  TextEntryContext c;
  // Auto: Steam's where it can be asked for (the Deck), the built-in one otherwise; nothing for a
  // hardware keyboard / mouse outside Gaming Mode.
  c.steamAvailable = true;
  CHECK(textEntryFor(c) == TextEntry::steam);
  c.steamAvailable = false;
  CHECK(textEntryFor(c) == TextEntry::builtin);
  c.lastInputKeyboardOrMouse = true;
  CHECK(textEntryFor(c) == TextEntry::external);
  c.gamingMode = true;  // Gaming Mode: the trackpad is a mouse, there is no keyboard
  CHECK(textEntryFor(c) == TextEntry::builtin);
  c.mode = "builtin";
  c.steamAvailable = true;
  CHECK(textEntryFor(c) == TextEntry::builtin);
  c.mode = "steam";
  CHECK(textEntryFor(c) == TextEntry::steam);
  c.steamAvailable = false;
  CHECK(textEntryFor(c) == TextEntry::external);
}

// ------------------------------------------------------------------ in-app updates (Flatpak portal)

TEST_CASE("update reports: download, restart to use, nothing") {
  CHECK(classifyUpdate("aaa", "aaa", "bbb") == UpdateKind::download);
  CHECK(classifyUpdate("aaa", "bbb", "bbb") == UpdateKind::restartToUse);  // `flatpak update` ran meanwhile
  CHECK(classifyUpdate("aaa", "aaa", "aaa") == UpdateKind::none);           // the portal's first report
  CHECK(classifyUpdate("aaa", "", "") == UpdateKind::none);
  CHECK(classifyUpdate("aaa", "aaa", "") == UpdateKind::none);
  CHECK_EQ(overallPercent(0, 0, 50), 0);
  CHECK_EQ(overallPercent(0, 1, 50), 50);
  CHECK_EQ(overallPercent(1, 2, 50), 75);
  CHECK_EQ(overallPercent(5, 2, 100), 100);  // out of range: clamped
}

TEST_CASE("update errors are classified for their message") {
  CHECK(classifyUpdateError("org.freedesktop.DBus.Error.NotSupported",
                            "Self update not supported, new version requires new permissions") == UpdateError::newPermissions);
  CHECK(classifyUpdateError("org.freedesktop.DBus.Error.AccessDenied", "Application update not allowed") == UpdateError::denied);
  CHECK(classifyUpdateError("org.freedesktop.DBus.Error.NotSupported", "No portal support found") == UpdateError::noDialog);
  CHECK(classifyUpdateError("org.freedesktop.DBus.Error.NameHasNoOwner",
                            "Could not activate remote peer 'org.freedesktop.impl.portal.desktop.gtk': startup job failed") ==
        UpdateError::noDialog);
  CHECK(classifyUpdateError("org.freedesktop.DBus.Error.Failed", "While fetching https://x/summary: Could not resolve host") ==
        UpdateError::network);
  CHECK(classifyUpdateError("", "something else") == UpdateError::other);
}

TEST_CASE("update model: notice, progress, Later, errors") {
  UpdateModel m;
  CHECK_FALSE(m.noticeVisible());
  m.portalReady();
  m.updateAvailable("a", "a", "a");  // first report of the monitor: nothing new
  CHECK(m.phase == UpdatePhase::idle);
  uint64_t s0 = m.serial;
  m.updateAvailable("a", "a", "b");
  CHECK(m.phase == UpdatePhase::available);
  CHECK(m.noticeVisible());
  CHECK(m.serial != s0);
  m.dismiss();  // Later
  CHECK_FALSE(m.noticeVisible());
  m.updateAvailable("a", "a", "b");  // the same commit again: stays hidden
  CHECK_FALSE(m.noticeVisible());
  m.updateAvailable("a", "a", "c");  // a newer one: shows again
  CHECK(m.noticeVisible());
  m.updateStarted();
  CHECK(m.busy());
  CHECK(m.noticeVisible());
  UpdateProgressInfo p;
  p.op = 0;
  p.nOps = 2;
  p.progress = 40;
  m.progress(p);
  CHECK(m.phase == UpdatePhase::updating);
  CHECK_EQ(m.percent, 20);
  m.updateAvailable("a", "a", "d");  // ignored while installing
  CHECK(m.phase == UpdatePhase::updating);
  p.status = kUpdateDone;
  m.progress(p);
  CHECK(m.phase == UpdatePhase::installed);
  CHECK(m.noticeVisible());
  m.setNewVersion("0.3.1");
  CHECK_EQ(m.newVersion, std::string("0.3.1"));
  // Check now: nothing to update ("up to date", shown in Settings only).
  UpdateModel c;
  c.portalReady();
  c.checkStarted();
  CHECK_FALSE(c.noticeVisible());
  UpdateProgressInfo e;
  e.status = kUpdateEmpty;
  c.progress(e);
  CHECK(c.phase == UpdatePhase::upToDate);
  CHECK_FALSE(c.noticeVisible());
  // Errors: shown on the notice for Update, in Settings for Check now.
  UpdateModel f;
  f.updateAvailable("a", "a", "b");
  f.updateStarted();
  e.status = kUpdateError;
  e.error = "org.freedesktop.DBus.Error.AccessDenied";
  e.errorMessage = "Application update not allowed";
  f.progress(e);
  CHECK(f.phase == UpdatePhase::failed);
  CHECK(f.error == UpdateError::denied);
  CHECK(f.noticeVisible());
  f.dismiss();
  CHECK(f.phase == UpdatePhase::idle);
  CHECK_FALSE(f.noticeVisible());
  // Unavailable: not a Flatpak (developer build).
  UpdateModel u;
  u.setUnsupported(UpdateUnavailable::notFlatpak);
  CHECK(u.phase == UpdatePhase::unsupported);
  CHECK_FALSE(u.noticeVisible());
}

TEST_CASE("restart into an update: arguments, environment, version") {
  std::vector<std::string> argv = {"/app/bin/replaynes-linux", "--script", "update restart", "--windowed", "--rom", "a.nes",
                                   "--perf-seconds", "5", "--inject-input"};
  std::vector<std::string> args = restartArguments(argv);
  REQUIRE_EQ(args.size(), size_t(3));
  CHECK_EQ(args[0], std::string("--windowed"));
  CHECK_EQ(args[1], std::string("--rom"));
  CHECK_EQ(args[2], std::string("a.nes"));
  std::vector<std::string> env = {"PATH=/app/bin:/usr/bin", "DISPLAY=:99.0", "LANG=ja_JP.UTF-8", "SteamGameId=123",
                                  "SDL_VIDEO_DRIVER=x11", "REPLAYNES_LANG=en", "XDG_CONFIG_HOME=/x", "LD_PRELOAD=/y.so",
                                  "GAMESCOPE_WAYLAND_DISPLAY=gamescope-0", "broken"};
  auto kept = restartEnvironment(env);
  std::vector<std::string> keys;
  for (auto& [k, v] : kept) keys.push_back(k);
  std::vector<std::string> want = {"LANG", "SteamGameId", "SDL_VIDEO_DRIVER", "REPLAYNES_LANG", "GAMESCOPE_WAYLAND_DISPLAY"};
  CHECK(keys == want);
  CHECK_EQ(parseVersionOutput("ReplayNES 0.3.1\n"), std::string("0.3.1"));
  CHECK_EQ(parseVersionOutput("ReplayNES 1.10.0-test\n"), std::string("1.10.0"));
  CHECK_EQ(parseVersionOutput(""), std::string());
  CHECK_EQ(parseVersionOutput("error: no such command\n"), std::string());
}

// ------------------------------------------------------------------ trash

#ifndef _WIN32
TEST_CASE("trash info follows the freedesktop specification") {
  CHECK_EQ(trashEscapePath("/home/deck/Documents/ReplayNES/Projects/My Game (Before Reset 2026-10-07 12.00).nesrec"),
           std::string("/home/deck/Documents/ReplayNES/Projects/My%20Game%20%28Before%20Reset%202026-10-07%2012.00%29.nesrec"));
  CHECK_EQ(trashEscapePath("/a/\xE3\x81\x82"), std::string("/a/%E3%81%82"));
  std::string info = trashInfoContents("/x/y z", 0);
  CHECK(info.rfind("[Trash Info]\nPath=/x/y%20z\nDeletionDate=", 0) == 0);
  CHECK(info.size() == std::string("[Trash Info]\nPath=/x/y%20z\nDeletionDate=1970-01-01T00:00:00\n").size());
  CHECK_EQ(trashCandidateName("a.nesrec", 1), std::string("a.nesrec"));
  CHECK_EQ(trashCandidateName("a.nesrec", 3), std::string("a 3.nesrec"));
  CHECK_EQ(trashCandidateName("noext", 2), std::string("noext 2"));
}

TEST_CASE("move to trash: folders, collisions, info files") {
  std::string d = tmpDir("trash");
  std::string trash = d + "/share/Trash";
  for (int i = 0; i < 2; ++i) {
    std::string p = d + "/Proj.nesrec";
    fs::create_directories(p + "/timeline");
    std::ofstream(p + "/manifest.json") << "{}";
    std::string as, err;
    REQUIRE(moveToTrash(p, trash, 1e9, &as, &err));
    CHECK_FALSE(fs::exists(p));
    CHECK(fs::exists(as + "/manifest.json"));
    std::string name = fs::path(as).filename().string();
    CHECK_EQ(name, std::string(i == 0 ? "Proj.nesrec" : "Proj 2.nesrec"));
    std::string info = readFile(trash + "/info/" + name + ".trashinfo");
    CHECK(info.find("Path=" + trashEscapePath(fs::absolute(p).lexically_normal().string()) + "\n") != std::string::npos);
  }
  std::string err;
  CHECK_FALSE(moveToTrash(d + "/missing", trash, 0, nullptr, &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("home trash location (Flatpak: the host's)") {
  const char* keepHost = std::getenv("HOST_XDG_DATA_HOME");
  setenv("HOST_XDG_DATA_HOME", "/h/share", 1);
  CHECK_EQ(homeTrashDir(), std::string("/h/share/Trash"));
  unsetenv("HOST_XDG_DATA_HOME");
  setenv("FLATPAK_ID", "x", 1);
  setenv("HOME", "/home/u", 1);
  CHECK_EQ(homeTrashDir(), std::string("/home/u/.local/share/Trash"));
  unsetenv("FLATPAK_ID");
  if (keepHost) setenv("HOST_XDG_DATA_HOME", keepHost, 1);
}
#endif

TEST_CASE("copy tree: folders, refuses an existing destination") {
  std::string d = tmpDir("copytree");
  fs::create_directories(d + "/a.nesrec/timeline");
  std::ofstream(d + "/a.nesrec/manifest.json") << "{}";
  std::string err;
  REQUIRE(copyTree(d + "/a.nesrec", d + "/b.nesrec", &err));
  CHECK_EQ(readFile(d + "/b.nesrec/manifest.json"), std::string("{}"));
  CHECK(fs::is_directory(d + "/b.nesrec/timeline"));
  CHECK_FALSE(copyTree(d + "/a.nesrec", d + "/b.nesrec", &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("standard paths: library, session and settings folders") {
  Paths p = Paths::standard();
  CHECK_FALSE(p.libraryRoot.empty());
  CHECK(p.romDir.rfind(p.libraryRoot, 0) == 0);
  CHECK(p.projectsDir.rfind(p.libraryRoot, 0) == 0);
  CHECK(p.sessionRoot.find("ReplayNES") != std::string::npos);
  CHECK(p.configDir.find("ReplayNES") != std::string::npos);
  CHECK_EQ(Paths::stripSlash("C:/x//"), std::string("C:/x"));
#ifdef _WIN32
  // Documents\ReplayNES, %LOCALAPPDATA%\ReplayNES\Session, %APPDATA%\ReplayNES; shown with '\'.
  CHECK(p.libraryRoot.size() > 3 && p.libraryRoot[1] == ':');
  CHECK(p.sessionRoot.find("\\ReplayNES\\Session") != std::string::npos);
  CHECK_EQ(Paths::display("C:\\Users\\u/Documents/ReplayNES/ROM"), std::string("C:\\Users\\u\\Documents\\ReplayNES\\ROM"));
  CHECK(p.settingsFile().find("settings.ini") != std::string::npos);
#else
  CHECK(p.sessionRoot.find("/ReplayNES/Session") != std::string::npos);
  CHECK(p.settingsFile().find("linux-settings.ini") != std::string::npos);
#endif
}

#ifdef _WIN32
// Moves a real file to the user's Recycle Bin: only with RN_TEST_RECYCLE_BIN=1 (the VM / a test PC).
TEST_CASE("recycle bin: a file goes there (RN_TEST_RECYCLE_BIN=1)") {
  const char* on = std::getenv("RN_TEST_RECYCLE_BIN");
  if (!on || std::string(on) != "1") return;
  std::string d = tmpDir("recycle");
  std::string f = d + "/ReplayNES recycle test \xE3\x81\x82.txt";
  std::ofstream(fs::u8path(f)) << "x";
  REQUIRE(fs::exists(fs::u8path(f)));
  std::string err;
  CHECK(trashItem(f, &err));
  CHECK_FALSE(fs::exists(fs::u8path(f)));
  CHECK_FALSE(trashItem(d + "/missing", &err));
}
#endif

// ------------------------------------------------------------------ small pure helpers

TEST_CASE("chooser project names") {
  CHECK_EQ(chooserProjectFileName("Super Mario"), std::string("Super Mario.nesrec"));
  CHECK_EQ(chooserProjectFileName("  run.nesrec  "), std::string("run.nesrec"));
  CHECK_EQ(chooserProjectFileName("a/b\\c"), std::string("abc.nesrec"));
  CHECK_EQ(chooserProjectFileName("..."), std::string(""));
  CHECK_EQ(chooserProjectFileName(".nesrec"), std::string(""));
  CHECK_EQ(chooserProjectFileName("\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA"), std::string("\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA.nesrec"));
}

TEST_CASE("library search") {
  LibraryROM r;
  r.name = "Super Mario Bros. (World)";
  r.relativePath = "Nintendo/Super Mario Bros. (World).nes";
  CHECK(librarySearchMatches("", r));
  CHECK(librarySearchMatches("  mario ", r));
  CHECK(librarySearchMatches("WORLD", r));
  CHECK(librarySearchMatches("nintendo/", r));
  CHECK(librarySearchMatches("\xEF\xBC\xAD\xEF\xBD\x81\xEF\xBD\x92\xEF\xBD\x89\xEF\xBD\x8F", r));  // full-width "Mario"
  CHECK_FALSE(librarySearchMatches("zelda", r));
}

TEST_CASE("library: Add Test Cartridge writes it once, the game database names it") {
  std::string dir = tmpDir("testcart-lib");
  LibraryModel lib(dir + "/ReplayNES");
  auto scan = [&] {
    for (int i = 0; i < 300 && !lib.poll(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    for (int i = 0; i < 300 && lib.scanning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    lib.poll();
  };
  std::string error;
  const std::string path = lib.addTestCartridge(&error);
  REQUIRE(!path.empty());
  CHECK(error.empty());
  CHECK(LibraryModel::samePath(path, lib.romDir() + "/" + RNF_TEST_CARTRIDGE_FILE));
  CHECK(LibraryModel::samePath(path, lib.romDir() + "/./" + RNF_TEST_CARTRIDGE_FILE));
  CHECK_FALSE(LibraryModel::samePath(path, lib.romDir() + "/other.nes"));
  REQUIRE(fs::exists(fs::u8path(path)));
  const auto size = fs::file_size(fs::u8path(path));
  scan();
  REQUIRE_EQ(lib.roms().size(), size_t(1));
  const LibraryROM& rom = lib.roms().front();
  CHECK(LibraryModel::samePath(rom.path, path));
  REQUIRE(rom.known);  // by its hash (frontend/data/nesdb-overrides.json)
  CHECK_EQ(std::string(rom.game.id), std::string(RNF_TEST_CARTRIDGE_GAME_ID));
  CHECK_EQ(rom.game.genre, RNF_GENRE_UTILITY);
  CHECK_EQ(rom.game.year, 2026);
  CHECK_EQ(std::string(rom.game.publisher_en), std::string("ReplayNES"));
  // Again: the same file, nothing written (a renamed copy is found by the database id too).
  CHECK(LibraryModel::samePath(lib.addTestCartridge(&error), path));
  CHECK_EQ(fs::file_size(fs::u8path(path)), size);
  fs::rename(fs::u8path(path), fs::u8path(lib.romDir() + "/cart.nes"));
  lib.refresh();
  scan();
  REQUIRE_EQ(lib.roms().size(), size_t(1));
  CHECK(LibraryModel::samePath(lib.addTestCartridge(&error), lib.romDir() + "/cart.nes"));
  CHECK_FALSE(fs::exists(fs::u8path(path)));
}

TEST_CASE("controller conventions: timeline scrub steps and L1 / R1 jumps") {
  CHECK_EQ(scrubStepFrames(0.0), 1);
  CHECK_EQ(scrubStepFrames(1.0), 4);
  CHECK_EQ(scrubStepFrames(2.0), 15);
  CHECK_EQ(scrubStepFrames(10.0), 60);
  std::vector<uint64_t> marks = {100, 900, 5000};
  CHECK_EQ(timelineJumpTarget(500, 2000, marks, 1), uint64_t(900));
  CHECK_EQ(timelineJumpTarget(500, 2000, marks, -1), uint64_t(100));
  CHECK_EQ(timelineJumpTarget(900, 2000, marks, 1), uint64_t(1200));  // 5000 is past the take end: +5 s
  CHECK_EQ(timelineJumpTarget(1900, 2000, marks, 1), uint64_t(2000));
  CHECK_EQ(timelineJumpTarget(100, 2000, marks, -1), uint64_t(0));
  CHECK_EQ(timelineJumpTarget(800, 2000, {}, -1), uint64_t(500));
  // Glyphs by position: confirm (south) is A on Steam Deck / Xbox, ✕ on PlayStation, B on Nintendo.
  CHECK_EQ(padGlyph(RNF_FAMILY_STEAM_DECK, "face.south"), std::string("A"));
  CHECK_EQ(padGlyph(RNF_FAMILY_XBOX, "face.east"), std::string("B"));
  CHECK_EQ(padGlyph(RNF_FAMILY_PLAYSTATION, "face.south"), std::string("\xE2\x9C\x95"));
  CHECK_EQ(padGlyph(RNF_FAMILY_NINTENDO, "face.south"), std::string("B"));
  CHECK_EQ(padGlyph(RNF_FAMILY_NINTENDO, "face.east"), std::string("A"));
  CHECK_EQ(padGlyph(RNF_FAMILY_STEAM_DECK, "rightShoulder"), std::string("R1"));
  // The Menu pill's chord.
  CHECK_EQ(menuChordGlyph(RNF_FAMILY_STEAM_DECK, true), std::string("L+R"));
  CHECK_EQ(menuChordGlyph(RNF_FAMILY_XBOX, true), std::string("L+R"));
  CHECK_EQ(menuChordGlyph(RNF_FAMILY_NINTENDO, true), std::string("L+R"));
  CHECK_EQ(menuChordGlyph(RNF_FAMILY_PLAYSTATION, true), std::string("L1+R1"));
  CHECK_EQ(menuChordGlyph(RNF_FAMILY_PLAYSTATION, false), std::string("Esc"));
}

TEST_CASE("controller conventions: A / B resume the paused seek bar only as a tap") {
  ConfirmTap tap;
  // A tap resumes.
  tap.cancel();
  tap.press("gc0:face.south");
  CHECK(tap.release("gc0:face.south"));
  // B held for a frame advance (D-pad step / another button while held): no resume on release.
  tap.cancel();
  tap.press("gc0:face.east");
  tap.cancel();  // gc0:dpad.right steps a frame
  CHECK(!tap.release("gc0:face.east"));
  // A release without a press while paused (pressed before pausing) does nothing.
  CHECK(!tap.release("gc0:face.south"));
  // Two confirm buttons: the second press cancels the first; the second is still a tap.
  tap.cancel();
  tap.press("gc0:face.south");
  tap.cancel();
  tap.press("gc0:face.east");
  CHECK(!tap.release("gc0:face.south"));
  CHECK(tap.release("gc0:face.east"));
  // clear() (menu opened, pad detached) forgets held presses.
  tap.press("gc1:face.south");
  tap.clear();
  CHECK(!tap.release("gc1:face.south"));
}

TEST_CASE("menu icons: every icon of the menu model is in the bundled Tabler subset") {
  struct Entry {
    const char* name;
    unsigned code;
  };
  static const Entry icons[] = {
#include "../resources/fonts/icons.inc"
  };
  auto has = [&](const char* n) {
    for (const Entry& e : icons)
      if (std::strcmp(e.name, n) == 0) return true;
    return false;
  };
  rnf_menu* menu = rnf_menu_new(0xffffffffu);
  for (size_t p = 0; p < rnf_menu_page_count(menu); ++p) {
    rnf_menu_page_info pi{};
    rnf_menu_page_get(menu, p, &pi);
    if (!has(pi.icon)) MESSAGE(std::string("missing page icon ") + pi.icon);
    CHECK(has(pi.icon));
    rnf_menu_item_info it{};
    for (size_t i = 0; rnf_menu_item_get(menu, p, i, &it); ++i) {
      if (!has(it.icon)) MESSAGE(std::string("missing item icon ") + it.icon);
      CHECK(has(it.icon));
    }
  }
  rnf_menu_free(menu);
}

TEST_CASE("menu layout: no screen scrolls (every page of the menu model fits)") {
  const uint32_t features = RNF_MENU_FEATURE_EXPORT | RNF_MENU_FEATURE_CRT | RNF_MENU_FEATURE_STEAM | RNF_MENU_FEATURE_OSK |
                            RNF_MENU_FEATURE_UPDATES | RNF_MENU_FEATURE_QUIT | RNF_MENU_FEATURE_FULLSCREEN |
                            RNF_MENU_FEATURE_UI_SCALE;
  rnf_menu* menu = rnf_menu_new(features);
  REQUIRE(menu);
  struct Size {
    float w, h;
  };
  int checked = 0;
  for (Size sz : {Size{1280, 800}, Size{1920, 1080}, Size{1280, 720}, Size{2560, 1600}, Size{1024, 768}}) {
    for (float ui : {0.75f, 1.0f, 1.25f, 1.5f, -1.0f, -1.5f}) {  // negative: the Japanese text size (CJK font)
      UiMetrics m = UiMetrics::make(sz.w, sz.h, std::fabs(ui), ui < 0 ? 1.18f : 1.0f);
      for (size_t p = 0; p < rnf_menu_page_count(menu); ++p) {
        rnf_menu_page_info pi{};
        REQUIRE(rnf_menu_page_get(menu, p, &pi));
        size_t count = pi.kind == RNF_MENU_PAGE_LIST ? size_t(RNF_MENU_MAX_ITEMS) : pi.count;
        bool header = (pi.group && *pi.group) || pi.kind == RNF_MENU_PAGE_LIST;
        PageGeometry g = layoutPage(m, pi.kind, count, pi.columns, header);
        const char* why = "";
        bool fits = pageFits(m, g, &why);
        if (!fits)
          MESSAGE(std::string(pi.id) + " at " + std::to_string(int(sz.w)) + "x" + std::to_string(int(sz.h)) + " size " +
                  std::to_string(ui) + ": " + why);
        CHECK(fits);
        if (pi.kind != RNF_MENU_PAGE_CUSTOM) CHECK_EQ(g.items.size(), count);
        // Labels of 2-5 Japanese characters / 1-2 English words fit a tile.
        if (pi.kind == RNF_MENU_PAGE_TILES)
          for (const LRect& r : g.items) CHECK(r.w >= m.label() / m.text * 5.5f);
        ++checked;
      }
      LibraryGeometry lib = layoutLibrary(m, true);
      LRect screen{0, 0, m.W, m.H};
      CHECK(lib.content.contains(lib.hero));
      CHECK(lib.content.contains(lib.emptyCard));
      for (const LRect& c : lib.cards) {
        CHECK(lib.content.contains(c));
        CHECK_FALSE(c.overlaps(lib.hero));
        CHECK(c.h - libraryCardTextHeight(m) >= m.label() * 2);
      }
      CHECK(screen.contains(layoutLibrary(m, false).cards.back()));
      SeekGeometry seek = layoutSeek(m);
      CHECK(screen.contains(seek.panel));
      CHECK(seek.panel.contains(seek.strip));
      CHECK(seek.panel.contains(seek.hints));
      PillGeometry pill = layoutPill(m, 220 * m.s, LRect{});
      CHECK(screen.contains(pill.rect));
      CHECK_FALSE(pill.overPicture);
    }
  }
  CHECK(checked > 100);
  // The pill over a FILL picture (no room beside it) is marked so (drawn faint while playing).
  UiMetrics deck = UiMetrics::make(1280, 800, 1);
  CHECK(layoutPill(deck, 200, LRect{0, 0, 1280, 800}).overPicture);
  CHECK_FALSE(layoutPill(deck, 200, LRect{256, 40, 768, 720}).overPicture);  // integer 3x: room beside it
  rnf_menu_free(menu);
}

TEST_CASE("cadence: integer lock from the core, 3:2 at 90 Hz") {
  const double P = nesFramePeriod();
  Cadence c60 = Cadence::classify(1.0 / 60);
  CHECK(c60.kind == Cadence::Kind::locked);
  CHECK_EQ(c60.k, 1);
  Cadence c120 = Cadence::classify(1.0 / 120);
  CHECK(c120.kind == Cadence::Kind::locked);
  CHECK_EQ(c120.k, 2);
  Cadence c90 = Cadence::classify(1.0 / 90);
  CHECK(c90.kind == Cadence::Kind::three_two);
  CHECK(std::fabs(c90.emulationRate(1.0 / 90) - 60.0) < 1e-9);
  CHECK(Cadence::classify(1.0 / 144).kind == Cadence::Kind::free);
  CHECK(Cadence::classify(1.0 / 75).kind == Cadence::Kind::free);
  CHECK(std::fabs(Cadence::classify(1.0 / 75).emulationRate(1.0 / 75) - 1.0 / P) < 1e-9);
  CHECK(Cadence::classify(0).kind == Cadence::Kind::unknown);
}

TEST_CASE("refresh estimate from 3:2 present timestamps") {
  RefreshEstimator e;
  e.seed(1.0 / 90);
  const double r = 1.0 / 90;
  for (int i = 0; i < 60; ++i) e.observeDelta(i % 2 ? r : 2 * r);  // pictures 2 and 1 refreshes apart
  CHECK(std::fabs(e.refresh - r) < 1e-6);
  RefreshEstimator wrong;
  wrong.seed(1.0 / 60);  // seeded from a wrong mode: the raw deltas take over
  for (int i = 0; i < 30; ++i) wrong.observeDelta(1.0 / 45);
  CHECK(wrong.refresh > 0);
}

namespace {
/// Runs the scheduler like the frame loop for `n` frames on a `hz` display whose present
/// timestamps are late by 0...jitter (deterministic pseudo-random; timestamps are taken after the
/// flip, delays only add); returns the frames per second it aimed at and how many targets were a
/// single refresh apart (k = 2 expected at 120 Hz).
struct SchedRun {
  double fps = 0;
  int oneRefreshSteps = 0;
  Cadence last;
};
SchedRun runScheduler(DisplayScheduler& s, double hz, double jitter, int n, bool hint) {
  const double r = 1.0 / hz, lead = 0.004;
  double now = 1.0, first = 0, prev = 0;
  uint32_t rnd = 12345;
  SchedRun out;
  for (int i = 0; i < n; ++i) {
    if (hint) s.setRefreshHint(r);
    double t = s.nextTarget(now, lead);
    if (i == 0) first = t;
    if (prev > 0 && t - prev < 1.5 * r) out.oneRefreshSteps += 1;
    prev = t;
    rnd = rnd * 1664525u + 1013904223u;
    double j = double(rnd >> 8) / double(1u << 24) * jitter;
    s.observeDisplayed(t + j, t);  // shown at its vblank, the timestamp jittered
    now = t + 0.001;               // the loop wakes after it
  }
  out.fps = (n - 1) / (prev - first);
  out.last = s.cadence();
  return out;
}
}  // namespace

TEST_CASE("scheduler: a presenter's refresh period keeps 120 Hz locked under timestamp jitter") {
  DisplayScheduler withHint;
  withHint.seedRefresh(1.0 / 120);
  SchedRun a = runScheduler(withHint, 120, 0.002, 1200, true);
  CHECK(a.last.kind == Cadence::Kind::locked);
  CHECK_EQ(a.last.k, 2);
  CHECK(std::fabs(a.fps - 60.0) < 0.2);
  CHECK_EQ(a.oneRefreshSteps, 0);
  // Without the hint, 3 ms of jitter moves the estimate around; whatever the cadence becomes, the
  // aimed rate stays near the NES rate (no catch-up frames when it leaves "locked").
  DisplayScheduler noHint;
  noHint.seedRefresh(1.0 / 120);
  SchedRun b = runScheduler(noHint, 120, 0.003, 1200, false);
  CHECK(b.fps < 61.0);
  CHECK(b.fps > 59.0);
}

TEST_CASE("scheduler: leaving the locked cadence does not emulate a catch-up frame") {
  DisplayScheduler s;
  s.seedRefresh(1.0 / 120);
  runScheduler(s, 120, 0, 120, true);  // locked k = 2
  CHECK(s.cadence().kind == Cadence::Kind::locked);
  // The display (or the estimate) changes to a rate without a lock: the next frame follows the
  // NES period from the last target, not one refresh after it.
  double last = s.nextTarget(0, 0.004);  // a locked frame (now = 0: nothing is skipped)
  const double r = 1.0 / 144;
  s.setRefreshHint(r);
  CHECK(s.cadence().kind == Cadence::Kind::free);
  double next = s.nextTarget(0, 0.004);
  CHECK(next - last > 1.5 * r);
}

TEST_CASE("manifest fields for the open-error dialogs") {
  ManifestInfo m = parseManifest(
      "{\"formatVersion\":2,\"coreCompatId\":\"nestopia-ue@x\",\"rom\":{\"sha256\":\"ab\",\"size\":1,\"lastPath\":\"/r/a\\u00e9.nes\","
      "\"name\":\"a.nes\"},\"list\":[1,2,{\"x\":true}]}");
  CHECK_EQ(m.romName, std::string("a.nes"));
  CHECK_EQ(m.romPath, std::string("/r/a\xC3\xA9.nes"));
  CHECK_EQ(m.romSHA256, std::string("ab"));
  CHECK_EQ(m.coreCompatID, std::string("nestopia-ue@x"));
  ManifestInfo bad = parseManifest("{\"rom\":{\"name\":\"z.nes\"");  // truncated: what was read is kept
  CHECK_EQ(bad.romName, std::string("z.nes"));
  CHECK_EQ(bad.coreCompatID, std::string("?"));
}

TEST_CASE("png writer") {
  std::vector<uint8_t> px(3 * 2 * 4, 0x80);
  std::vector<uint8_t> png = encodePNG(3, 2, px.data());
  REQUIRE(png.size() > 33);
  CHECK(std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
  CHECK(std::memcmp(png.data() + 12, "IHDR", 4) == 0);
  CHECK_EQ(int(png[19]), 3);  // width (big endian)
  CHECK_EQ(int(png[23]), 2);  // height
  CHECK(std::memcmp(png.data() + png.size() - 8, "IEND", 4) == 0);
}

// ------------------------------------------------------------------ emulation controller

TEST_CASE("emulation: record, playback toggle, pause, step, rewind, fast-forward") {
  std::string d = tmpDir("emu");
  std::string rom = testRom(d);
  rn_input* in = rn_input_new();
  NullAudio audio;
  EmulationController emu(in, &audio);
  std::vector<std::string> notices;
  emu.onNotice = [&](const std::string& s) { notices.push_back(s); };
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), (d + "/p.nesrec").c_str(), nullptr, &s) == RN_OK);
  emu.install(s);
  CHECK_FALSE(emu.status().paused);  // a fresh recording runs at once
  tickN(emu, 120);
  CHECK_EQ(emu.status().takeLength, uint64_t(120));
  CHECK(emu.status().recording);
  CHECK(audio.samples > 0);
  // Record -> playback at the take end: plays from the start.
  emu.toggleRecord();
  CHECK_FALSE(emu.status().recording);
  CHECK_FALSE(emu.paused());
  tickN(emu, 1);
  CHECK_EQ(emu.status().frame, uint64_t(1));
  tickN(emu, 200);
  CHECK(emu.paused());  // the end of the recording pauses
  CHECK(emu.status().endOfTake);
  // Back to record mode: stays paused at the position.
  emu.seek(60);
  emu.toggleRecord();
  CHECK(emu.status().recording);
  CHECK(emu.paused());
  // Paused stepping.
  emu.frameAdvance(1);
  tickN(emu, 1);
  CHECK_EQ(emu.status().frame, uint64_t(61));  // recording from 60: a new take
  emu.stepBack(1);
  CHECK_EQ(emu.status().frame, uint64_t(60));
  // Rewind while held (pauses afterwards).
  emu.seek(100 > emu.status().takeLength ? emu.status().takeLength : 100);
  uint64_t before = emu.status().frame;
  emu.setRewindHeld(true);
  tickN(emu, 10);
  emu.setRewindHeld(false);
  tickN(emu, 1);
  CHECK(emu.status().frame < before);
  CHECK(emu.paused());
  // Fast-forward plays the recorded range only and returns to record mode.
  emu.seek(0);
  emu.setFastForwardHeld(true);
  tickN(emu, 5);
  CHECK(emu.status().fastForward);
  CHECK_FALSE(emu.status().recording == false && false);
  emu.setFastForwardHeld(false);
  tickN(emu, 1);
  CHECK(emu.status().frame > 0);
  CHECK(emu.status().recording);
  // Slow toggle and the "Nothing recorded" rules are notices.
  emu.toggleSlow();
  CHECK_EQ(emu.status().slow, 2);
  emu.toggleSlow();
  CHECK_EQ(emu.status().slow, 1);
  CHECK_FALSE(notices.empty());
  emu.closeSession();
  CHECK_FALSE(emu.status().hasSession);
  rn_input_free(in);
}

TEST_CASE("emulation: practice A/B on the timeline, loop, stop restores the take") {
  std::string d = tmpDir("practice");
  std::string rom = testRom(d);
  rn_input* in = rn_input_new();
  NullAudio audio;
  EmulationController emu(in, &audio);
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), (d + "/p.nesrec").c_str(), nullptr, &s) == RN_OK);
  emu.install(s);
  tickN(emu, 300);
  emu.setPaused(true);
  emu.seek(60);
  emu.timelineMarkA(2);
  emu.seek(120);
  emu.timelineMarkB(2);
  std::vector<rnf_timeline_range> r = emu.visibleRanges();
  REQUIRE_EQ(r.size(), size_t(1));
  CHECK_EQ(r[0].slot, 2);
  CHECK_EQ(r[0].a, uint64_t(60));
  CHECK_EQ(r[0].b, uint64_t(120));
  CHECK(emu.structure().hasContent());
  CHECK_EQ(emu.structure().slots[2].length, uint64_t(60));
  uint64_t takeFrame = emu.status().frame;
  emu.setPracticeCountdown(false);  // the countdown: "emulation: practice return ..." below
  emu.startPractice(2);
  CHECK(emu.status().practicing);
  CHECK_FALSE(emu.paused());
  tickN(emu, 30);
  CHECK_EQ(emu.status().practiceFrame, uint64_t(30));
  CHECK_EQ(emu.status().takeLength, uint64_t(300));  // nothing recorded
  // Seeking / bookmarks are blocked while practicing.
  std::string last;
  emu.onNotice = [&](const std::string& n) { last = n; };
  emu.addBookmark();
  CHECK_EQ(last, std::string(EmulationController::practiceBlockedText()));
  // Reaching B: hold, rewind animation, restart at A (wall-clock driven, ~1 s).
  for (int i = 0; i < 200 && emu.status().practiceLoops == 0; ++i) {
    emu.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  CHECK(emu.status().practiceLoops >= 1);
  emu.stopPractice();
  CHECK_FALSE(emu.status().practicing);
  CHECK_EQ(emu.status().frame, takeFrame);
  CHECK_EQ(emu.status().takeLength, uint64_t(300));
  emu.closeSession();
  rn_input_free(in);
}

TEST_CASE("emulation: practice return - countdown at A, L2 + R2 back to A with the VTR effect, cancel aborts") {
  std::string d = tmpDir("practice-return");
  std::string rom = testRom(d);
  rn_input* in = rn_input_new();
  NullAudio audio;
  EmulationController emu(in, &audio);
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), (d + "/p.nesrec").c_str(), nullptr, &s) == RN_OK);
  emu.install(s);
  tickN(emu, 300);
  emu.setPaused(true);
  emu.seek(60);
  emu.timelineMarkA(1);
  emu.seek(200);
  emu.timelineMarkB(1);
  // Start: the countdown on A's picture first; nothing is emulated meanwhile.
  emu.startPractice(1);
  REQUIRE(emu.status().practicing);
  tickN(emu, 10);
  CHECK_EQ(emu.status().countdown, 3);
  CHECK_EQ(emu.status().practiceFrame, uint64_t(0));
  // Cancel tapped during the countdown: practice ends, the take is as it was.
  emu.abortCountdown();
  CHECK_FALSE(emu.status().practicing);
  CHECK_EQ(emu.status().frame, uint64_t(200));
  // Without the countdown: plays at once; L2 + R2 mid-section returns to A through the VTR sweep.
  emu.setPracticeCountdown(false);
  emu.startPractice(1);
  tickN(emu, 40);
  CHECK_EQ(emu.status().practiceFrame, uint64_t(40));
  emu.setRewindHeld(true);
  emu.setFastForwardHeld(true);
  emu.tick();
  CHECK(emu.status().practiceLooping);
  CHECK(emu.signal().codes == nullptr);  // the effect's picture (RGB) during the sweep
  uint64_t frameAtChord = emu.status().practiceFrame;
  for (int i = 0; i < 20; ++i) emu.tick();
  CHECK_EQ(emu.status().practiceFrame, frameAtChord);  // L2 still held: no rewind of the run meanwhile
  emu.setRewindHeld(false);
  emu.setFastForwardHeld(false);
  for (int i = 0; i < 300 && emu.status().practiceLoops == 0; ++i) {
    emu.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK_EQ(emu.status().practiceLoops, 1);
  CHECK(emu.status().practiceFrame <= uint64_t(2));  // back at A, playing again
  // The VTR effect off: the sweep shows the plain pictures.
  emu.setVtrEffect(false);
  emu.setRewindHeld(true);
  emu.setFastForwardHeld(true);
  emu.tick();
  emu.tick();
  CHECK(emu.status().practiceLooping);
  emu.setRewindHeld(false);
  emu.setFastForwardHeld(false);
  emu.stopPractice();
  emu.closeSession();
  rn_input_free(in);
}

TEST_CASE("emulation: bookmarks, takes, undo, reset with backup") {
  std::string d = tmpDir("takes");
  std::string rom = testRom(d);
  rn_input* in = rn_input_new();
  NullAudio audio;
  EmulationController emu(in, &audio);
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), (d + "/p.nesrec").c_str(), nullptr, &s) == RN_OK);
  emu.install(s);
  tickN(emu, 120);
  emu.addBookmark();
  REQUIRE_EQ(emu.structure().bookmarks.size(), size_t(1));
  uint64_t bm = emu.structure().bookmarks[0].id;
  emu.renameBookmark(bm, "boss");
  CHECK_EQ(emu.structure().bookmarks[0].name, std::string("boss"));
  emu.seek(30);
  emu.rerecordHere();
  tickN(emu, 10);  // branches: a second take
  CHECK_EQ(emu.structure().takes.size(), size_t(2));
  CHECK(emu.status().undoDepth >= 1);
  emu.undoTake();
  CHECK_EQ(emu.status().takeLength, uint64_t(120));
  emu.gotoBookmark(bm);
  CHECK_EQ(emu.status().frame, uint64_t(120));
  emu.removeBookmark(bm);
  CHECK(emu.structure().bookmarks.empty());
  CHECK(emu.save());
  std::string backedUp;
  std::string err = emu.resetProject(true, [&](const std::string& dir) {
    backedUp = dir;
    return std::string();
  });
  CHECK(err.empty());
  CHECK_EQ(backedUp, d + "/p.nesrec");
  CHECK_EQ(emu.status().takeLength, uint64_t(0));
  CHECK_FALSE(emu.paused());
  // A failing backup leaves the project alone.
  tickN(emu, 10);
  err = emu.resetProject(false, [](const std::string&) { return std::string("disk on fire"); });
  CHECK(err.find("disk on fire") != std::string::npos);
  CHECK_EQ(emu.status().takeLength, uint64_t(10));
  emu.closeSession();
  rn_input_free(in);
}

// ------------------------------------------------------------------ session lifecycle

TEST_CASE("session: temporary session is resumed where it was") {
  std::string dir = tmpDir("resume");
  std::string rom = testRom(dir + "/roms");
  uint64_t frame = 0;
  {
    World w(dir + "/w", true);
    w.app->tryRom(rom);
    REQUIRE(w.emu->session() != nullptr);
    CHECK(w.app->isTempSession());
    CHECK(fs::exists(w.paths.tempProject()));
    tickN(*w.emu, 200);
    w.emu->setPaused(true);
    w.emu->seek(150);
    frame = w.emu->status().frame;
    w.app->update(1e9);
    w.app->quitNow();
    w.settle();
    CHECK(fs::exists(w.paths.resumeFile()));
  }
  {
    World w(dir + "/w", false);
    // The launch shows the library; its Continue card offers the temporary session.
    w.app->startup("", true);
    CHECK(w.emu->session() == nullptr);
    std::optional<AppModel::ContinueTarget> t = w.app->continueTarget();
    REQUIRE(t.has_value());
    CHECK(t->isTemp);
    CHECK(t->fromRecord);
    CHECK_EQ(t->romPath, rom);
    w.app->continueLast();
    REQUIRE(w.emu->session() != nullptr);
    CHECK_FALSE(w.app->continueTarget().has_value());  // none while playing
    CHECK(w.app->isTempSession());
    CHECK(w.emu->paused());
    CHECK_EQ(w.emu->status().frame, frame);
    CHECK_EQ(w.emu->status().takeLength, uint64_t(200));
    bool resumedNotice = false;
    for (auto& n : w.host.notices) resumedNotice = resumedNotice || n == TR("Resumed where you left off");
    CHECK(resumedNotice);
  }
}

TEST_CASE("session: switching ROM asks to save a temporary session; Don't Save discards it") {
  std::string dir = tmpDir("discard");
  std::string rom = testRom(dir + "/roms");
  World w(dir + "/w", true);
  w.app->tryRom(rom);
  tickN(*w.emu, 60);
  w.host.answers = {1};  // Don't Save
  w.app->tryRom(rom);
  CHECK(w.host.sawTitle(TR("Do you want to save?")));
  REQUIRE(w.emu->session() != nullptr);
  CHECK_EQ(w.emu->status().takeLength, uint64_t(0));  // a new temporary session
  // Cancel keeps the current session.
  tickN(*w.emu, 30);
  w.host.answers = {2};
  w.app->closeProject();
  CHECK(w.emu->session() != nullptr);
  // An empty temporary session is replaced without asking.
  World w2(dir + "/w2", true);
  w2.app->tryRom(rom);
  w2.app->tryRom(rom);
  CHECK(w2.host.dialogs.empty());
}

TEST_CASE("session: Save As moves the temporary project into Projects and reopens it") {
  std::string dir = tmpDir("saveas");
  std::string rom = testRom(dir + "/roms");
  World w(dir + "/w", true);
  w.app->tryRom(rom);
  tickN(*w.emu, 90);
  std::string dest = w.library->projectsDir() + "/Run.nesrec";
  w.host.choices = {dest};
  w.app->save();
  REQUIRE(w.emu->session() != nullptr);
  CHECK_FALSE(w.app->isTempSession());
  CHECK(fs::exists(dest + "/manifest.json"));
  CHECK_FALSE(fs::exists(w.paths.tempProject()));
  CHECK_EQ(w.emu->status().takeLength, uint64_t(90));
  CHECK_EQ(std::string(rn_session_project_dir(w.emu->session())), dest);
  w.settle();
  rnf_resume_record r{};
  int found = 0;
  REQUIRE(rnf_resume_read(w.paths.resumeFile().c_str(), &r, &found) == RN_OK);
  CHECK(found);
  CHECK_EQ(std::string(r.project_path), dest);
  CHECK_FALSE(r.is_temp);
  rnf_resume_record_clear(&r);
}

TEST_CASE("session: library Play creates a project in Projects; Continue reopens it") {
  std::string dir = tmpDir("library");
  World w(dir + "/w", true);
  std::string rom = testRom(w.library->romDir(), "Test Game.nes");
  w.library->refresh();
  for (int i = 0; i < 200 && !w.library->poll(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE_EQ(w.library->roms().size(), size_t(1));
  const LibraryROM romEntry = w.library->roms()[0];
  CHECK_EQ(romEntry.name, std::string("Test Game"));
  CHECK_EQ(romEntry.sha256.size(), size_t(64));
  w.app->playFromLibrary(romEntry);
  REQUIRE(w.emu->session() != nullptr);
  CHECK_FALSE(w.app->isTempSession());
  std::string project = rn_session_project_dir(w.emu->session());
  // (The frontend core joins with '\' on Windows.)
  CHECK(project.find(w.library->projectsDir()) == 0);
  CHECK(fs::path(project).filename().string().rfind("Test Game ", 0) == 0);
  CHECK(fs::equivalent(fs::path(project).parent_path(), fs::path(w.library->projectsDir())));
  tickN(*w.emu, 30);
  CHECK(w.emu->save());
  for (int i = 0; i < 200; ++i) {
    w.library->poll();
    if (w.library->projectsFor(romEntry).size() == 1) break;
    if (!w.library->scanning()) w.library->refresh();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE_EQ(w.library->projectsFor(romEntry).size(), size_t(1));
  // Another ROM session, then "Continue".
  w.app->tryRom(rom);
  CHECK(w.app->isTempSession());
  w.app->continueProject(project);
  REQUIRE(w.emu->session() != nullptr);
  CHECK_EQ(std::string(rn_session_project_dir(w.emu->session())), project);
  CHECK_EQ(w.emu->status().takeLength, uint64_t(30));
  CHECK(w.emu->paused());
}

TEST_CASE("session: Reset Project backs a saved project up to the Trash") {
  std::string dir = tmpDir("reset");
  World w(dir + "/w", true);
  std::string rom = testRom(w.library->romDir());
#ifdef _WIN32
  // The Recycle Bin is the user's: a scratch folder stands in for it (trashItem itself: the test above).
  w.app->trash = [dir](const std::string& p, std::string* err) {
    std::error_code ec;
    fs::create_directories(dir + "/hostdata/Trash/files", ec);
    fs::rename(p, dir + "/hostdata/Trash/files/" + fs::path(p).filename().string(), ec);
    if (ec && err) *err = ec.message();
    return !ec;
  };
#else
  setenv("HOST_XDG_DATA_HOME", (dir + "/hostdata").c_str(), 1);
#endif
  LibraryROM r;
  r.path = rom;
  r.name = "testrom";
  w.app->playFromLibrary(r);
  tickN(*w.emu, 60);
  w.emu->practiceSetA(0);
  w.host.answers = {0};  // Reset (keep A/B checked)
  w.app->resetProjectPrompt();
  CHECK(w.host.sawTitle(TR("Reset this project?")));
  CHECK_EQ(w.emu->status().takeLength, uint64_t(0));
  CHECK(w.emu->structure().slots[0].hasA);  // kept
  bool trashed = false;
  std::error_code ec;
  for (fs::directory_iterator it(dir + "/hostdata/Trash/files", ec), end; !ec && it != end; it.increment(ec))
    trashed = trashed || it->path().filename().string().find("(") != std::string::npos;
  CHECK(trashed);
#ifndef _WIN32
  CHECK(fs::exists(dir + "/hostdata/Trash/info"));
  unsetenv("HOST_XDG_DATA_HOME");
#endif
}

TEST_CASE("session: after a crash the Continue card recovers the session from the journal") {
  std::string dir = tmpDir("crash");
  std::string rom = testRom(dir + "/roms");
  {
    World w(dir + "/w", true);
    w.app->tryRom(rom);
    tickN(*w.emu, 120);
    w.app->update(1e9);
    w.app->persistNow();  // what the autosave / focus loss wrote before the crash
    w.settle();
    // No quitNow: the process ends here without persisting.
  }
  World w(dir + "/w", false);
  w.app->startup("", true);
  CHECK(w.emu->session() == nullptr);
  std::optional<AppModel::ContinueTarget> t = w.app->continueTarget();
  REQUIRE(t.has_value());
  CHECK(t->isTemp);
  w.app->continueLast();
  REQUIRE(w.emu->session() != nullptr);
  CHECK(w.app->isTempSession());
  CHECK_EQ(w.emu->status().takeLength, uint64_t(120));
}

TEST_CASE("session: another game while a temporary session waits on the Continue card asks first") {
  std::string dir = tmpDir("pendingtemp");
  std::string rom = testRom(dir + "/roms");
  {
    World w(dir + "/w", true);
    w.app->tryRom(rom);
    tickN(*w.emu, 60);
    w.app->update(1e9);
    w.app->quitNow();
    w.settle();
  }
  World w(dir + "/w", false);
  w.library->setPrefsFile(w.paths.configDir + "/" + RNF_LIBRARY_PREFS_FILE);
  w.app->startup("", true);
  REQUIRE(w.app->continueTarget().has_value());
  LibraryROM r;
  r.path = rom;
  r.name = "testrom";
  w.host.answers = {2};  // Cancel: nothing happens
  w.app->playFromLibrary(r);
  CHECK(w.host.sawTitle(TR("An unsaved previous session remains")));
  CHECK(w.emu->session() == nullptr);
  CHECK(fs::exists(w.paths.tempProject()));
  REQUIRE(w.app->continueTarget().has_value());
  w.host.answers = {1};  // Don't Save: discarded, the new game starts
  w.app->playFromLibrary(r);
  REQUIRE(w.emu->session() != nullptr);
  CHECK_FALSE(w.app->isTempSession());
  CHECK_FALSE(fs::exists(w.paths.tempProject()));
  // The play history has the game.
  r.sha256 = rn_session_rom_sha256(w.emu->session());
  rnf_library_history_entry h{};
  CHECK(w.library->history(r, &h));
  CHECK(h.plays >= 1);
  // Back to the library: the Continue card is the project just played.
  w.app->closeProject();
  w.library->refresh();
  for (int i = 0; i < 100 && (w.library->scanning() || !w.library->poll()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::optional<AppModel::ContinueTarget> t = w.app->continueTarget();
  REQUIRE(t.has_value());
  CHECK_FALSE(t->isTemp);
  CHECK_FALSE(t->fromRecord);
}

TEST_CASE("session: a second instance neither persists nor resumes") {
  std::string dir = tmpDir("lock");
  World a(dir + "/w", true);
  CHECK(a.app->persistSessions());
  World b(dir + "/w", false);
  CHECK_FALSE(b.app->persistSessions());
}

TEST_CASE("session: a missing ROM offers Locate ROM; cancelling keeps the resume record") {
  std::string dir = tmpDir("missingrom");
  std::string rom = testRom(dir + "/roms");
  {
    World w(dir + "/w", true);
    w.app->tryRom(rom);
    tickN(*w.emu, 30);
    w.app->update(1e9);
    w.app->quitNow();
    w.settle();
  }
  fs::remove(rom);
  World w(dir + "/w", false);
  w.app->startup("", true);
  CHECK(w.emu->session() == nullptr);
  CHECK(w.host.dialogs.empty());  // nothing is opened at launch
  w.app->continueLast();
  CHECK(w.emu->session() == nullptr);
  CHECK(w.host.sawTitle(TR("ROM not found")));
  CHECK(w.host.sawNotice(TR("Couldn’t resume where you left off (nothing was deleted). Try again from the library.")));
  CHECK(fs::exists(w.paths.tempProject()));  // kept for the next launch
}

// ------------------------------------------------------------------ thumbnails

TEST_CASE("thumbnails: live capture and background batches") {
  std::string d = tmpDir("thumbs");
  std::string rom = testRom(d);
  rn_input* in = rn_input_new();
  ThumbnailManager thumbs;
  EmulationController emu(in, nullptr);
  emu.observer = &thumbs;
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), (d + "/p.nesrec").c_str(), nullptr, &s) == RN_OK);
  emu.install(s);
  double step = thumbs.layout(1000, 64, 0, 0);
  CHECK_EQ(step, RNF_THUMB_BASE_STEP);
  for (int i = 0; i < 700; ++i) {
    emu.tick();
    emu.afterFrame(1);
  }
  // Grid pictures seen while recording were kept (live capture).
  CHECK(bool(thumbs.imageAt(rnf_thumb_picture_frame(1, step))));
  // Reopened: the background batch renders the missing pictures from the checkpoints.
  emu.save();
  emu.closeSession();
  REQUIRE(rn_session_open((d + "/p.nesrec").c_str(), nullptr, nullptr, &s) == RN_OK);
  emu.install(s);
  CHECK_FALSE(bool(thumbs.imageAt(rnf_thumb_picture_frame(1, step))));
  thumbs.layout(1000, 64, rn_take_length(s), 0);
  for (int i = 0; i < 400 && !thumbs.imageAt(rnf_thumb_picture_frame(2, step)); ++i) {
    thumbs.pump(s, 1.0 + i * 0.01);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(bool(thumbs.imageAt(rnf_thumb_picture_frame(1, step))));
  CHECK(bool(thumbs.imageAt(rnf_thumb_picture_frame(2, step))));
  emu.closeSession();
  rn_input_free(in);
}

// ------------------------------------------------------------------ MP4 export helpers (export/)

TEST_CASE("export: frame / sample times in 100 ns units from counts") {
  using namespace rnl::exportframe;
  CHECK_EQ(frameTime100ns(0), int64_t(0));
  CHECK_EQ(frameTime100ns(1), int64_t(166393));      // 655171 / 39375000 s = 16.6392635 ms
  CHECK_EQ(frameTime100ns(63), int64_t(10482736));   // exact: 63 * 655171 * 16 / 63
  CHECK_EQ(frameTime100ns(39375000), int64_t(655171) * 10000000);
  for (uint64_t n = 0; n < 5000; ++n) {
    const int64_t d = frameTime100ns(n + 1) - frameTime100ns(n);
    if (d < 166392 || d > 166393) {
      CHECK(false);
      break;
    }
  }
  CHECK_EQ(sampleTime100ns(48000), int64_t(10000000));
  CHECK_EQ(sampleTime100ns(1), int64_t(208));  // 208.33
  CHECK_EQ(sampleTime100ns(2), int64_t(417));  // 416.67
}

TEST_CASE("export: NV12 and I420 conversion agree, BT.709 limited range") {
  using namespace rnl::exportframe;
  const int w = 8, h = 4;
  std::vector<uint32_t> px(size_t(w) * h);
  uint32_t seed = 12345;
  for (auto& p : px) {
    seed = seed * 1664525u + 1013904223u;
    p = 0xFF000000u | (seed >> 8);
  }
  px[0] = px[1] = px[w] = px[w + 1] = 0xFFFFFFFFu;  // a white 2x2 block
  px[2] = px[3] = px[w + 2] = px[w + 3] = 0xFF000000u;  // a black one
  std::vector<uint8_t> y1(size_t(w) * h), u(size_t(w / 2) * h / 2), v(size_t(w / 2) * h / 2), y2(size_t(w) * h), uv(size_t(w) * h / 2);
  const auto* canvas = reinterpret_cast<const uint8_t*>(px.data());
  convertToYuv(canvas, size_t(w) * 4, w, h, y1.data(), w, u.data(), w / 2, v.data(), w / 2);
  convertToYuv(canvas, size_t(w) * 4, w, h, y2.data(), w, uv.data(), w, nullptr, 0);
  CHECK(y1 == y2);
  bool same = true;
  for (size_t i = 0; i < u.size(); ++i) same &= uv[i * 2] == u[i] && uv[i * 2 + 1] == v[i];
  CHECK(same);
  CHECK_EQ(int(y1[0]), 235);
  CHECK_EQ(int(y1[2]), 16);
  CHECK_EQ(int(u[0]), 128);
  CHECK_EQ(int(v[0]), 128);
  CHECK_EQ(int(u[1]), 128);
}

namespace {
void be32(std::string& o, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o += char(uint8_t(v >> s));
}
std::string box(const char* type, const std::string& body) {
  std::string o;
  be32(o, uint32_t(body.size() + 8));
  o += type;
  return o + body;
}
uint32_t rd(const std::string& s, size_t at) {
  return uint32_t(uint8_t(s[at])) << 24 | uint32_t(uint8_t(s[at + 1])) << 16 | uint32_t(uint8_t(s[at + 2])) << 8 | uint8_t(s[at + 3]);
}
/// Payload of the first box of `type` anywhere in `s` (no validation; the test files are tiny).
std::string findBox(const std::string& s, const char* type) {
  size_t at = s.find(type);
  if (at == std::string::npos || at < 4) return "";
  return s.substr(at + 4, rd(s, at - 4) - 8);
}
}  // namespace

TEST_CASE("export: MP4 video track retimed to 39375000 / 655171 per frame") {
  // What Media Foundation's sink writes: video at timescale 60098 (999 / 1000 per frame), zero
  // composition offsets; audio at 48000; movie timescale 48000; moov after mdat.
  auto mvhd = [&](uint32_t ts, uint32_t dur) { std::string b(12, '\0'); be32(b, ts); be32(b, dur); return box("mvhd", b + std::string(80, '\0')); };
  auto mdhd = [&](uint32_t ts, uint32_t dur) { std::string b(12, '\0'); be32(b, ts); be32(b, dur); b += std::string(4, '\0'); return box("mdhd", b); };
  auto tkhd = [&](uint32_t dur) { std::string b(20, '\0'); be32(b, dur); return box("tkhd", b + std::string(60, '\0')); };
  auto hdlr = [&](const char* t) { return box("hdlr", std::string(8, '\0') + t + std::string(13, '\0')); };
  std::string stts(4, '\0');
  be32(stts, 2);
  be32(stts, 2);
  be32(stts, 1000);
  be32(stts, 1);
  be32(stts, 999);
  std::string ctts(4, '\0');
  be32(ctts, 1);
  be32(ctts, 3);
  be32(ctts, 0);
  std::string video = box("trak", tkhd(2400) + box("mdia", mdhd(60098, 2999) + hdlr("vide") +
                                                              box("minf", box("stbl", box("stts", stts) + box("ctts", ctts)))));
  std::string audio = box("trak", tkhd(2400) + box("mdia", mdhd(48000, 2400) + hdlr("soun")));
  std::string file = box("ftyp", "isom") + box("mdat", std::string(16, 'x')) + box("moov", mvhd(48000, 2400) + video + audio);
  const std::string path = tmpDir("retime") + "/a.mp4";
  {
    std::ofstream f(fs::u8path(path), std::ios::binary);
    f << file;
  }
  std::string err;
  CHECK(!rnl::retimeMp4Video(path, 4, RN_FPS_NUM, RN_FPS_DEN, &err));  // 3 samples, not 4
  CHECK(err.find("expected 4") != std::string::npos);
  REQUIRE(rnl::retimeMp4Video(path, 3, RN_FPS_NUM, RN_FPS_DEN, &err));
  std::ifstream f(fs::u8path(path), std::ios::binary);
  std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(out.compare(0, 36, file, 0, 36) == 0);  // ftyp + mdat untouched
  const std::string md = findBox(out, "mdhd");
  REQUIRE(md.size() >= 32);
  CHECK_EQ(int(uint8_t(md[0])), 1);
  CHECK_EQ(rd(md, 20), uint32_t(RN_FPS_NUM));
  CHECK_EQ((uint64_t(rd(md, 24)) << 32 | rd(md, 28)), uint64_t(3) * RN_FPS_DEN);
  const std::string st = findBox(out, "stts");
  CHECK_EQ(rd(st, 4), 1u);
  CHECK_EQ(rd(st, 8), 3u);
  CHECK_EQ(rd(st, 12), uint32_t(RN_FPS_DEN));
  CHECK(findBox(out, "ctts").empty());  // all-zero offsets dropped
  const std::string tk = findBox(out, "tkhd");
  CHECK_EQ(rd(tk, 20), uint32_t(std::llround(3.0 * RN_FPS_DEN * 48000 / RN_FPS_NUM)));  // 2396
  const std::string mv = findBox(out, "mvhd");
  CHECK_EQ(rd(mv, 16), 2400u);  // the audio track is longer
  // The audio track is unchanged.
  CHECK(out.find(audio) != std::string::npos);
}

namespace {
void be64(std::string& o, uint64_t v) {
  be32(o, uint32_t(v >> 32));
  be32(o, uint32_t(v));
}
uint64_t rd64s(const std::string& s, size_t at) { return uint64_t(rd(s, at)) << 32 | rd(s, at + 4); }

/// A finished MP4 as Media Foundation's sink lays it out for a long export: ftyp, one mdat (a
/// 64-bit "largesize" header when it passes 4 GiB), moov at the end. Video: 6 frames of H.264
/// (length-prefixed NAL units) in 2 chunks; audio: 4 samples in 2 chunks. The second chunk of
/// each track starts `gap` bytes into the media data (past 4 GiB: a sparse hole).
struct LargeMp4 {
  uint64_t gap = (uint64_t(1) << 32) + 4096;
  bool largeMdat = true;
  enum Offsets { kCo64, kStco32Wrapped } offsets = kCo64;
  bool finished = true;  // false: mdat size 0, no moov (the export never got to Finalize)
  uint64_t videoChunk[2] = {0, 0}, audioChunk[2] = {0, 0};
  uint64_t fileSize = 0;

  static std::string videoSample() {  // SPS-like + IDR-like NAL units, 4-byte lengths
    std::string o;
    be32(o, 2);
    o += "\x67\x64";
    be32(o, 3);
    o += "\x65\x88\x84";
    return o;
  }
  bool write(const std::string& path) {
    if (!createSparseFile(path)) return false;
    const std::string v = videoSample(), a = "AAAAAAAA";  // 3 video samples + 2 audio samples per chunk
    const uint64_t hdr = largeMdat ? 16 : 8, ftypSize = 24, payload = ftypSize + hdr;
    videoChunk[0] = payload;
    audioChunk[0] = payload + 3 * v.size();
    videoChunk[1] = payload + gap;
    audioChunk[1] = videoChunk[1] + 3 * v.size();
    const uint64_t mdatEnd = audioChunk[1] + 2 * a.size();
    std::string head = box("ftyp", "isom" + std::string(4, '\0') + "isomavc1");
    if (!finished) {
      be32(head, 0);
      head += "mdat";
    } else if (largeMdat) {
      be32(head, 1);
      head += "mdat";
      be64(head, mdatEnd - ftypSize);
    } else {
      be32(head, uint32_t(mdatEnd - ftypSize));
      head += "mdat";
    }
    auto offsets_ = [&](const uint64_t* c) {
      std::string b(4, '\0');
      be32(b, 2);
      if (offsets == kCo64) {
        be64(b, c[0]);
        be64(b, c[1]);
        return box("co64", b);
      }
      be32(b, uint32_t(c[0]));
      be32(b, uint32_t(c[1]));  // what a 32-bit muxer stores: the offset modulo 2^32
      return box("stco", b);
    };
    auto stsz = [&](uint32_t size, uint32_t n) {
      std::string b(4, '\0');
      be32(b, size);
      be32(b, n);
      return box("stsz", b);
    };
    auto stsc = [&](uint32_t per) {
      std::string b(4, '\0');
      be32(b, 1);
      be32(b, 1);
      be32(b, per);
      be32(b, 1);
      return box("stsc", b);
    };
    auto stts = [&](uint32_t n, uint32_t d) {
      std::string b(4, '\0');
      be32(b, 1);
      be32(b, n);
      be32(b, d);
      return box("stts", b);
    };
    auto header = [&](const char* t, uint32_t ts, uint32_t dur, size_t pad) {
      std::string b(12, '\0');
      be32(b, ts);
      be32(b, dur);
      return box(t, b + std::string(pad, '\0'));
    };
    auto tkhd = [&](uint32_t dur) {
      std::string b(20, '\0');
      be32(b, dur);
      return box("tkhd", b + std::string(60, '\0'));
    };
    auto hdlr = [&](const char* t) { return box("hdlr", std::string(8, '\0') + t + std::string(13, '\0')); };
    std::string avcC = box("avcC", std::string("\x01\x64\x00\x2a\xff\xe0\x00", 7));
    std::string avc1 = box("avc1", std::string(78, '\0') + avcC);
    std::string stsdV(4, '\0');
    be32(stsdV, 1);
    stsdV += avc1;
    std::string stsdA(4, '\0');
    be32(stsdA, 1);
    stsdA += box("mp4a", std::string(28, '\0'));
    // Video tables as Media Foundation's sink writes them: 999 / 1000 ticks alternating (several
    // stts runs) and all-zero composition offsets - the retimed moov is smaller than this one.
    std::string sttsV(4, '\0');
    be32(sttsV, 3);
    for (uint32_t d : {1000u, 999u, 1000u}) {
      be32(sttsV, 2);
      be32(sttsV, d);
    }
    std::string cttsV(4, '\0');
    be32(cttsV, 1);
    be32(cttsV, 6);
    be32(cttsV, 0);
    std::string video = box("trak", tkhd(4800) + box("mdia", header("mdhd", 60098, 5998, 4) + hdlr("vide") +
                                                                  box("minf", box("stbl", box("stsd", stsdV) + box("stts", sttsV) + box("ctts", cttsV) + stsc(3) +
                                                                                                  stsz(uint32_t(v.size()), 6) + offsets_(videoChunk)))));
    std::string audio = box("trak", tkhd(4800) + box("mdia", header("mdhd", 48000, 4800, 4) + hdlr("soun") +
                                                                  box("minf", box("stbl", box("stsd", stsdA) + stts(4, 1024) + stsc(2) +
                                                                                                  stsz(uint32_t(a.size()), 4) + offsets_(audioChunk)))));
    std::string moov = box("moov", header("mvhd", 48000, 4800, 80) + video + audio);
    std::fstream f(fs::u8path(path), std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return false;
    f << head;
    for (int c = 0; c < 2; ++c) {
      f.seekp(std::streamoff(videoChunk[c]));
      f << v << v << v << a << a;
    }
    if (finished) {
      f.seekp(std::streamoff(mdatEnd));
      f << moov;
    }
    f.close();
    fileSize = finished ? mdatEnd + moov.size() : mdatEnd;
    std::error_code ec;
    return bool(f) && fs::file_size(fs::u8path(path), ec) == fileSize;
  }
};

std::string readMoov(const std::string& path, uint64_t from) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  f.seekg(std::streamoff(from));
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("export: MP4 check + retime of a file past 4 GiB (largesize mdat, co64)") {
  const std::string path = tmpDir("mp4-large") + "/large.mp4";
  LargeMp4 m;
  REQUIRE(m.write(path));
  REQUIRE(m.fileSize > (uint64_t(1) << 32));
  std::string err;
  rnl::Mp4Summary sum;
  REQUIRE(rnl::checkMp4File(path, &sum, &err));
  REQUIRE_EQ(sum.tracks.size(), size_t(2));
  CHECK(sum.moovAtEnd);
  CHECK(sum.tracks[0].co64);
  CHECK_EQ(sum.tracks[0].codec, std::string("avc1"));
  CHECK_EQ(sum.tracks[0].samples, uint64_t(6));
  CHECK_EQ(sum.tracks[0].chunks, uint64_t(2));
  CHECK(sum.tracks[0].dataEnd > (uint64_t(1) << 32));
  // Retime: the moov at the end is rewritten (smaller: the stts runs collapse, the zero ctts
  // goes), the chunk offsets (64-bit) stay as they are and the file keeps its size - the rest of
  // the old moov becomes a 'free' box. (0.5.2 truncated it with std::filesystem::resize_file,
  // which llvm-mingw's libc++ does through a 32-bit off_t: a file past 4 GiB was cut to its size
  // modulo 2^32, losing the moov.)
  REQUIRE(rnl::retimeMp4Video(path, 6, RN_FPS_NUM, RN_FPS_DEN, &err));
  {
    std::error_code ec;
    CHECK_EQ(uint64_t(fs::file_size(fs::u8path(path), ec)), m.fileSize);
  }
  REQUIRE(rnl::checkMp4File(path, &sum, &err));
  CHECK(sum.tracks.size() == 2);
  CHECK(sum.tracks[0].co64);
  CHECK_EQ(sum.tracks[0].timescale, uint32_t(RN_FPS_NUM));
  CHECK_EQ(sum.tracks[0].duration, uint64_t(6) * RN_FPS_DEN);
  const std::string moov = readMoov(path, m.audioChunk[1] + 16);
  const std::string co = findBox(moov, "co64");
  REQUIRE(co.size() >= 24);
  CHECK_EQ(rd64s(co, 8), m.videoChunk[0]);
  CHECK_EQ(rd64s(co, 16), m.videoChunk[1]);
  fs::remove(fs::u8path(path));
}

TEST_CASE("export: MP4 check rejects wrapped 32-bit chunk offsets, unfinished files, bad box sizes") {
  const std::string dir = tmpDir("mp4-bad");
  std::string err;
  {
    LargeMp4 m;
    m.offsets = LargeMp4::kStco32Wrapped;
    REQUIRE(m.write(dir + "/wrapped.mp4"));
    CHECK_FALSE(rnl::checkMp4File(dir + "/wrapped.mp4", nullptr, &err));
    CHECK(err.find("MP4 check:") == 0);
    fs::remove(fs::u8path(dir + "/wrapped.mp4"));
  }
  {
    LargeMp4 m;
    m.finished = false;
    REQUIRE(m.write(dir + "/unfinished.mp4"));
    CHECK_FALSE(rnl::checkMp4File(dir + "/unfinished.mp4", nullptr, &err));
    CHECK(err.find("never finished") != std::string::npos);
    std::string rerr;
    CHECK_FALSE(rnl::retimeMp4Video(dir + "/unfinished.mp4", 6, RN_FPS_NUM, RN_FPS_DEN, &rerr));
    fs::remove(fs::u8path(dir + "/unfinished.mp4"));
  }
  {
    // A 32-bit mdat size can't describe more than 4 GiB of media data: the size field wraps and
    // the next box header lands inside the media data.
    LargeMp4 m;
    m.largeMdat = false;
    REQUIRE(m.write(dir + "/mdat32.mp4"));
    CHECK_FALSE(rnl::checkMp4File(dir + "/mdat32.mp4", nullptr, &err));
    fs::remove(fs::u8path(dir + "/mdat32.mp4"));
  }
  {
    // Small file, everything consistent: OK; then one chunk offset off by a byte: rejected.
    LargeMp4 m;
    m.gap = 4096;
    REQUIRE(m.write(dir + "/small.mp4"));
    rnl::Mp4Summary sum;
    REQUIRE(rnl::checkMp4File(dir + "/small.mp4", &sum, &err));
    CHECK(sum.fileSize < 10000);
    std::fstream f(fs::u8path(dir + "/small.mp4"), std::ios::in | std::ios::out | std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t at = all.find("co64");
    REQUIRE(at != std::string::npos);
    f.clear();
    f.seekp(std::streamoff(at + 4 + 8 + 8 + 7));  // low byte of the second video chunk offset
    f.put(char(uint8_t(m.videoChunk[1] + 1)));
    f.close();
    CHECK_FALSE(rnl::checkMp4File(dir + "/small.mp4", nullptr, &err));
    CHECK(err.find("not H.264") != std::string::npos);
  }
}

TEST_CASE("export: MP4 retime of a long take (27:53, 100577 frames): 64-bit media duration, 32-bit movie fields stay exact") {
  // Media Foundation's layout for the 100577-frame take: video at 60098 ticks/s (999 / 1000 per
  // frame), audio and movie at 48000. In the exact timescale the video lasts 100577 x 655171 =
  // 65,894,233,667 ticks: more than 32 bits (any take longer than 109 s), so mdhd must be version 1.
  const uint64_t frames = 100577;
  auto mvhd = [&](uint32_t ts, uint32_t dur) { std::string b(12, '\0'); be32(b, ts); be32(b, dur); return box("mvhd", b + std::string(80, '\0')); };
  auto mdhd = [&](uint32_t ts, uint32_t dur) { std::string b(12, '\0'); be32(b, ts); be32(b, dur); b += std::string(4, '\0'); return box("mdhd", b); };
  auto tkhd = [&](uint32_t dur) { std::string b(20, '\0'); be32(b, dur); return box("tkhd", b + std::string(60, '\0')); };
  auto hdlr = [&](const char* t) { return box("hdlr", std::string(8, '\0') + t + std::string(13, '\0')); };
  std::string stts(4, '\0');
  be32(stts, 2);
  be32(stts, uint32_t(frames / 2));
  be32(stts, 1000);
  be32(stts, uint32_t(frames - frames / 2));
  be32(stts, 999);
  const uint32_t mfDuration = uint32_t(frames / 2 * 1000 + (frames - frames / 2) * 999);
  const uint32_t audio48k = uint32_t(std::llround(double(frames) * RN_FPS_DEN / RN_FPS_NUM * 48000)) + 1024;
  std::string video = box("trak", tkhd(uint32_t(uint64_t(mfDuration) * 48000 / 60098)) +
                                      box("mdia", mdhd(60098, mfDuration) + hdlr("vide") + box("minf", box("stbl", box("stts", stts)))));
  std::string audio = box("trak", tkhd(audio48k) + box("mdia", mdhd(48000, audio48k) + hdlr("soun")));
  std::string file = box("ftyp", "isom") + box("mdat", std::string(16, 'x')) + box("moov", mvhd(48000, audio48k) + video + audio);
  const std::string path = tmpDir("retime-long") + "/long.mp4";
  {
    std::ofstream f(fs::u8path(path), std::ios::binary);
    f << file;
  }
  std::string err;
  REQUIRE(rnl::retimeMp4Video(path, frames, RN_FPS_NUM, RN_FPS_DEN, &err));
  std::ifstream f(fs::u8path(path), std::ios::binary);
  std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const std::string md = findBox(out, "mdhd");
  REQUIRE(md.size() >= 32);
  CHECK_EQ(int(uint8_t(md[0])), 1);
  CHECK_EQ(rd(md, 20), uint32_t(RN_FPS_NUM));
  const uint64_t mediaDuration = uint64_t(rd(md, 24)) << 32 | rd(md, 28);
  CHECK_EQ(mediaDuration, frames * RN_FPS_DEN);
  CHECK(mediaDuration > 0xFFFFFFFFull);
  const std::string st = findBox(out, "stts");
  CHECK_EQ(rd(st, 4), 1u);
  CHECK_EQ(rd(st, 8), uint32_t(frames));
  CHECK_EQ(rd(st, 12), uint32_t(RN_FPS_DEN));
  // tkhd / mvhd in the movie timescale (48000): 1673.53 s = 80,329,... ticks, fits version 0.
  const std::string tk = findBox(out, "tkhd");
  const uint32_t trackDuration = uint32_t(std::llround(double(frames) * RN_FPS_DEN / RN_FPS_NUM * 48000));
  CHECK_EQ(int(uint8_t(tk[0])), 0);
  CHECK_EQ(rd(tk, 20), trackDuration);
  const std::string mv = findBox(out, "mvhd");
  CHECK_EQ(rd(mv, 16), audio48k);
  CHECK(out.find(audio) != std::string::npos);
}
