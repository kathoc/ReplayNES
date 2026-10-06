// Linux frontend logic (no SDL / Vulkan): settings, freedesktop trash, file chooser names,
// library search, the 3:2 cadence, manifest reading, PNG writer, and - with the engine's test
// ROM - the emulation controller (transport, practice, timeline A/B), the session lifecycle
// (temporary session, resume, Save As, prompts, Reset Project backup, lock), library scanning
// and filmstrip thumbnails.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <unistd.h>

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
#include "dialogs.h"
#include "emulation.h"
#include "l10n.h"
#include "library.h"
#include "manifest.h"
#include "png_writer.h"
#include "settings.h"
#include "support/rn_test.h"
#include "thumbnails.h"
#include "trash.h"
#include "ui_logic.h"
#include "update_model.h"

namespace fs = std::filesystem;
using namespace rnl;

namespace {

std::string tmpDir(const std::string& name) {
  std::string d = std::string(RNL_TEST_TMP) + "/" + name + "-" + std::to_string(::getpid());
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
  void notice(const std::string& t) override { notices.push_back(t); }
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

TEST_CASE("controller conventions: B / Menu / View pages") {
  using P = MenuPage;
  using C = MenuCommand;
  // In a session: B and Menu on the hub resume play; on a page they return to the hub.
  CHECK(menuTransition(P::playback, C::back, true, P::playback).closeAndResume);
  CHECK(menuTransition(P::playback, C::menuButton, true, P::playback).closeAndResume);
  for (P p : {P::takes, P::bookmarks, P::practice, P::settings, P::guide}) {
    MenuTransition t = menuTransition(p, C::back, true, P::playback);
    CHECK(t.page == P::playback);
    CHECK_FALSE(t.closeAndResume);
    CHECK(menuTransition(p, C::menuButton, true, P::playback).page == P::playback);
  }
  // View toggles the guide and back to where it was opened.
  CHECK(menuTransition(P::settings, C::viewButton, true, P::playback).page == P::guide);
  CHECK(menuTransition(P::guide, C::viewButton, true, P::settings).page == P::settings);
  CHECK(menuTransition(P::guide, C::viewButton, true, P::guide).page == P::playback);
  // Start screen: Menu = Library <-> Settings, B = back to the Library, never "resume".
  CHECK(menuTransition(P::library, C::menuButton, false, P::library).page == P::settings);
  CHECK(menuTransition(P::settings, C::menuButton, false, P::library).page == P::library);
  CHECK(menuTransition(P::settings, C::back, false, P::library).page == P::library);
  CHECK_FALSE(menuTransition(P::library, C::back, false, P::library).closeAndResume);
  CHECK(menuTransition(P::guide, C::viewButton, false, P::playback).page == P::library);
  // L1 / R1 on pages without tabs.
  CHECK(cyclePage(P::practice, 1) == P::takes);
  CHECK(cyclePage(P::practice, -1) == P::guide);
  CHECK(cyclePage(P::guide, 1) == P::practice);
  CHECK(cyclePage(P::settings, 1) == P::settings);
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
    w.app->startup("", true);
    REQUIRE(w.emu->session() != nullptr);
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
  CHECK(project.find(w.library->projectsDir() + "/Test Game ") == 0);
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
  setenv("HOST_XDG_DATA_HOME", (dir + "/hostdata").c_str(), 1);
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
  CHECK(fs::exists(dir + "/hostdata/Trash/info"));
  unsetenv("HOST_XDG_DATA_HOME");
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
  CHECK(w.host.sawTitle(TR("ROM not found")));
  CHECK(w.host.sawTitle(TR("Couldn’t resume where you left off")));
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
