// Keyboard (SDL scancodes) + SDL3 gamepads -> rn_input, the engine's input pipeline (mapping,
// turbo, SOCD, hotkeys). Reports physical changes with the shared core's stable ids:
// "kb:<SDL scancode>" (RNF_KEYBOARD_SDL) and "gc<slot>:<element>" with POSITIONAL elements
// (rnf_sdl_button_element: NES A = east, NES B = south on every pad).
// Also: bindings persistence (bindings.json, the engine's JSON) + the core's layout migrations,
// paused D-pad frame stepping (routed away from the game, InputManager.swift on macOS), the live
// "pressed" set and connected pads for the settings diagram, and "press a key to assign".
// The right stick click (R3) and the Guide button open the ReplayNES menu (Gaming Mode has no
// keyboard); they are reserved and never reach the game.
// Frame-loop thread only (SDL events are polled there); rn_input itself is internally locked.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "replaynes/frontend.h"
#include "replaynes/replaynes.h"

namespace rnl {

struct Settings;

struct PadInfo {
  SDL_JoystickID id = 0;
  SDL_Gamepad* pad = nullptr;
  std::string name;
  rnf_controller_family family = RNF_FAMILY_GENERIC;
  std::map<std::string, std::string> labels;  // printed face labels by element ("face.east" -> "B")
  bool lt = false, rt = false;
  float lx = 0, ly = 0, rx = 0, ry = 0;
};

class InputRouter {
 public:
  static constexpr int kSlots = 4;

  InputRouter(std::string bindingsFile, Settings* settings);
  ~InputRouter();
  InputRouter(const InputRouter&) = delete;
  InputRouter& operator=(const InputRouter&) = delete;

  rn_input* input() const { return in_; }

  /// Saved bindings (or the defaults) + the core's layout migrations.
  void load();

  // Hooks.
  std::function<void()> onMenuButton;                    // R3 / Guide / Esc / F1
  std::function<void(int dir, bool down)> onPausedStep;  // D-pad left/right while paused
  std::function<void(const std::string& name)> onDisconnect;
  std::function<void()> onPadsChanged;

  /// ui = a menu / the library has the input: nothing reaches the game (all game input released).
  void setUIMode(bool ui);
  bool uiMode() const { return ui_; }
  /// Emulation paused state (paused D-pad stepping). Entering pause releases D-pad left/right held
  /// for the game, so a following frame advance does not record them.
  void setPausedStepMode(bool paused);

  /// One SDL event (keyboard / gamepad). evTime: the event's time on the CLOCK_MONOTONIC clock.
  /// keyboardForUI: ImGui wants the keyboard (text field or menu).
  void handleEvent(const SDL_Event& e, double evTime, bool keyboardForUI);

  uint64_t pressSequence() const { return pressSeq_.load(); }
  /// Time of the latest physical change that reached the game (latency measurement).
  double lastEventTime() const { return lastEvent_.load(); }
  void noteInjectedEvent(double t) { lastEvent_.store(t); }

  // Settings diagram / bindings UI.
  const PadInfo& pad(int slot) const { return pads_[slot]; }
  int connectedCount() const;
  const std::set<std::string>& pressed() const { return pressed_; }
  /// Bindings as (input, action) pairs (refreshed on every change).
  const std::vector<rnf_binding>& bindings() const { return bindingView_; }
  int turboPeriod() const { return turboPeriod_; }
  int turboDuty() const { return turboDuty_; }
  std::string socd() const { return socd_; }
  double analogThreshold() const { return analogThreshold_; }

  void bind(const std::string& physical, const std::string& action);
  void unbind(const std::string& physical, const std::string& action);
  /// Diagram: `physical` does exactly `action` from now on ("" = nothing).
  void setAssignment(const std::string& physical, const std::string& action);
  void resetController(int slot);
  void resetToDefaults();
  void setTurbo(int period, int duty);
  void setSOCD(const std::string& policy);
  void setAnalogThreshold(double t);

  /// "Add…": the next key / button / stick direction is handed to fn ("" = cancelled with Esc).
  void beginCapture(std::function<void(const std::string& id)> fn);
  void cancelCapture();
  bool capturing() const { return bool(capture_); }

  /// Reserved for the menu (not assignable in the diagram).
  static bool isReserved(const std::string& element) { return element == "rightThumb" || element == "home"; }

  /// Pad thread: SDL_UpdateJoysticks at ~1 kHz (SDL_HINT_AUTO_UPDATE_JOYSTICKS = 0).
  void closeAll();

 private:
  void persist();
  void refreshBindings();
  void setPressed(const std::string& id, bool down, double t, bool game);
  void padButton(int slot, int button, bool down, double t);
  void padAxis(int slot, int axis, float v, double t);
  void stickDirections(int slot, const char* stick, float x, float y, double t);
  bool routePausedStep(const std::string& id, bool down);
  bool applyPlan(rnf_list* unbind, rnf_list* bind);
  int slotOf(SDL_JoystickID id) const;
  void attach(SDL_JoystickID id);
  void detach(SDL_JoystickID id);

  rn_input* in_ = nullptr;
  std::string file_;
  Settings* settings_;
  PadInfo pads_[kSlots];
  bool ui_ = false;
  bool pausedStepMode_ = false;
  std::map<std::string, int> stepDirs_;  // controller id -> -1 / +1
  std::set<std::string> routedSteps_;
  std::set<std::string> pressed_;        // every held key / button / stick direction
  std::map<std::string, bool> stickState_;
  std::atomic<uint64_t> pressSeq_{0};
  std::atomic<double> lastEvent_{0};
  std::function<void(const std::string&)> capture_;
  // parsed config
  std::vector<std::pair<std::string, std::string>> bindingPairs_;
  std::vector<rnf_binding> bindingView_;
  int turboPeriod_ = 4, turboDuty_ = 2;
  std::string socd_ = "neutral";
  double analogThreshold_ = 0.5;
};

}  // namespace rnl
