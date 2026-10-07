// ReplayNES for Linux / Steam Deck: SDL3 window + gamepads + audio, a Vulkan presenter, the
// Dear ImGui UI (ui.h), display-locked frame loop on the engine C API and the shared frontend
// core (frontend/include/replaynes/frontend.h).
//
// Frame loop (one iteration per emulated frame, docs/FRAME_PACING.md adapted to Vulkan):
//   1. wait until the previous present is on screen (VK_KHR_present_wait) -> vblank timestamp
//   2. DisplayScheduler picks the vblank the next frame is aimed at (cadence: 60 Hz every
//      refresh, 90 Hz alternating 2/1, 120 Hz every 2nd); rnf_input_deadline gives the lead
//   3. sleep until target - lead, pump SDL events, sample input, emulate one frame
//      (EmulationController), push audio (DRC), build the UI, draw, submit, present (FIFO)
//   4. autosave, resume record, thumbnails, library results only in the slack before the next
//      sample
// Logical time is the frame index: host timing decides only when rn_step is called.
// Without present_wait the loop runs throttled by FIFO alone (no JIT sampling; reported).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <sys/prctl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app_model.h"
#include "audio_out.h"
#include "display_scheduler.h"
#include "emulation.h"
#include "host_clock.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "input_router.h"
#include "l10n.h"
#include "pad_nav.h"
#include "library.h"
#include "paths.h"
#include "perf_stats.h"
#include "replaynes/frontend.h"
#include "replaynes/replaynes.h"
#include "script.h"
#include "settings.h"
#include "steam_shortcut.h"
#include "thumbnails.h"
#include "ui.h"
#include "update_service.h"
#include "vk_renderer.h"

using namespace rnl;

namespace {

struct Options {
  std::string rom;
  double perfSeconds = 0;
  double warmup = 8;
  std::string statsLog, frameLog, sessionRoot, libraryRoot, script, label = "run";
  int fullscreen = -1;  // -1 = auto (gamescope: on)
  bool injectInput = false;
  bool resume = true;
  int flash = -1;  // -1 = the saved setting
  // Measurement overrides (not saved): CRT display on, its knobs, integer scaling.
  bool crt = false;
  int crtMaxWidth = 1600;
  bool crtAdaptive = true;
  bool crtBuildAhead = true;
  int integerScale = -1;  // -1 = the saved setting
  std::vector<std::string> argv;  // as started (restart into an update)
};

void usage() {
  std::printf(
      "replaynes-linux [options]\n"
      "  --rom PATH            play PATH without a project (temporary session)\n"
      "  --no-resume           do not reopen the last session\n"
      "  --fullscreen | --windowed\n"
      "  --perf-seconds N      measure N seconds after --warmup S (default 8), then quit\n"
      "  --stats-log FILE      append the JSON summary to FILE\n"
      "  --frame-log FILE      per-frame CSV\n"
      "  --session-root DIR    session folder (perf runs use a scratch folder)\n"
      "  --library-root DIR    library folder (default ~/Documents/ReplayNES)\n"
      "  --inject-input        press B every ~0.25 s from another thread (event -> screen latency)\n"
      "  --label NAME          label of the summary\n"
      "  --flash 0-3           flash reduction level for this run (off / low / standard / high)\n"
      "  --script \"CMD; ...\"   scripted UI steps (see apps/linux/src/script.h)\n"
      "  --crt                 CRT display on for this run (Settings -> Display -> CRT Display saves it)\n"
      "  --crt-max-width N     tube width cap (default 1600); --crt-fixed: no adaptive resolution;\n"
      "  --crt-no-build-ahead  never build CRT pictures one frame ahead\n"
      "  --integer-scale 0|1   integer scaling for this run (0 = fill the screen)\n"
      "  --version             print the version and exit\n"
      "  --add-to-steam        add ReplayNES to the Steam library with its artwork, then exit (close Steam first);\n"
      "                        add --dry-run to only show what would change (also --force, --steam-userdata DIR, --steam-artwork DIR)\n"
      "Environment: REPLAYNES_LANG=ja|en overrides the system language.\n");
}

bool parseArgs(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](std::string* v) {
      if (i + 1 >= argc) return false;
      *v = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--rom" && next(&v)) o->rom = v;
    else if (a == "--perf-seconds" && next(&v)) o->perfSeconds = std::atof(v.c_str());
    else if (a == "--warmup" && next(&v)) o->warmup = std::atof(v.c_str());
    else if (a == "--stats-log" && next(&v)) o->statsLog = v;
    else if (a == "--frame-log" && next(&v)) o->frameLog = v;
    else if (a == "--session-root" && next(&v)) o->sessionRoot = v;
    else if (a == "--library-root" && next(&v)) o->libraryRoot = v;
    else if (a == "--script" && next(&v)) o->script = v;
    else if (a == "--label" && next(&v)) o->label = v;
    else if (a == "--flash" && next(&v)) o->flash = std::atoi(v.c_str());
    else if (a == "--crt") o->crt = true;
    else if (a == "--integer-scale" && next(&v)) o->integerScale = std::atoi(v.c_str()) != 0;
    else if (a == "--crt-max-width" && next(&v)) o->crtMaxWidth = std::atoi(v.c_str());
    else if (a == "--crt-fixed") o->crtAdaptive = false;
    else if (a == "--crt-no-build-ahead") o->crtBuildAhead = false;
    else if (a == "--fullscreen") o->fullscreen = 1;
    else if (a == "--windowed") o->fullscreen = 0;
    else if (a == "--inject-input") o->injectInput = true;
    else if (a == "--resume") o->resume = true;  // the default since resume.json
    else if (a == "--no-resume") o->resume = false;
    else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return false; }
  }
  return true;
}

bool underGamescope() { return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") || std::getenv("SteamGamepadUI"); }

std::vector<std::string> preferredLanguages() {
  std::vector<std::string> out;
  int n = 0;
  if (SDL_Locale** locs = SDL_GetPreferredLocales(&n)) {
    for (int i = 0; i < n; ++i)
      if (locs[i] && locs[i]->language) out.push_back(locs[i]->language);
    SDL_free(locs);
  }
  return out;
}

// ---------------------------------------------------------------- the app
class App {
 public:
  int run(const Options& opt);

 private:
  void handleEvent(const SDL_Event& e, double now);
  void applyFullscreen(bool on);
  void applySettings();
  void updateNavigation();

  Options opt_;
  Paths paths_;
  Settings settings_;
  SDL_Window* window_ = nullptr;
  VkRenderer vr_;
  AudioOut audio_;
  bool audioOK_ = false;
  std::unique_ptr<InputRouter> input_;
  std::unique_ptr<EmulationController> emu_;
  std::unique_ptr<ThumbnailManager> thumbs_;
  std::unique_ptr<LibraryModel> library_;
  std::unique_ptr<UI> ui_;
  std::unique_ptr<AppModel> app_;
  std::unique_ptr<UpdateService> updates_;
  bool restart_ = false;  // quit, then start the newest installed version (an update)
  PadNavFeed padNav_;
  DisplayScheduler sched_;
  rnf_input_deadline* deadline_ = nullptr;
  rnf_input_deadline* cpuWork_ = nullptr;  // CPU part of the work only (CRT build-ahead budget)
  double lastPictureSample_ = 0;
  bool running_ = true;
  bool fullscreen_ = false;
  int lastHeight_ = 0;
  float lastUIScale_ = 0;
  double pickedEvent_ = 0;
  std::vector<double> recentLatency_;
  size_t recentHead_ = 0;
};

void App::applyFullscreen(bool on) {
  fullscreen_ = on;
  SDL_SetWindowFullscreen(window_, on);
}

void App::applySettings() {
  emu_->pauseAfterRewind = settings_.pauseAfterRewind;
  emu_->autosaveInterval = settings_.autosaveInterval;
  emu_->setFlashLevel(rn_flash_level(settings_.flash));
  audio_.setVolume(settings_.volume);
  DisplayPostProcess pp = vr_.postProcess();
  pp.crt = settings_.crt || opt_.crt;
  pp.crtSettings = ui_->crtSettings();
  pp.maxTubeWidth = opt_.crtMaxWidth;
  pp.adaptiveResolution = opt_.crtAdaptive;
  pp.allowBuildAhead = opt_.crtBuildAhead;
  vr_.setPostProcess(pp);
}

/// ImGui navigates with the gamepad / keyboard only while the UI has the controls (menu, hub,
/// library, dialogs); then nothing reaches the game. ImGui never polls the pads itself (its
/// per-frame polling would wait for the joystick lock while the pad thread is in a slow HIDAPI
/// call): PadNavFeed passes it the SDL gamepad events.
void App::updateNavigation() {
  bool inter = ui_->interactive();
  input_->setUIMode(inter);
  ImGuiIO& io = ImGui::GetIO();
  if (inter) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  else io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
}

void App::handleEvent(const SDL_Event& e, double now) {
  // A text field with an on-screen keyboard takes the controller and taps on the keyboard
  // (ui_osk.cpp); PadNavFeed still follows the pads (it feeds ImGui nothing meanwhile).
  bool textEntry = ui_->textEntryEvent(e, now);
  if (!textEntry) ImGui_ImplSDL3_ProcessEvent(&e);
  padNav_.setSuppressed(ui_->textEntryOwnsPad());
  padNav_.handle(e);
  if (textEntry) return;
  switch (e.type) {  // which input activated a text field (Auto: no on-screen keyboard for a hardware keyboard)
    case SDL_EVENT_KEY_DOWN:
      if (!e.key.repeat) ui_->keyboardOrMouseUsed();
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e.button.which == SDL_TOUCH_MOUSEID) ui_->touchUsed();
      else ui_->keyboardOrMouseUsed();
      break;
    case SDL_EVENT_FINGER_DOWN: ui_->touchUsed(); break;
    default: break;
  }
  // SDL event timestamps (SDL_GetTicksNS) -> our CLOCK_MONOTONIC seconds.
  double evTime = now - double(SDL_GetTicksNS() - e.common.timestamp) * 1e-9;
  switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      // Window closed / Steam closing the game (SIGTERM): persist without asking.
      app_->quitNow();
      running_ = false;
      return;
    case SDL_EVENT_WINDOW_FOCUS_LOST: app_->persistNow(); break;
    // A click / tap while playing shows the dock for a moment (pointer motion alone does not:
    // compositors send motion events of their own).
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_FINGER_DOWN: ui_->pointerMoved(now); break;
    case SDL_EVENT_KEY_DOWN:
      if (!e.key.repeat && e.key.scancode == SDL_SCANCODE_F11) { applyFullscreen(!fullscreen_); return; }
      if (!e.key.repeat && e.key.scancode == SDL_SCANCODE_F3) {
        settings_.showStats = !settings_.showStats;
        settings_.save(paths_.settingsFile());
        return;
      }
      break;
    default: break;
  }
  input_->handleEvent(e, evTime, ui_->wantsKeyboard());
}

int App::run(const Options& opt) {
  opt_ = opt;
  paths_ = Paths::standard();
  if (!opt.sessionRoot.empty()) paths_.sessionRoot = opt.sessionRoot;
  if (!opt.libraryRoot.empty()) paths_.setLibraryRoot(opt.libraryRoot);
  paths_.ensure();
  settings_.load(paths_.settingsFile());
  if (opt.flash >= 0 && opt.flash <= 3) settings_.flash = opt.flash;
  if (opt.integerScale >= 0) settings_.integerScale = opt.integerScale != 0;
  std::fprintf(stderr, "ReplayNES %s\n", RNL_APP_VERSION);
  prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);  // precise wake-ups for the just-in-time sample
  const bool perfMode = opt.perfSeconds > 0;

  SDL_SetAppMetadata("ReplayNES", RNL_APP_VERSION, "io.github.replaynes.ReplayNES");
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
  // Controllers are updated on their own thread (below), not by the frame loop's event pump: the
  // Steam Deck HIDAPI driver blocks ~8 ms every few seconds (lizard-mode feature reports), which
  // would land on the input sample. Gamepad events still arrive through the event queue.
  SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  // Language: Japanese iff the first preferred language is Japanese (before any other thread
  // reads localized strings).
  std::string lang = chooseUILanguage(preferredLanguages(), std::getenv("REPLAYNES_LANG"));
  rnf_l10n_set_language(lang.c_str());

  bool fullscreen = opt.fullscreen == 1 || (opt.fullscreen == -1 && underGamescope());
  window_ = SDL_CreateWindow("ReplayNES", 1280, 800, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window_) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
  if (fullscreen) applyFullscreen(true);
  std::string err;
  if (!vr_.init(window_, &err)) { std::fprintf(stderr, "Vulkan: %s\n", err.c_str()); return 1; }
  std::fprintf(stderr, "video: %s, %s\n", SDL_GetCurrentVideoDriver(), vr_.description().c_str());
  if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_)); m && m->refresh_rate > 0) {
    sched_.seedRefresh(1.0 / m->refresh_rate);
    std::fprintf(stderr, "display mode: %dx%d @ %.3f Hz\n", m->w, m->h, m->refresh_rate);
  }
  audioOK_ = audio_.open(&err);
  if (audioOK_) std::fprintf(stderr, "audio: %s\n", audio_.deviceName().c_str());
  else std::fprintf(stderr, "%s (continuing without sound)\n", err.c_str());

  // Model objects.
  input_ = std::make_unique<InputRouter>(paths_.bindingsFile(), &settings_);
  input_->load();
  emu_ = std::make_unique<EmulationController>(input_->input(), audioOK_ ? &audio_ : nullptr);
  thumbs_ = std::make_unique<ThumbnailManager>();
  library_ = std::make_unique<LibraryModel>(paths_.libraryRoot);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigNavCaptureKeyboard = true;
  // In-app updates (Flatpak portal; its own thread). Not for measurement runs.
  updates_ = std::make_unique<UpdateService>();
  if (!perfMode) updates_->start(settings_.checkForUpdates);
  UI::Deps deps{nullptr, emu_.get(), library_.get(), input_.get(), thumbs_.get(), &vr_, &settings_, window_, updates_.get()};
  // The UI is the app model's dialog host and shows its state.
  ui_ = std::make_unique<UI>(deps);
  app_ = std::make_unique<AppModel>(paths_, emu_.get(), library_.get(), ui_.get());
  ui_->setApp(app_.get());
  if (!ui_->loadFonts(lang == "ja")) {
    std::fprintf(stderr, "no Japanese font found: using English\n");
    rnf_l10n_set_language("en");
  }
  int ww = 0, wh = 0;
  SDL_GetWindowSizeInPixels(window_, &ww, &wh);
  ui_->updateScale(wh);
  lastHeight_ = wh;
  lastUIScale_ = settings_.uiScale;
  ImGui_ImplSDL3_InitForVulkan(window_);
  ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
  vr_.initImGui();

  // Wiring.
  emu_->observer = thumbs_.get();
  emu_->onNotice = [this](const std::string& s) { ui_->notice(s); };
  emu_->onError = [this](const std::string& t, const std::string& m) {
    Dialog d;
    d.title = t;
    d.message = m;
    ui_->showDialog(std::move(d));
  };
  emu_->onPausedChanged = [this](bool p) { input_->setPausedStepMode(p); };
  emu_->pressSequence = [this] { return input_->pressSequence(); };
  input_->onMenuButton = [this] { ui_->toggleMenu(); };
  input_->onPausedStep = [this](int dir, bool down) { emu_->pausedStep(dir, down); };
  input_->onDisconnect = [this](const std::string& name) {
    if (!emu_->session()) return;
    emu_->setPaused(true);
    ui_->notice(TRF("Paused because %@ was disconnected", {name}));
  };
  ui_->onQuit = [this] { app_->requestQuit([this] { running_ = false; }); };
  ui_->onRestart = [this] {
    app_->requestQuit([this] {
      restart_ = true;
      running_ = false;
    });
  };
  ui_->onSettingsChanged = [this] {
    settings_.save(paths_.settingsFile());
    applySettings();
    updates_->setAutoCheck(settings_.checkForUpdates);
  };
  ui_->onFullscreen = [this](bool on) { applyFullscreen(on); };
  ui_->isFullscreen = [this] { return fullscreen_; };
  deadline_ = rnf_input_deadline_new();
  cpuWork_ = rnf_input_deadline_new();
  ui_->stats = [this] {
    StatsInfo s;
    double r = sched_.refresh();
    s.refreshHz = r > 0 ? 1 / r : 0;
    s.cadence = sched_.cadence().name();
    s.leadMs = rnf_input_deadline_lead(deadline_) * 1000;
    double lat = 0;
    for (double v : recentLatency_) lat += v;
    if (!recentLatency_.empty()) lat /= double(recentLatency_.size());
    s.latencyMs = lat * 1000;
    s.audioRatio = audio_.ratio();
    s.audioFillMs = audio_.fillMs();
    s.underruns = audio_.underruns();
    s.presentWait = vr_.presentWait();
    PostProcessStatus ps = vr_.postProcessStatus();
    if (vr_.postProcess().crt && ps.crtShown) {
      char b[160];
      std::snprintf(b, sizeof b, "CRT %dx%d (x%.2f) %s  GPU p50 %.2f / p90 %.2f ms%s", ps.tubeWidth, ps.tubeHeight, ps.scale,
                    ps.usedCodes ? "RF" : "RGB", ps.gpuMsP50, ps.gpuMsP90, ps.buildAhead ? "  built ahead" : "");
      s.crt = b;
    }
    return s;
  };
  applySettings();
  if (opt.injectInput && perfMode) rn_input_bind(input_->input(), "inject:b", "p1.b");  // not persisted

  // Library + session.
  library_->ensureFolders();
  library_->refresh();
  app_->setupSessionPersistence(true);
  if (!app_->persistSessions()) ui_->notice(TR("Another ReplayNES is running: this one doesn’t resume or keep sessions"));
  app_->startup(opt.rom, opt.resume && opt.rom.empty());
  std::unique_ptr<Script> script;
  if (!opt.script.empty()) {
    script = std::make_unique<Script>(opt.script, ui_.get(), emu_.get(), &vr_, app_.get());
    script->onQuit = [this] {
      app_->quitNow();
      running_ = false;
    };
    script->onSettingsPage = [this](int p) { ui_->setSettingsPage(p); };
    script->padId = [this]() -> SDL_JoystickID { return input_->pad(0).pad ? input_->pad(0).id : SDL_JoystickID(0x7fff0000); };
    script->onCrt = [this](bool on) {
      settings_.crt = on;
      applySettings();
    };
  }

  std::atomic<bool> polling{true};
  std::thread padThread([&] {
    double t = nowSeconds();
    while (polling.load()) {
      SDL_UpdateJoysticks();
      t = std::max(t + 0.001, nowSeconds() - 0.001);  // ~1 kHz
      sleepUntil(t);
    }
  });

  // Perf mode: optional input injection from another thread (unsynchronised with the display).
  std::atomic<bool> injecting{opt.injectInput && perfMode};
  std::thread injector;
  if (injecting) {
    injector = std::thread([&] {
      bool down = false;
      double t = nowSeconds() + 3.0;
      while (injecting.load()) {
        sleepUntil(t);
        down = !down;
        rn_input_set_pressed(input_->input(), "inject:b", down);
        if (down) input_->noteInjectedEvent(nowSeconds());
        t += 0.2503;  // not a multiple of the frame period: presses land at every phase
      }
    });
  }

  PerfStats perf;
  RunInfo info;
  info.label = opt.label;
  const double start = nowSeconds();
  std::deque<FrameRecord> inflight;  // presented, not yet confirmed on screen
  uint64_t seq = 0;
  double lastRate = 0;
  double lastSample = 0;
  recentLatency_.assign(60, 0.0);

  std::map<int, int> waitFailures;  // VkResult -> count (diagnostics)
  double missGuard = 0, lastTargetIssued = 0;
  auto settle = [&](FrameRecord& rec, const PresentDone* d) {
    if (d && !d->ok) waitFailures[d->result] += 1;
    if (!d) waitFailures[1000] += 1;  // no result for this id
    if (d && d->ok) {
      rec.displayed = d->time;
      // One lead increase per episode: pictures already in flight when a miss is seen would
      // miss for the same reason.
      if (sched_.observeDisplayed(d->time, rec.target) && rec.target > missGuard) {
        rnf_input_deadline_observe_miss(deadline_);
        missGuard = lastTargetIssued;
      }
      if (rec.emulated)
        recentLatency_[recentHead_++ % recentLatency_.size()] = d->time - (rec.pictureSample > 0 ? rec.pictureSample : rec.sample);
    }
    if (perfMode) perf.add(rec);
  };

  while (running_) {
    double wake = nowSeconds();
    // 1. Pictures confirmed on screen since the last frame (in present order).
    for (const PresentDone& d : vr_.takeCompleted()) {
      while (!inflight.empty() && inflight.front().presentId < d.id) { settle(inflight.front(), nullptr); inflight.pop_front(); }
      if (!inflight.empty() && inflight.front().presentId == d.id) { settle(inflight.front(), &d); inflight.pop_front(); }
    }
    while (inflight.size() > 16) { settle(inflight.front(), nullptr); inflight.pop_front(); }
    // 2. Target vblank and input lead for the next frame.
    double r = sched_.refresh();
    if (r > 0) {
      // The lead also absorbs the compositor's latch point before the vblank (unknown here):
      // frames may overlap (lead > one refresh).
      double maxLead = std::min(0.030, 2.0 * r);
      rnf_input_deadline_set_limits(deadline_, 0, maxLead, std::max(0.003, maxLead - RNF_INPUT_DEADLINE_MIN_LEAD));
      double rate = sched_.cadence().emulationRate(r);
      if (std::fabs(rate - lastRate) > 0.01) { audio_.setEmulationRate(rate); lastRate = rate; }
      audio_.setFramesPerPush(sched_.cadence().kind == Cadence::Kind::slower ? RNF_MAX_FRAMES_PER_PRESENT : 1);
    }
    double lead = rnf_input_deadline_lead(deadline_);
    int frames = 1;
    double target = vr_.presentWait() ? sched_.nextTarget(nowSeconds(), lead, &frames) : 0;
    double sampleAt = target > 0 ? target - lead : 0;
    lastTargetIssued = target;
    if (!vr_.presentWait()) {
      // No present timing: host clock at the NES rate (FIFO keeps the pictures on vblanks).
      sampleAt = lastSample + nesFramePeriod();
      if (sampleAt < nowSeconds() - 2 * nesFramePeriod()) sampleAt = nowSeconds();
    }
    // 3. Display slower than the NES rate: the frames before the shown one are emulated now, in
    //    the slack (their own, earlier time slots); the shown frame keeps its just-in-time sample.
    bool extraPicture = false;
    for (int i = 1; i < frames; ++i) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) handleEvent(e, nowSeconds());
      extraPicture = emu_->tick().newPicture || extraPicture;
    }
    // Housekeeping in the slack, then wait for the sample point.
    double slack = sampleAt - nowSeconds();
    if (slack > 0.004) {
      double now = nowSeconds();
      emu_->afterFrame(slack);
      app_->update(now);
      thumbs_->pump(emu_->session(), now);
      library_->poll();
    }
    sleepUntil(sampleAt);

    // 4. Sample: events, then one emulated frame.
    FrameRecord rec;
    rec.seq = ++seq;
    rec.wake = wake;
    rec.target = target;
    rec.lead = lead;
    rec.frames = frames;
    rec.refresh = sched_.refresh();
    rec.sample = lastSample = nowSeconds();
    SDL_Event e;
    while (SDL_PollEvent(&e)) handleEvent(e, rec.sample);
    if (!running_) break;
    rec.polled = nowSeconds();
    double ev = input_->lastEventTime();
    EmulationController::Tick tick = emu_->tick();
    rec.emulated = tick.emulated;
    if (tick.emulated && ev > pickedEvent_) { rec.event = ev; pickedEvent_ = ev; }
    if (emu_->session()) rec.frame = rn_frame(emu_->session());
    rec.emulatedAt = nowSeconds();

    // 5. UI + draw + present.
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (h != lastHeight_ || settings_.uiScale != lastUIScale_) {
      lastHeight_ = h;
      lastUIScale_ = settings_.uiScale;
      ui_->updateScale(h);
    }
    if (script) script->step(rec.sample);
    updateNavigation();
    vr_.beginUIFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;  // fed from the events (PadNavFeed)
    padNav_.setSuppressed(ui_->textEntryOwnsPad());
    if (padNav_.takeActivity()) ui_->padUsed();
    ImGui::NewFrame();
    ui_->build(rec.sample);
    ImGui::Render();
    rec.ui = nowSeconds();
    GameRect rect = ui_->gameRect(w, h);
    bool newPicture = tick.newPicture || extraPicture;
    const EmulationController::Signal& es = emu_->signal();
    FrameSignal sig{es.codes, es.burstPhase, es.ordinal, es.flashAltered};
    uint64_t id = vr_.drawAndPresent(newPicture ? emu_->picture() : nullptr, rect, ImGui::GetDrawData(), &sig);
    rec.submit = nowSeconds();
    rec.acquireWait = vr_.lastAcquireWait();
    // CRT built with the present: its GPU time is part of the sample -> screen work.
    rec.gpuExtra = vr_.gpuLeadExtra();
    {
      // CRT built ahead: this present shows the previous new picture (sampled one frame earlier).
      PostProcessStatus ps = vr_.postProcessStatus();
      if (ps.crtShown && ps.buildAhead && lastPictureSample_ > 0) rec.pictureSample = lastPictureSample_;
      if (newPicture) lastPictureSample_ = rec.sample;
    }
    rnf_input_deadline_observe_work(cpuWork_, rec.submit - rec.sample);
    rnf_input_deadline_observe_work(deadline_, rec.submit - rec.sample + rec.gpuExtra);
    // Build-ahead budget: GPU time left before the target vblank at the maximum lead.
    vr_.setGpuBudget(std::max(0.0, rnf_input_deadline_max_lead(deadline_) - rnf_input_deadline_work_quantile(cpuWork_) -
                                       RNF_INPUT_DEADLINE_MARGIN));
    if (id) {
      rec.presentId = id;
      if (vr_.presentWait()) inflight.push_back(rec);
      else if (perfMode) perf.add(rec);
    } else {
      sleepUntil(nowSeconds() + nesFramePeriod());  // hidden / minimised: keep time on the host clock
    }

    // Perf window.
    if (perfMode) {
      double el = nowSeconds() - start;
      if (!perf.inWindow() && el >= opt.warmup && el < opt.warmup + opt.perfSeconds)
        perf.beginWindow(nowSeconds(), processCPUSeconds(), threadCPUSeconds(), audio_.underruns(), emu_->emulatedFrames());
      if (el >= opt.warmup + opt.perfSeconds) {
        perf.endWindow(nowSeconds(), processCPUSeconds(), threadCPUSeconds(), audio_.underruns(), emu_->emulatedFrames(),
                       audio_.ratio(), audio_.fillMs(), sched_.skippedRefreshes());
        app_->quitNow();
        running_ = false;
      }
    }
  }

  injecting = false;
  if (injector.joinable()) injector.join();
  polling = false;
  padThread.join();
  if (perfMode) {
    sleepUntil(nowSeconds() + 0.1);  // the last presents reach the screen
    for (const PresentDone& d : vr_.takeCompleted()) {
      while (!inflight.empty() && inflight.front().presentId < d.id) { settle(inflight.front(), nullptr); inflight.pop_front(); }
      if (!inflight.empty() && inflight.front().presentId == d.id) { settle(inflight.front(), &d); inflight.pop_front(); }
    }
    for (auto& rec : inflight) settle(rec, nullptr);
    Cadence c = sched_.cadence();
    info.cadence = c.kind == Cadence::Kind::locked ? "locked k=" + std::to_string(c.k) : c.name();
    if (c.kind == Cadence::Kind::unknown) info.cadence = "unknown";
    info.multiFramePresents = sched_.multiFramePresents();
    info.droppedFrames = sched_.droppedFrames();
    {
      PostProcessStatus ps = vr_.postProcessStatus();
      if (vr_.postProcess().crt) {
        char b[200];
        std::snprintf(b, sizeof b, "%dx%d x%.2f %s gpu p50 %.2f p90 %.2f ms%s", ps.tubeWidth, ps.tubeHeight, ps.scale,
                      ps.usedCodes ? "RF" : "RGB", ps.gpuMsP50, ps.gpuMsP90, ps.buildAhead ? " built-ahead" : "");
        info.crt = b;
      } else {
        info.crt = "off";
      }
    }
    info.videoDriver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?";
    info.gpu = vr_.description();
    info.audioDevice = audio_.deviceName();
    info.presentWait = vr_.presentWait();
    info.backlogDrains = sched_.backlogDrains();
    info.fullscreen = fullscreen_;
    info.width = int(vr_.extent().width);
    info.height = int(vr_.extent().height);
    info.mode = underGamescope() ? "gamescope" : (std::getenv("XDG_CURRENT_DESKTOP") ? std::getenv("XDG_CURRENT_DESKTOP") : "desktop");
    for (auto [res, n] : waitFailures) std::fprintf(stderr, "present wait failures: result %d x %d\n", res, n);
    std::string summary = perf.finish(info, opt.statsLog, opt.frameLog);
    std::fputs(summary.c_str(), stdout);
  }
  emu_->closeSession();  // persisted above (quitNow / requestQuit)
  if (!perfMode) settings_.save(paths_.settingsFile());
  app_.reset();
  ui_.reset();
  ImGui_ImplSDL3_Shutdown();
  vr_.shutdown();
  ImGui::DestroyContext();
  thumbs_.reset();
  emu_.reset();
  library_.reset();
  if (audioOK_) audio_.close();
  input_.reset();
  rnf_input_deadline_free(deadline_);
  rnf_input_deadline_free(cpuWork_);
  SDL_DestroyWindow(window_);
  SDL_Quit();
  if (restart_) {
    // Into the update: the newest installed version, through the portal; this process waits for
    // it so that Steam keeps the game running (and stops it with this one).
    int rc = updates_->restartLatestAndWait(restartArguments(opt.argv));
    if (rc >= 0) return rc;
    std::fprintf(stderr, "could not start the updated ReplayNES: start it again\n");
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (int rc = 0; steam::runCli(argc, argv, &rc)) return rc;  // --add-to-steam [--dry-run] (no window)
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("ReplayNES %s\n", RNL_APP_VERSION);
    return 0;
  }
  Options opt;
  if (!parseArgs(argc, argv, &opt)) return 2;
  opt.argv.assign(argv, argv + argc);
  App app;
  return app.run(opt);
}
