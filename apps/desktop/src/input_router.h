// Keyboard (SDL scancodes) + SDL3 gamepads -> rn_input, the engine's input pipeline (mapping,
// turbo, SOCD, hotkeys). Reports physical changes with the shared core's stable ids:
// "kb:<SDL scancode>" (RNF_KEYBOARD_SDL) and "gc<slot>:<element>" with POSITIONAL elements
// (rnf_sdl_button_element: NES A = east, NES B = south on every pad).
// Also: bindings persistence (bindings.json, the engine's JSON) + the core's layout migrations,
// paused D-pad frame stepping (routed away from the game, InputManager.swift on macOS), the live
// "pressed" set and connected pads for the settings diagram, and "press a key to assign".
// The Quick Menu (docs/design/UI_REDESIGN.md) is the action "hk.menu": pad 1's L+R combo through
// the shared chord detector (rnf_chord) and Esc. L or R alone act on their RELEASE (never in the
// way of the chord): their own bindings in play (pause / slow), previous / next page in menus, a
// frame step back / forward while paused (held >= 400 ms: repeated), a held marker move while a
// seek bar marker is edited. The right stick click (R3) and the Guide button open the menu too
// (reserved, never reach the game). While paused in play (the seek bar), the UI's cancel button
// tapped resumes, confirm tapped drops a marker, up tapped goes to the markers (taps: they still
// reach the game, so a held button survives frame steps); with a marker focused the pad is the
// seek bar's (onSeekInput) and nothing reaches the game.
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
#include "ui_logic.h"

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
  std::function<void()> onMenuButton;                    // hk.menu (L+R, Esc), R3 / Guide / F1
  std::function<void(int dir, bool down)> onPausedStep;  // D-pad left/right while paused
  std::function<void()> onPausedResume;                  // cancel tapped while paused in play: resume
  /// The paused seek bar's controller input (docs/design/UI_REDESIGN.md): taps on the bar (ok, up,
  /// y: press only), everything while a marker is focused / edited (press and release).
  enum class SeekInput { ok, cancel, up, down, left, right, x, y };
  std::function<void(SeekInput in, bool down)> onSeekInput;
  /// L / R alone while paused: a frame step (-1 / +1) at the release, repeated while held.
  std::function<void(int dir)> onPausedShoulder;
  /// L / R held while a marker is edited: the marker moves (-1 / +1) until released.
  std::function<void(int dir, bool down)> onMarkerHold;
  /// L / R in the UI (menus, library): -1 / +1 when pressed (alone: after the chord detector).
  std::function<void(int dir)> onUiShoulder;
  std::function<void(const std::string& name)> onDisconnect;
  std::function<void()> onPadsChanged;

  /// ui = a menu / the library has the input: nothing reaches the game (all game input released).
  void setUIMode(bool ui);
  bool uiMode() const { return ui_; }
  /// Emulation paused state (paused D-pad stepping). Entering pause releases D-pad left/right held
  /// for the game, so a following frame advance does not record them.
  void setPausedStepMode(bool paused);
  /// The seek bar's focus while paused: 0 = the bar, 1 = a marker, 2 = a marker being edited.
  void setSeekFocus(int focus);

  /// One SDL event (keyboard / gamepad). evTime: the event's time on the CLOCK_MONOTONIC clock.
  /// keyboardForUI: ImGui wants the keyboard (text field or menu).
  void handleEvent(const SDL_Event& e, double evTime, bool keyboardForUI);
  /// Every frame after the events (the chord detector's 100 ms window). now: CLOCK_MONOTONIC s.
  void tick(double now);

  uint64_t pressSequence() const { return pressSeq_.load(); }
  /// Time of the latest physical change that reached the game (latency measurement).
  double lastEventTime() const { return lastEvent_.load(); }
  void noteInjectedEvent(double t) { lastEvent_.store(t); }

  // Settings diagram / bindings UI.
  const PadInfo& pad(int slot) const { return pads_[slot]; }
  int connectedCount() const;
  /// Slot of the controller used last (-1: none yet); its family picks the UI's button prompts.
  int lastSlot() const { return lastSlot_; }
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
  /// keysOnly: only keys are taken (a controller's B cancels, other buttons are ignored).
  void beginCapture(std::function<void(const std::string& id)> fn, bool keysOnly = false);
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
  bool routePausedTap(const std::string& id, bool down);
  bool routeSeek(const std::string& id, bool down);
  void updateRepeat();
  bool isGameMember(const std::string& id) const;
  void pumpChord();
  void routeButton(const std::string& id, bool down, double t);
  bool captureFrom(const std::string& id, bool keyboard);
  bool applyPlan(rnf_list* unbind, rnf_list* bind);
  int slotOf(SDL_JoystickID id) const;
  void attach(SDL_JoystickID id);
  void detach(SDL_JoystickID id);

  rn_input* in_ = nullptr;
  std::string file_;
  Settings* settings_;
  PadInfo pads_[kSlots];
  bool ui_ = false;
  int lastSlot_ = -1;
  bool pausedStepMode_ = false;
  std::map<std::string, int> stepDirs_;  // controller id -> -1 / +1
  std::set<std::string> routedSteps_;
  std::set<std::string> pressed_;        // every held key / button / stick direction
  std::map<std::string, bool> stickState_;
  std::atomic<uint64_t> pressSeq_{0};
  std::atomic<double> lastEvent_{0};
  std::function<void(const std::string&)> capture_;
  bool captureKeysOnly_ = false;
  rnf_chord* chord_ = nullptr;
  std::set<std::string> menuIds_;         // single inputs bound to hk.menu (Esc)
  ConfirmTap confirmTap_;                 // confirm / cancel / up / Y pressed while paused: taps
  int seekFocus_ = 0;
  std::set<std::string> gameMembers_;     // chord members bound to game input (held, not tapped)
  std::set<std::string> forwardedMembers_;  // ... pressed for the game now
  std::map<std::string, int> holdMembers_;  // members moving an edited marker now -> -1 / +1
  // parsed config
  std::vector<std::pair<std::string, std::string>> bindingPairs_;
  std::vector<rnf_binding> bindingView_;
  int turboPeriod_ = 4, turboDuty_ = 2;
  std::string socd_ = "neutral";
  double analogThreshold_ = 0.5;
};

}  // namespace rnl
