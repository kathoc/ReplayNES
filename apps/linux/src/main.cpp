// ReplayNES for Linux / Steam Deck (plan Step 2 skeleton): SDL3 window + gamepads + audio, a
// minimal Vulkan presenter, Dear ImGui shell, display-locked frame loop on the engine C API.
//
// Frame loop (one iteration per emulated frame, docs/FRAME_PACING.md adapted to Vulkan):
//   1. wait until the previous present is on screen (VK_KHR_present_wait) -> vblank timestamp
//   2. DisplayScheduler picks the vblank the next frame is aimed at (cadence: 60 Hz every
//      refresh, 90 Hz alternating 2/1, 120 Hz every 2nd); InputDeadline gives the lead
//   3. sleep until target - lead, pump SDL events, sample input, emulate one frame (EmuHost),
//      flash filter, push audio (DRC), build the UI, draw, submit, present (FIFO)
//   4. autosave etc. only in the slack before the next sample
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
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio_out.h"
#include "display_scheduler.h"
#include "host_clock.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "interim/emu_host.h"
#include "interim/pacing_logic.h"
#include "interim/playback_logic.h"
#include "mp4_export.h"
#include "render/crt_export.h"
#include "paths.h"
#include "perf_stats.h"
#include "replaynes/replaynes.h"
#include "vk_renderer.h"

namespace fs = std::filesystem;
using namespace rnl;

namespace {

struct Options {
  std::string rom;
  double perfSeconds = 0;
  double warmup = 8;
  std::string statsLog, frameLog, sessionRoot, label = "run";
  int fullscreen = -1;  // -1 = auto (gamescope: on)
  bool injectInput = false;
  bool resume = false;
  int flash = -1;  // -1 = the saved setting
  bool crt = false;          // --crt: start with the CRT display on (F4 toggles; temporary hook)
  int crtMaxWidth = 1600;
  bool crtAdaptive = true;
  bool crtBuildAhead = true;
  int integerScale = -1;     // --integer-scale 0|1 for this run (-1 = the saved setting)
};

void usage() {
  std::printf(
      "replaynes-linux [options]\n"
      "  --rom PATH            start recording PATH (temporary project)\n"
      "  --resume              reopen the last session\n"
      "  --fullscreen | --windowed\n"
      "  --perf-seconds N      measure N seconds after --warmup S (default 8), then quit\n"
      "  --stats-log FILE      append the JSON summary to FILE\n"
      "  --frame-log FILE      per-frame CSV\n"
      "  --session-root DIR    session folder (perf runs use a scratch folder)\n"
      "  --inject-input        press B every ~0.25 s from another thread (event -> screen latency)\n"
      "  --label NAME          label of the summary\n"
      "  --flash 0-3           flash reduction level for this run (off / low / standard / high)\n"
      "  --crt                 CRT display (nesterm physical model) on; F4 toggles it\n"
      "  --crt-max-width N     tube width cap (default 1600); --crt-fixed: no adaptive resolution;\n"
      "  --crt-no-build-ahead  never build CRT pictures one frame ahead\n"
      "  --integer-scale 0|1   integer scaling for this run (0 = fill the screen)\n");
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
    else if (a == "--resume") o->resume = true;
    else if (a == "--help" || a == "-h") { usage(); std::exit(0); }
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return false; }
  }
  return true;
}

bool underGamescope() { return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") || std::getenv("SteamGamepadUI"); }

// ---------------------------------------------------------------- settings (minimal ini)
struct Settings {
  bool integerScale = true;
  bool par87 = false;
  bool hideOverscan = true;
  bool showStats = false;
  int flash = RN_FLASH_STANDARD;
  void load(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq);
      int v = std::atoi(line.c_str() + eq + 1);
      if (k == "integerScale") integerScale = v;
      else if (k == "par87") par87 = v;
      else if (k == "hideOverscan") hideOverscan = v;
      else if (k == "showStats") showStats = v;
      else if (k == "flash" && v >= 0 && v <= 3) flash = v;
    }
  }
  void save(const std::string& path) const {
    std::ofstream f(path);
    f << "integerScale=" << integerScale << "\npar87=" << par87 << "\nhideOverscan=" << hideOverscan
      << "\nshowStats=" << showStats << "\nflash=" << flash << "\n";
  }
};

// ---------------------------------------------------------------- input mapping
const char* buttonName(SDL_GamepadButton b) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "face.south";
    case SDL_GAMEPAD_BUTTON_EAST: return "face.east";
    case SDL_GAMEPAD_BUTTON_WEST: return "face.west";
    case SDL_GAMEPAD_BUTTON_NORTH: return "face.north";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "dpad.up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "dpad.down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "dpad.left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "dpad.right";
    case SDL_GAMEPAD_BUTTON_START: return "menu";
    case SDL_GAMEPAD_BUTTON_BACK: return "options";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "leftShoulder";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "rightShoulder";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "leftThumbstickButton";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "paddle.l4";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "paddle.r4";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "paddle.l5";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "paddle.r5";
    default: return nullptr;
  }
}

void bindDefaults(rn_input* in) {
  // Keyboard (SDL scancodes), same keys as the macOS defaults.
  const std::pair<SDL_Scancode, const char*> kb[] = {
      {SDL_SCANCODE_UP, "p1.up"},       {SDL_SCANCODE_DOWN, "p1.down"},         {SDL_SCANCODE_LEFT, "p1.left"},
      {SDL_SCANCODE_RIGHT, "p1.right"}, {SDL_SCANCODE_X, "p1.a"},               {SDL_SCANCODE_Z, "p1.b"},
      {SDL_SCANCODE_S, "p1.turbo_a"},   {SDL_SCANCODE_A, "p1.turbo_b"},         {SDL_SCANCODE_RETURN, "p1.start"},
      {SDL_SCANCODE_RSHIFT, "p1.select"}, {SDL_SCANCODE_BACKSLASH, "p1.select"}, {SDL_SCANCODE_SPACE, "hk.pause"},
      {SDL_SCANCODE_PERIOD, "hk.frame_advance"}, {SDL_SCANCODE_COMMA, "hk.step_back"},
      {SDL_SCANCODE_BACKSPACE, "hk.rewind"}, {SDL_SCANCODE_TAB, "hk.fast_forward"}, {SDL_SCANCODE_L, "hk.slow"},
      {SDL_SCANCODE_B, "hk.bookmark"},
  };
  for (const auto& [sc, action] : kb) rn_input_bind(in, ("kb:" + std::to_string(int(sc))).c_str(), action);
  for (const auto& [id, action] : interim::defaultControllerBindings()) rn_input_bind(in, id.c_str(), action.c_str());
  rn_input_bind(in, "inject:b", "p1.b");
}

struct PadSlot {
  SDL_JoystickID id = 0;
  SDL_Gamepad* pad = nullptr;
  bool lt = false, rt = false;
  float lx = 0, ly = 0;
  std::string name;
};

// ---------------------------------------------------------------- the app
class App {
 public:
  int run(const Options& opt);

 private:
  void handleEvent(const SDL_Event& e, double now);
  void gamepadButton(int slot, SDL_GamepadButton b, bool down, double now);
  void setMenu(bool open);
  void buildUI(double now);
  void openRom(const std::string& path);
  void applyFullscreen(bool on);
  void releaseStepDirections();

  Options opt_;
  Paths paths_;
  Settings settings_;
  SDL_Window* window_ = nullptr;
  VkRenderer vr_;
  AudioOut audio_;
  bool audioOK_ = false;
  interim::EmuHost emu_;
  DisplayScheduler sched_;
  interim::InputDeadline deadline_;
  interim::InputDeadline cpuWork_;
  double lastPictureSample_ = 0;  // CPU part of the work only (build-ahead budget)
  PadSlot pads_[2];
  std::set<std::string> routedSteps_;
  bool running_ = true;
  bool menuOpen_ = false;
  bool focusMenu_ = false;
  bool fullscreen_ = false;
  bool sessionLocked_ = true;
  bool wasPaused_ = false;
  std::vector<std::string> roms_;
  ExportJob exportJob_;  // F6: temporary hook for the MP4 export (until the export UI)
  bool exportReported_ = true;
  std::atomic<double> lastEvent_{0};
  double pickedEvent_ = 0;
  std::string error_;
  // rolling stats for the overlay
  std::vector<double> recentLatency_;
  size_t recentHead_ = 0;
  uint64_t presents_ = 0;
};

void App::applyFullscreen(bool on) {
  fullscreen_ = on;
  SDL_SetWindowFullscreen(window_, on);
}

// Entering pause: D-pad left/right held for the game are released (a following frame advance
// would record them) and their releases are swallowed (InputManager.setPausedStepMode on macOS).
void App::releaseStepDirections() {
  if (!pads_[0].pad) return;
  for (SDL_GamepadButton b : {SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT}) {
    std::string id = std::string("gc0:") + buttonName(b);
    if (SDL_GetGamepadButton(pads_[0].pad, b) && !routedSteps_.count(id)) {
      rn_input_set_pressed(emu_.input(), id.c_str(), 0);
      routedSteps_.insert(id);
    }
  }
}

void App::setMenu(bool open) {
  if (open == menuOpen_) return;
  menuOpen_ = open;
  ImGuiIO& io = ImGui::GetIO();
  if (open) {
    rn_input_release_all(emu_.input());  // nothing held for the game while the menu is up
    routedSteps_.clear();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    focusMenu_ = true;
    roms_ = listRoms(paths_.romDir);
  } else {
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    // While playing ImGui does not read the pads: its per-frame polling would wait for the
    // joystick lock while the pad thread is in a slow HIDAPI call.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
  }
}

void App::openRom(const std::string& path) {
  std::string err;
  if (!sessionLocked_) { error_ = "Another ReplayNES instance owns the session folder"; return; }
  if (emu_.openRom(path, paths_.tempProject(), paths_.projectsDir, &err)) {
    error_.clear();
    setMenu(false);
  } else {
    error_ = "Couldn't open " + fs::path(path).filename().string() + ": " + err;
  }
}

void App::gamepadButton(int slot, SDL_GamepadButton b, bool down, double now) {
  if (b == SDL_GAMEPAD_BUTTON_RIGHT_STICK || b == SDL_GAMEPAD_BUTTON_GUIDE) {
    if (down) setMenu(!menuOpen_ || !emu_.session());
    return;
  }
  if (menuOpen_) {
    // East (B on the Deck) closes the menu when nothing is being edited.
    if (down && b == SDL_GAMEPAD_BUTTON_EAST && emu_.session() && !ImGui::IsAnyItemActive()) setMenu(false);
    return;
  }
  const char* n = buttonName(b);
  if (!n) return;
  std::string id = "gc" + std::to_string(slot) + ":" + n;
  // While paused, D-pad left/right step frames instead of reaching the game (frontend rule).
  bool stepKey = slot == 0 && (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
  int dir = b == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? -1 : 1;
  if (stepKey && down && emu_.paused() && emu_.session()) {
    routedSteps_.insert(id);
    emu_.stepFrame(emu_.stepRepeater().press(dir));
    return;
  }
  if (stepKey && !down && routedSteps_.count(id)) {
    routedSteps_.erase(id);
    emu_.stepRepeater().release(dir);
    return;
  }
  rn_input_set_pressed(emu_.input(), id.c_str(), down ? 1 : 0);
  if (down) lastEvent_.store(now);
}

void App::handleEvent(const SDL_Event& e, double now) {
  ImGui_ImplSDL3_ProcessEvent(&e);
  // SDL event timestamps (SDL_GetTicksNS) -> our CLOCK_MONOTONIC seconds.
  double evTime = now - double(SDL_GetTicksNS() - e.common.timestamp) * 1e-9;
  switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running_ = false; break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      if (menuOpen_) focusMenu_ = true;  // (gamescope gives the focus after the first frames)
      break;
    case SDL_EVENT_GAMEPAD_ADDED: {
      SDL_JoystickID id = e.gdevice.which;
      for (auto& p : pads_)
        if (p.pad && p.id == id) return;
      for (auto& p : pads_) {
        if (p.pad) continue;
        p.pad = SDL_OpenGamepad(id);
        p.id = id;
        p.name = p.pad ? SDL_GetGamepadName(p.pad) : "";
        std::fprintf(stderr, "gamepad: %s (slot %d)\n", p.name.c_str(), int(&p - pads_));
        break;
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
      for (int s = 0; s < 2; ++s) {
        if (pads_[s].pad && pads_[s].id == e.gdevice.which) {
          SDL_CloseGamepad(pads_[s].pad);
          pads_[s] = PadSlot();
          rn_input_release_prefix(emu_.input(), ("gc" + std::to_string(s) + ":").c_str());
          if (emu_.session()) { emu_.setPaused(true); emu_.setNotice("Controller disconnected (paused)"); }
        }
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      for (int s = 0; s < 2; ++s)
        if (pads_[s].pad && pads_[s].id == e.gbutton.which)
          gamepadButton(s, SDL_GamepadButton(e.gbutton.button), e.gbutton.down, evTime);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      for (int s = 0; s < 2; ++s) {
        PadSlot& p = pads_[s];
        if (!p.pad || p.id != e.gaxis.which) continue;
        float v = float(e.gaxis.value) / 32767.0f;
        std::string g = "gc" + std::to_string(s) + ":";
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
          bool right = e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
          bool& state = right ? p.rt : p.lt;
          bool now_ = state ? v > 0.35f : v > 0.5f;  // hysteresis
          if (now_ != state) {
            state = now_;
            if (!menuOpen_) rn_input_set_pressed(emu_.input(), (g + (right ? "rightTrigger" : "leftTrigger")).c_str(), now_);
          }
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
          (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? p.lx : p.ly) = v;
          if (!menuOpen_) rn_input_set_axis(emu_.input(), (g + "lstick").c_str(), p.lx, -p.ly);
        }
      }
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      if (e.key.repeat) break;
      bool down = e.type == SDL_EVENT_KEY_DOWN;
      if (std::getenv("REPLAYNES_DEBUG_INPUT"))
        std::fprintf(stderr, "key %d %s menu=%d navWindow=%s\n", int(e.key.scancode), down ? "down" : "up", int(menuOpen_),
                     ImGui::GetCurrentContext() && ImGui::GetIO().NavActive ? "active" : "-");
      SDL_Scancode sc = e.key.scancode;
      if (down && (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F1)) { setMenu(!menuOpen_ || !emu_.session()); break; }
      if (down && sc == SDL_SCANCODE_F11) { applyFullscreen(!fullscreen_); break; }
      if (down && sc == SDL_SCANCODE_F3) { settings_.showStats = !settings_.showStats; break; }
      if (down && sc == SDL_SCANCODE_F6 && emu_.session()) {
        // MP4 export of the take (temporary hook): current flash level and CRT display state.
        if (exportJob_.running()) { exportJob_.cancel(); break; }
        ExportOptions eo;
        eo.flash = emu_.flashLevel();
        if (vr_.postProcess().crt) eo.makeProcessor = crtExportProcessorFactory(vr_.postProcess().crtSettings);
        char name[64];
        std::snprintf(name, sizeof name, "export-%lld.mp4", (long long)std::time(nullptr));
        std::string out = (fs::path(paths_.projectsDir).parent_path() / name).string();
        if (exportJob_.start(emu_.session(), eo, out)) { emu_.setNotice("Exporting " + out); exportReported_ = false; }
        else emu_.setNotice("Export failed: " + exportJob_.error());
        break;
      }
      if (down && sc == SDL_SCANCODE_F4) {  // CRT display on/off (temporary hook until the settings UI)
        DisplayPostProcess pp = vr_.postProcess();
        pp.crt = !pp.crt;
        vr_.setPostProcess(pp);
        emu_.setNotice(pp.crt ? "CRT display on" : "CRT display off");
        break;
      }
      if (menuOpen_) break;
      rn_input_set_pressed(emu_.input(), ("kb:" + std::to_string(int(sc))).c_str(), down);
      if (down) lastEvent_.store(evTime);
      break;
    }
    default: break;
  }
}

void App::buildUI(double now) {
  ImGuiIO& io = ImGui::GetIO();
  rn_session* s = emu_.session();
  // Status line (top-left): mode, frame, transport state, notices, optional stats.
  {
    std::string state;
    if (s) {
      rn_mode m = rn_get_mode(s);
      char b[160];
      std::snprintf(b, sizeof b, "%s  %llu / %llu", m == RN_MODE_RECORD ? "REC" : m == RN_MODE_REPLAY ? "PLAY" : "PRACTICE",
                    (unsigned long long)rn_frame(s), (unsigned long long)rn_take_length(s));
      state = b;
      if (emu_.paused()) state += "  PAUSED";
      if (emu_.rewinding()) state += "  << REWIND";
      if (emu_.fastForwarding()) state += "  >> FF";
      if (emu_.slow() != interim::SlowRate::normal) state += "  SLOW 1/2";
    }
    if (exportJob_.running()) {
      char b[96];
      std::snprintf(b, sizeof b, "Exporting MP4 %.0f %% (F6 cancels)", exportJob_.progress() * 100);
      emu_.setNotice(b);
    } else if (!exportReported_ && exportJob_.finished()) {
      exportReported_ = true;
      emu_.setNotice(exportJob_.succeeded() ? "Exported " + exportJob_.outPath() : "Export: " + exportJob_.error());
    }
    bool notice = !emu_.notice().empty() && now - emu_.noticeTime() < 3.0;
    bool showState = s && (emu_.paused() || emu_.rewinding() || emu_.fastForwarding() || emu_.slow() != interim::SlowRate::normal);
    if (showState || notice || settings_.showStats) {
      ImGui::SetNextWindowPos(ImVec2(12, 12));
      ImGui::SetNextWindowBgAlpha(0.55f);
      ImGui::Begin("##status", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
      if (!state.empty()) ImGui::TextUnformatted(state.c_str());
      if (notice) ImGui::TextUnformatted(emu_.notice().c_str());
      if (settings_.showStats) {
        double r = sched_.refresh();
        interim::Cadence c = sched_.cadence();
        const char* cn = c.kind == interim::Cadence::Kind::locked ? "locked" : c.kind == interim::Cadence::Kind::three_two ? "3:2" : c.kind == interim::Cadence::Kind::free ? "free" : c.kind == interim::Cadence::Kind::slower ? "slower (<=2 frames/present)" : "?";
        double lat = 0;
        for (double v : recentLatency_) lat += v;
        if (!recentLatency_.empty()) lat /= double(recentLatency_.size());
        ImGui::Text("%.2f Hz %s  lead %.2f ms  sample->screen %.2f ms", r > 0 ? 1 / r : 0, cn, deadline_.lead() * 1000, lat * 1000);
        ImGui::Text("audio ratio %.4f  fill %.1f ms  underruns %llu  present_wait %s", audio_.ratio(), audio_.fillMs(),
                    (unsigned long long)audio_.underruns(), vr_.presentWait() ? "yes" : "no");
        PostProcessStatus ps = vr_.postProcessStatus();
        if (vr_.postProcess().crt)
          ImGui::Text("CRT %dx%d (x%.2f) %s  GPU p50 %.2f / p90 %.2f ms%s", ps.tubeWidth, ps.tubeHeight, ps.scale,
                      ps.usedCodes ? "RF" : "RGB", ps.gpuMsP50, ps.gpuMsP90, ps.buildAhead ? "  built ahead" : "");
      }
      ImGui::End();
    }
  }
  if (!menuOpen_ && s) return;
  // Main menu (gamepad navigable).
  ImVec2 size(std::min(760.0f, io.DisplaySize.x * 0.92f), std::min(700.0f, io.DisplaySize.y * 0.92f));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  bool focusFirst = focusMenu_;  // gamepad/keyboard nav starts on the first item
  if (focusMenu_) { ImGui::SetNextWindowFocus(); focusMenu_ = false; }
  ImGui::Begin("ReplayNES", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", error_.c_str());
  if (s) {
    ImGui::Text("%s", emu_.romName().c_str());
    if (focusFirst) { ImGui::SetKeyboardFocusHere(); ImGui::SetNavCursorVisible(true); focusFirst = false; }
    if (ImGui::Button("Resume")) setMenu(false);
    ImGui::SameLine();
    if (ImGui::Button(emu_.paused() ? "Play" : "Pause")) emu_.togglePause();
    ImGui::SameLine();
    if (ImGui::Button(rn_get_mode(s) == RN_MODE_RECORD ? "Play back recording" : "Record from here")) emu_.toggleRecord();
    ImGui::SameLine();
    if (ImGui::Button("Save")) emu_.save();
  } else {
    ImGui::TextWrapped("Choose a ROM to start recording.");
    std::error_code ec;
    bool canResume = sessionLocked_ && fs::exists(paths_.tempProject(), ec);
    if (canResume && focusFirst) { ImGui::SetKeyboardFocusHere(); ImGui::SetNavCursorVisible(true); focusFirst = false; }
    if (canResume && ImGui::Button("Resume last session")) {
      std::string err;
      if (emu_.resume(paths_.tempProject(), &err)) setMenu(false);
      else error_ = "Couldn't resume: " + err;
    }
  }
  ImGui::SeparatorText("Open ROM");
  if (roms_.empty()) {
    ImGui::TextWrapped("No .nes files in %s", paths_.romDir.c_str());
  } else {
    // NavFlattened: the D-pad moves straight into the list (no extra "enter child" press).
    ImGui::BeginChild("roms", ImVec2(0, std::max(120.0f, size.y * 0.32f)), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    for (size_t i = 0; i < roms_.size(); ++i) {
      std::string label = fs::path(roms_[i]).filename().string();
      if (focusFirst) { ImGui::SetKeyboardFocusHere(); ImGui::SetNavCursorVisible(true); focusFirst = false; }
      bool picked = ImGui::Selectable(label.c_str());
      if (picked) openRom(roms_[i]);
    }
    ImGui::EndChild();
  }
  if (ImGui::Button("Refresh list")) roms_ = listRoms(paths_.romDir);
  ImGui::SeparatorText("Display");
  bool changed = false;
  changed |= ImGui::Checkbox("Integer scale", &settings_.integerScale);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("8:7 pixel aspect", &settings_.par87);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Hide overscan", &settings_.hideOverscan);
  bool fs_ = fullscreen_;
  if (ImGui::Checkbox("Full screen", &fs_)) applyFullscreen(fs_);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Stats overlay", &settings_.showStats);
  const char* levels[] = {"Off", "Low", "Standard", "High"};
  ImGui::SetNextItemWidth(220);
  if (ImGui::Combo("Flash reduction", &settings_.flash, levels, 4)) {
    emu_.setFlashLevel(rn_flash_level(settings_.flash));
    changed = true;
  }
  if (changed) settings_.save(paths_.settingsFile());
  ImGui::SeparatorText("Controls");
  ImGui::TextWrapped("R2 rewind (hold) / L2 fast-forward (hold) / L slow 1/2 / R pause. While paused: D-pad left/right "
                     "steps frames. R3 or Esc: this menu. Keyboard: arrows, X = A, Z = B, Enter = START, Space pause, "
                     "Backspace rewind, Tab fast-forward, comma / period step.");
  if (ImGui::Button("Quit")) running_ = false;
  ImGui::End();
}

int App::run(const Options& opt) {
  opt_ = opt;
  paths_ = Paths::standard();
  if (!opt.sessionRoot.empty()) paths_.sessionRoot = opt.sessionRoot;
  paths_.ensure();
  settings_.load(paths_.settingsFile());
  if (opt.flash >= 0 && opt.flash <= 3) settings_.flash = opt.flash;
  if (opt.integerScale >= 0) settings_.integerScale = opt.integerScale != 0;
  sessionLocked_ = lockSessionRoot(paths_);
  prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);  // precise wake-ups for the just-in-time sample

  SDL_SetAppMetadata("ReplayNES", "0.2.0", "io.github.replaynes.ReplayNES");
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

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(1.6f);
  style.FontSizeBase = 24.0f;
  for (const char* f : {"/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
    std::error_code ec;
    if (fs::exists(f, ec) && io.Fonts->AddFontFromFileTTF(f)) break;
  }
  ImGui_ImplSDL3_InitForVulkan(window_);
  ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
  vr_.initImGui();

  bindDefaults(emu_.input());
  emu_.setFlashLevel(rn_flash_level(settings_.flash));
  {
    DisplayPostProcess pp;
    pp.crt = opt.crt;
    pp.maxTubeWidth = opt.crtMaxWidth;
    pp.adaptiveResolution = opt.crtAdaptive;
    pp.allowBuildAhead = opt.crtBuildAhead;
    vr_.setPostProcess(pp);
  }
  if (!sessionLocked_) error_ = "Another ReplayNES instance is running: the session folder is in use";
  if (!opt.rom.empty()) openRom(opt.rom);
  else if (opt.resume && emu_.resume(paths_.tempProject(), &err)) {}
  if (!emu_.session()) setMenu(true);

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
  std::atomic<bool> injecting{opt.injectInput && opt.perfSeconds > 0};
  std::thread injector;
  if (injecting) {
    injector = std::thread([&] {
      bool down = false;
      double t = nowSeconds() + 3.0;
      while (injecting.load()) {
        sleepUntil(t);
        down = !down;
        rn_input_set_pressed(emu_.input(), "inject:b", down);
        if (down) lastEvent_.store(nowSeconds());
        t += 0.2503;  // not a multiple of the frame period: presses land at every phase
      }
    });
  }

  PerfStats perf;
  RunInfo info;
  info.label = opt.label;
  const double start = nowSeconds();
  const bool perfMode = opt.perfSeconds > 0;
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
        deadline_.observeMiss();
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
      // The lead also absorbs the compositor's latch point before the vblank (unknown here).
      deadline_.maxLead = std::min(0.030, 2.0 * r);  // frames may overlap (lead > one refresh)
      deadline_.maxPenalty = std::max(0.003, deadline_.maxLead - deadline_.minLead);
      double rate = sched_.cadence().emulationRate(r);
      if (std::fabs(rate - lastRate) > 0.01) { audio_.setEmulationRate(rate); lastRate = rate; }
      audio_.setFramesPerPush(sched_.cadence().kind == interim::Cadence::Kind::slower ? RNF_MAX_FRAMES_PER_PRESENT : 1);
    }
    double lead = deadline_.lead();
    int frames = 1;
    double target = vr_.presentWait() ? sched_.nextTarget(nowSeconds(), lead, &frames) : 0;
    double sampleAt = target > 0 ? target - lead : 0;
    lastTargetIssued = target;
    if (!vr_.presentWait()) {
      // No present timing: host clock at the NES rate (FIFO keeps the pictures on vblanks).
      sampleAt = lastSample + interim::kNesFramePeriod;
      if (sampleAt < nowSeconds() - 2 * interim::kNesFramePeriod) sampleAt = nowSeconds();
    }
    // 3. Display slower than the NES rate: the frames before the shown one are emulated now, in
    //    the slack (their own, earlier time slots); the shown frame keeps its just-in-time sample.
    for (int i = 1; i < frames; ++i) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) handleEvent(e, nowSeconds());
      interim::EmuHost::Tick early = emu_.tick(menuOpen_);
      if (audioOK_) {
        audio_.setMuted(!early.audible);
        if (early.audible) audio_.push(early.pcm, early.pcmCount);
      }
    }
    // Housekeeping in the slack, then wait for the sample point.
    if (sampleAt - nowSeconds() > 0.004) emu_.housekeeping(nowSeconds());
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
    rec.polled = nowSeconds();
    double ev = lastEvent_.load();
    interim::EmuHost::Tick tick = emu_.tick(menuOpen_);
    rec.emulated = tick.emulated;
    if (tick.emulated && ev > pickedEvent_) { rec.event = ev; pickedEvent_ = ev; }
    if (audioOK_) {
      audio_.setMuted(!tick.audible);
      if (tick.audible) audio_.push(tick.pcm, tick.pcmCount);
    }
    if (emu_.session()) rec.frame = rn_frame(emu_.session());
    rec.emulatedAt = nowSeconds();
    if (emu_.paused() && !wasPaused_) releaseStepDirections();
    wasPaused_ = emu_.paused();

    // 5. UI + draw + present.
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    buildUI(rec.sample);
    ImGui::Render();
    rec.ui = nowSeconds();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    GameRect rect = emu_.session() ? computeGameRect(w, h, settings_.integerScale, settings_.par87, settings_.hideOverscan) : GameRect{};
    const interim::EmuHost::Signal& es = emu_.signal();
    FrameSignal sig{es.codes, es.burstPhase, es.ordinal, es.flashAltered};
    uint64_t id = vr_.drawAndPresent(tick.newPicture ? emu_.picture() : nullptr, rect, ImGui::GetDrawData(), &sig);
    rec.submit = nowSeconds();
    rec.acquireWait = vr_.lastAcquireWait();
    // CRT built with the present: its GPU time is part of the sample -> screen work.
    rec.gpuExtra = vr_.gpuLeadExtra();
    {
      // CRT built ahead: this present shows the previous new picture (sampled one frame earlier).
      PostProcessStatus ps = vr_.postProcessStatus();
      if (ps.crtShown && ps.buildAhead && lastPictureSample_ > 0) rec.pictureSample = lastPictureSample_;
      if (tick.newPicture) lastPictureSample_ = rec.sample;
    }
    cpuWork_.observeWork(rec.submit - rec.sample);
    deadline_.observeWork(rec.submit - rec.sample + rec.gpuExtra);
    // Build-ahead budget: GPU time left before the target vblank at the maximum lead.
    vr_.setGpuBudget(std::max(0.0, deadline_.maxLead - cpuWork_.workQuantile() - deadline_.margin));
    if (id) {
      presents_ += 1;
      rec.presentId = id;
      if (vr_.presentWait()) inflight.push_back(rec);
      else if (perfMode) perf.add(rec);
    } else {
      sleepUntil(nowSeconds() + interim::kNesFramePeriod);  // hidden / minimised: keep time on the host clock
    }

    // Perf window.
    if (perfMode) {
      double el = nowSeconds() - start;
      if (!perf.inWindow() && el >= opt.warmup && el < opt.warmup + opt.perfSeconds)
        perf.beginWindow(nowSeconds(), processCPUSeconds(), threadCPUSeconds(), audio_.underruns(), emu_.emulatedFrames());
      if (el >= opt.warmup + opt.perfSeconds) {
        perf.endWindow(nowSeconds(), processCPUSeconds(), threadCPUSeconds(), audio_.underruns(), emu_.emulatedFrames(),
                       audio_.ratio(), audio_.fillMs(), sched_.skippedRefreshes());
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
    interim::Cadence c = sched_.cadence();
    info.cadence = c.kind == interim::Cadence::Kind::locked ? "locked k=" + std::to_string(c.k)
                   : c.kind == interim::Cadence::Kind::three_two ? "3:2" : c.kind == interim::Cadence::Kind::free ? "free"
                   : c.kind == interim::Cadence::Kind::slower ? "slower" : "unknown";
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
    for (auto [r, n] : waitFailures) std::fprintf(stderr, "present wait failures: result %d x %d\n", r, n);
    std::string summary = perf.finish(info, opt.statsLog, opt.frameLog);
    std::fputs(summary.c_str(), stdout);
  }
  emu_.closeSession(true);  // clean quit: the temporary project is fully saved
  if (!perfMode) settings_.save(paths_.settingsFile());
  ImGui_ImplSDL3_Shutdown();
  vr_.shutdown();
  ImGui::DestroyContext();
  if (audioOK_) audio_.close();
  for (auto& p : pads_)
    if (p.pad) SDL_CloseGamepad(p.pad);
  SDL_DestroyWindow(window_);
  SDL_Quit();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) return 2;
  App app;
  return app.run(opt);
}
