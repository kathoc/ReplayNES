// SDL gamepad events -> Dear ImGui's gamepad keys, for the UI's controller navigation.
// The SDL3 backend's own gamepad polling stays off (ImGui_ImplSDL3_GamepadMode_Manual, no pads):
// polling would wait for the joystick lock while the pad thread is in a slow HIDAPI call, and the
// events carry everything already. All pads are merged; the left stick also drives the D-pad keys
// (focus moves with either), with hysteresis. X (west) is fed as kPadX instead of
// ImGuiKey_GamepadFaceLeft: ImGui uses that key to open its window switcher when held, which the
// UI never wants (X sets A on the timeline / continues a project in the library). Y (north) is fed
// as kPadY instead of ImGuiKey_GamepadFaceUp: ImGui activates the focused item with it ("text
// input" - a press on a button clicks it), while the UI uses Y for its own actions (search, rename,
// set B). While a text field has the controller (the on-screen keyboard, ui_osk.cpp) nothing is
// fed (setSuppressed); buttons still held when that ends are fed only after their release.
// Frame-loop thread only (events are polled there).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "imgui.h"

namespace rnl {

/// The UI's key for the controller's X (west) button (see above).
constexpr ImGuiKey kPadX = ImGuiKey_F24;
/// The UI's key for the controller's Y (north) button (see above).
constexpr ImGuiKey kPadY = ImGuiKey_F23;

class PadNavFeed {
 public:
  void handle(const SDL_Event& e) {
    switch (e.type) {
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (e.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) pads_[e.gbutton.which].button[e.gbutton.button] = e.gbutton.down;
        break;
      case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (e.gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT) return;
        Pad& p = pads_[e.gaxis.which];
        float v = float(e.gaxis.value) / 32767.0f;
        p.axis[e.gaxis.axis] = v < -1 ? -1 : v;
        auto hyst = [](bool on, float x) { return on ? x > 0.35f : x > 0.5f; };
        p.stick[0] = hyst(p.stick[0], -p.axis[SDL_GAMEPAD_AXIS_LEFTX]);
        p.stick[1] = hyst(p.stick[1], p.axis[SDL_GAMEPAD_AXIS_LEFTX]);
        p.stick[2] = hyst(p.stick[2], -p.axis[SDL_GAMEPAD_AXIS_LEFTY]);
        p.stick[3] = hyst(p.stick[3], p.axis[SDL_GAMEPAD_AXIS_LEFTY]);
        p.trigger[0] = hyst(p.trigger[0], p.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]);
        p.trigger[1] = hyst(p.trigger[1], p.axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
        break;
      }
      case SDL_EVENT_GAMEPAD_REMOVED: pads_.erase(e.gdevice.which); break;
      default: return;
    }
    publish();
  }

  /// Feeds nothing to ImGui while on (all keys released); the pads' state is still followed.
  void setSuppressed(bool on) {
    if (on == suppressed_) return;
    suppressed_ = on;
    if (!on) {
      // Keys held now (e.g. Menu (≡) that ended the text input) wait for their release.
      held_.clear();
      for (const auto& [k, v] : current_)
        if (v.first) held_.insert(k);
    }
    publish();
  }
  bool suppressed() const { return suppressed_; }

  /// A controller button / stick moved since the last call (the UI shows its prompts then).
  bool takeActivity() {
    bool a = activity_;
    activity_ = false;
    return a;
  }

 private:
  struct Pad {
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> button{};
    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axis{};
    bool stick[4] = {false, false, false, false};  // left, right, up, down
    bool trigger[2] = {false, false};
  };

  void publish() {
    struct Map {
      ImGuiKey key;
      int button;
      int stick;  // also on with this left-stick direction (-1 none)
    };
    static const Map maps[] = {
        {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH, -1},  {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST, -1},
        {kPadX, SDL_GAMEPAD_BUTTON_WEST, -1},                      {kPadY, SDL_GAMEPAD_BUTTON_NORTH, -1},
        {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START, -1},     {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK, -1},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, -1}, {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1},
        {ImGuiKey_GamepadL3, SDL_GAMEPAD_BUTTON_LEFT_STICK, -1},   {ImGuiKey_GamepadR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK, -1},
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0}, {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 1},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP, 2},   {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 3},
    };
    ImGuiIO& io = ImGui::GetIO();
    for (const Map& m : maps) {
      bool on = false;
      for (const auto& [id, p] : pads_) on = on || p.button[size_t(m.button)] || (m.stick >= 0 && p.stick[m.stick]);
      emit(io, m.key, on, on ? 1.0f : 0.0f);
    }
    // Triggers (hold rewind / fast-forward on the hub) and the analog sticks.
    struct Analog {
      ImGuiKey key;
      int axis;
      float sign;
    };
    static const Analog analogs[] = {
        {ImGuiKey_GamepadLStickLeft, SDL_GAMEPAD_AXIS_LEFTX, -1},  {ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX, 1},
        {ImGuiKey_GamepadLStickUp, SDL_GAMEPAD_AXIS_LEFTY, -1},    {ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY, 1},
        {ImGuiKey_GamepadRStickLeft, SDL_GAMEPAD_AXIS_RIGHTX, -1}, {ImGuiKey_GamepadRStickRight, SDL_GAMEPAD_AXIS_RIGHTX, 1},
        {ImGuiKey_GamepadRStickUp, SDL_GAMEPAD_AXIS_RIGHTY, -1},   {ImGuiKey_GamepadRStickDown, SDL_GAMEPAD_AXIS_RIGHTY, 1},
    };
    for (const Analog& a : analogs) {
      float v = 0;
      for (const auto& [id, p] : pads_) v = std::max(v, p.axis[size_t(a.axis)] * a.sign);
      v = v < 0.25f ? 0.0f : (v - 0.25f) / 0.75f;  // dead zone
      emit(io, a.key, v > 0.1f, v);
    }
    bool l2 = false, r2 = false;
    for (const auto& [id, p] : pads_) {
      l2 = l2 || p.trigger[0];
      r2 = r2 || p.trigger[1];
    }
    emit(io, ImGuiKey_GamepadL2, l2, l2 ? 1.0f : 0.0f);
    emit(io, ImGuiKey_GamepadR2, r2, r2 ? 1.0f : 0.0f);
  }

  void emit(ImGuiIO& io, ImGuiKey key, bool down, float value) {
    current_[key] = {down, value};
    if (suppressed_) down = false, value = 0.0f;
    if (held_.count(key)) {
      if (down) down = false, value = 0.0f;
      else held_.erase(key);
    }
    auto it = sent_.find(key);
    if (it != sent_.end() && it->second.first == down && std::fabs(it->second.second - value) < 0.02f) return;
    sent_[key] = {down, value};
    io.AddKeyAnalogEvent(key, down, value);
    if (down) activity_ = true;
  }

  std::map<SDL_JoystickID, Pad> pads_;
  std::map<ImGuiKey, std::pair<bool, float>> sent_;
  std::map<ImGuiKey, std::pair<bool, float>> current_;  // the pads' state (also while suppressed)
  std::set<ImGuiKey> held_;                             // down when suppression ended: fed after release
  bool suppressed_ = false;
  bool activity_ = false;
};

}  // namespace rnl
