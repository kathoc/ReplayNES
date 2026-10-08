// SPDX-License-Identifier: GPL-2.0-or-later
#include "input_router.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "settings.h"

namespace rnl {

namespace {
std::string readFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
std::string slotPrefix(int slot) { return "gc" + std::to_string(slot) + ":"; }
}  // namespace

InputRouter::InputRouter(std::string bindingsFile, Settings* settings)
    : in_(rn_input_new()), file_(std::move(bindingsFile)), settings_(settings), chord_(rnf_chord_new(0)) {}

InputRouter::~InputRouter() {
  closeAll();
  rnf_chord_free(chord_);
  rn_input_free(in_);
}

void InputRouter::closeAll() {
  for (PadInfo& p : pads_) {
    if (p.pad) SDL_CloseGamepad(p.pad);
    p = PadInfo();
  }
}

bool InputRouter::applyPlan(rnf_list* unbind, rnf_list* bind) {
  bool did = rnf_input_apply_plan(in_, unbind, bind) != 0;
  rnf_list_free(unbind);
  rnf_list_free(bind);
  return did;
}

void InputRouter::load() {
  std::string text = readFile(file_);
  bool loaded = !text.empty() && rn_input_load_json(in_, text.c_str()) == RN_OK;
  if (!loaded) {
    char* def = rnf_input_default_config_json(RNF_KEYBOARD_SDL);
    rn_input_load_json(in_, def);
    rnf_string_free(def);
    if (settings_) settings_->controllerLayoutVersion = RNF_CONTROLLER_LAYOUT_VERSION;
    refreshBindings();
    return;
  }
  refreshBindings();
  // One-time upgrades of saved bindings, unless the user customised the part concerned.
  int from = settings_ ? settings_->controllerLayoutVersion : RNF_CONTROLLER_LAYOUT_VERSION;
  if (from < RNF_CONTROLLER_LAYOUT_VERSION) {
    bool changed = false;
    rnf_list *u = nullptr, *b = nullptr;
    if (from < 2) {
      rnf_input_controller_layout_migration(bindingView_.data(), bindingView_.size(), &u, &b);
      changed = applyPlan(u, b) || changed;
      refreshBindings();
    }
    if (from < 3) {
      rnf_input_face_layout_migration(bindingView_.data(), bindingView_.size(), &u, &b);
      changed = applyPlan(u, b) || changed;
      refreshBindings();
    }
    if (from < 4) {  // L2 rewind / R2 fast-forward (untouched layout-3 triggers only)
      rnf_input_trigger_swap_migration(bindingView_.data(), bindingView_.size(), &u, &b);
      changed = applyPlan(u, b) || changed;
      refreshBindings();
    }
    if (from < 5) {  // the Quick Menu: L+R and Esc (R keeps pausing)
      rnf_input_menu_migration(bindingView_.data(), bindingView_.size(), RNF_KEYBOARD_SDL, &u, &b);
      changed = applyPlan(u, b) || changed;
      refreshBindings();
    }
    if (settings_) settings_->controllerLayoutVersion = RNF_CONTROLLER_LAYOUT_VERSION;
    if (changed) persist();
  }
}

void InputRouter::refreshBindings() {
  bindingPairs_.clear();
  char* json = rn_input_save_json(in_);
  rnf_input_config* c = nullptr;
  if (json && rnf_input_config_parse(json, &c) == RN_OK) {
    for (size_t i = 0, n = rnf_input_config_binding_count(c); i < n; ++i) {
      rnf_binding b{};
      if (rnf_input_config_binding_get(c, i, &b)) bindingPairs_.push_back({b.input, b.action});
    }
    turboPeriod_ = rnf_input_config_turbo_period(c);
    turboDuty_ = rnf_input_config_turbo_duty(c);
    socd_ = rnf_input_config_socd(c);
    analogThreshold_ = rnf_input_config_analog_threshold(c);
    rnf_input_config_free(c);
  }
  rn_string_free(json);
  bindingView_.clear();
  for (const auto& p : bindingPairs_) bindingView_.push_back(rnf_binding{p.first.c_str(), p.second.c_str()});
  stepDirs_.clear();
  rnf_list* dirs = rnf_input_paused_step_directions(bindingView_.data(), bindingView_.size());
  for (size_t i = 0, n = rnf_list_count(dirs); i < n; ++i) stepDirs_[rnf_list_a(dirs, i)] = int(rnf_list_value(dirs, i));
  rnf_list_free(dirs);
  // The Quick Menu: combos go through the chord detector, single inputs open it directly.
  rnf_chord_configure(chord_, bindingView_.data(), bindingView_.size(), "hk.menu");
  menuIds_.clear();
  for (const rnf_binding& b : bindingView_)
    if (std::string(b.action) == "hk.menu" && !rnf_input_combo_split(b.input, nullptr, nullptr)) menuIds_.insert(b.input);
}

void InputRouter::persist() {
  char* json = rn_input_save_json(in_);
  if (json) {
    std::string tmp = file_ + ".tmp";
    {
      std::ofstream f(tmp, std::ios::trunc);
      f << json;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_, ec);  // replaces the old file (also on Windows)
    if (ec) std::fprintf(stderr, "cannot save %s\n", file_.c_str());
    rn_string_free(json);
  }
  refreshBindings();
}

void InputRouter::bind(const std::string& physical, const std::string& action) {
  rn_input_bind(in_, physical.c_str(), action.c_str());
  persist();
}
void InputRouter::unbind(const std::string& physical, const std::string& action) {
  rn_input_unbind(in_, physical.c_str(), action.c_str());
  persist();
}
void InputRouter::setAssignment(const std::string& physical, const std::string& action) {
  rn_input_unbind(in_, physical.c_str(), nullptr);
  if (!action.empty()) rn_input_bind(in_, physical.c_str(), action.c_str());
  persist();
}
void InputRouter::resetController(int slot) {
  rnf_list *u = nullptr, *b = nullptr;
  rnf_input_controller_reset_plan(bindingView_.data(), bindingView_.size(), slot, &u, &b);
  applyPlan(u, b);
  persist();
}
void InputRouter::resetToDefaults() {
  char* def = rnf_input_default_config_json(RNF_KEYBOARD_SDL);
  rn_input_load_json(in_, def);
  rnf_string_free(def);
  persist();
}
void InputRouter::setTurbo(int period, int duty) {
  int p = std::max(1, period), d = std::min(std::max(1, duty), p);
  rn_input_set_turbo(in_, uint32_t(p), uint32_t(d));
  persist();
}
void InputRouter::setSOCD(const std::string& policy) {
  rn_socd v = policy == "last_wins" ? RN_SOCD_LAST_WINS : policy == "allow" ? RN_SOCD_ALLOW : RN_SOCD_NEUTRAL;
  rn_input_set_socd(in_, v);
  persist();
}
void InputRouter::setAnalogThreshold(double t) {
  rn_input_set_analog_threshold(in_, float(std::min(std::max(t, 0.05), 0.95)));
  persist();
}

void InputRouter::beginCapture(std::function<void(const std::string&)> fn, bool keysOnly) {
  capture_ = std::move(fn);
  captureKeysOnly_ = keysOnly;
}
void InputRouter::cancelCapture() {
  if (!capture_) return;
  auto fn = std::move(capture_);
  capture_ = nullptr;
  fn("");
}

// A press while "press a key / button" waits: true when the capture took it.
bool InputRouter::captureFrom(const std::string& id, bool keyboard) {
  if (!capture_) return false;
  if (captureKeysOnly_ && !keyboard) {
    // Keys only: a controller's B cancels, anything else waits for a key.
    if (id.find(":face.east") != std::string::npos) cancelCapture();
    return true;
  }
  auto fn = std::move(capture_);
  capture_ = nullptr;
  fn(id);
  return true;
}

int InputRouter::connectedCount() const {
  int n = 0;
  for (const PadInfo& p : pads_) n += p.pad ? 1 : 0;
  return n;
}

void InputRouter::setUIMode(bool ui) {
  if (ui == ui_) return;
  ui_ = ui;
  if (ui) {
    rn_input_release_all(in_);  // nothing held for the game while the menu is up
    routedSteps_.clear();
  }
}

void InputRouter::setPausedStepMode(bool paused) {
  bool changed = pausedStepMode_ != paused;
  pausedStepMode_ = paused;
  if (!changed || !paused) return;
  for (const auto& [id, dir] : stepDirs_) {
    if (pressed_.count(id) && !routedSteps_.count(id)) {
      rn_input_set_pressed(in_, id.c_str(), 0);
      routedSteps_.insert(id);  // its release is then swallowed too
    }
  }
}

bool InputRouter::routePausedStep(const std::string& id, bool down) {
  auto it = stepDirs_.find(id);
  if (it == stepDirs_.end()) return false;
  bool active = pausedStepMode_ && (!settings_ || settings_->dpadStepWhenPaused);
  if (down) {
    if (!active) return false;
    routedSteps_.insert(id);
    if (onPausedStep) onPausedStep(it->second, true);
    return true;
  }
  if (routedSteps_.erase(id)) {
    if (onPausedStep) onPausedStep(it->second, false);
    return true;
  }
  return false;
}

bool InputRouter::routePausedConfirm(const std::string& id, bool down) {
  if (down) {
    // Paused in play (the seek bar): A / B resume; nothing reaches the game.
    if (ui_ || !pausedStepMode_) return false;
    size_t colon = id.find(':');
    if (colon == std::string::npos || id.compare(0, 2, "gc") != 0) return false;
    std::string el = id.substr(colon + 1);
    if (el != "face.south" && el != "face.east") return false;
    routedConfirm_.insert(id);
    if (onPausedConfirm) onPausedConfirm();
    return true;
  }
  return routedConfirm_.erase(id) > 0;
}

// The chord detector's outcome: the Quick Menu, or L / R alone going their usual way.
void InputRouter::pumpChord() {
  rnf_chord_event ev;
  while (rnf_chord_poll(chord_, &ev)) {
    std::string id = ev.input;
    switch (ev.kind) {
      case RNF_CHORD_COMBO_DOWN:
        if (onMenuButton) onMenuButton();
        break;
      case RNF_CHORD_COMBO_UP: break;
      case RNF_CHORD_ALONE_DOWN:
      case RNF_CHORD_ALONE_UP: routeButton(id, ev.kind == RNF_CHORD_ALONE_DOWN, ev.time); break;
    }
  }
}

void InputRouter::tick(double now) {
  rnf_chord_tick(chord_, now);
  pumpChord();
}

// A button after the menu / chord / capture checks: the UI's shoulders, paused stepping and
// confirming, or the game.
void InputRouter::routeButton(const std::string& id, bool down, double t) {
  if (ui_) {
    setPressed(id, down, t, false);
    size_t colon = id.find(':');
    std::string el = colon == std::string::npos ? id : id.substr(colon + 1);
    if (down && onUiShoulder && (el == "leftShoulder" || el == "rightShoulder")) onUiShoulder(el == "leftShoulder" ? -1 : 1);
    return;
  }
  if (routePausedStep(id, down) || routePausedConfirm(id, down)) {
    if (down) pressed_.insert(id);
    else pressed_.erase(id);
    return;
  }
  setPressed(id, down, t, true);
}

void InputRouter::setPressed(const std::string& id, bool down, double t, bool game) {
  if (down) pressed_.insert(id);
  else pressed_.erase(id);
  if (!game) return;
  rn_input_set_pressed(in_, id.c_str(), down ? 1 : 0);
  if (down) {
    pressSeq_.fetch_add(1);
    lastEvent_.store(t);
  }
}

int InputRouter::slotOf(SDL_JoystickID id) const {
  for (int s = 0; s < kSlots; ++s)
    if (pads_[s].pad && pads_[s].id == id) return s;
  return -1;
}

void InputRouter::attach(SDL_JoystickID id) {
  if (slotOf(id) >= 0) return;
  for (int s = 0; s < kSlots; ++s) {
    PadInfo& p = pads_[s];
    if (p.pad) continue;
    p.pad = SDL_OpenGamepad(id);
    if (!p.pad) return;
    p.id = id;
    const char* n = SDL_GetGamepadName(p.pad);
    p.name = n ? n : "";
    p.family = rnf_sdl_controller_family(int(SDL_GetGamepadType(p.pad)), p.name.c_str());
    p.labels.clear();
    for (int b : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH}) {
      const char* l = rnf_sdl_button_label_text(int(SDL_GetGamepadButtonLabel(p.pad, SDL_GamepadButton(b))));
      if (l) p.labels[rnf_sdl_button_element(b)] = l;
    }
    std::fprintf(stderr, "gamepad: %s (slot %d, type %d)\n", p.name.c_str(), s, int(SDL_GetGamepadType(p.pad)));
    if (onPadsChanged) onPadsChanged();
    return;
  }
}

void InputRouter::detach(SDL_JoystickID id) {
  int s = slotOf(id);
  if (s < 0) return;
  std::string name = pads_[s].name;
  if (lastSlot_ == s) lastSlot_ = -1;
  SDL_CloseGamepad(pads_[s].pad);
  pads_[s] = PadInfo();
  std::string prefix = slotPrefix(s);
  rn_input_release_prefix(in_, prefix.c_str());
  rnf_chord_reset(chord_);
  for (auto it = routedConfirm_.begin(); it != routedConfirm_.end();)
    it = it->compare(0, prefix.size(), prefix) == 0 ? routedConfirm_.erase(it) : std::next(it);
  for (auto it = routedSteps_.begin(); it != routedSteps_.end();) {
    if (it->compare(0, prefix.size(), prefix) == 0) {
      auto d = stepDirs_.find(*it);
      if (d != stepDirs_.end() && onPausedStep) onPausedStep(d->second, false);
      it = routedSteps_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = pressed_.begin(); it != pressed_.end();)
    it = it->compare(0, prefix.size(), prefix) == 0 ? pressed_.erase(it) : std::next(it);
  if (onPadsChanged) onPadsChanged();
  if (onDisconnect) onDisconnect(name);
}

void InputRouter::padButton(int slot, int button, bool down, double t) {
  const char* el = rnf_sdl_button_element(button);
  if (!el) return;
  std::string id = slotPrefix(slot) + el;
  if (down) lastSlot_ = slot;
  if (isReserved(el)) {
    if (down) pressed_.insert(id);
    else pressed_.erase(id);
    if (down && onMenuButton) onMenuButton();
    return;
  }
  if (down && capture_) {
    pressed_.insert(id);
    captureFrom(id, false);
    return;
  }
  // The Quick Menu: L+R (chord detector) or a button of its own.
  if (rnf_chord_feed(chord_, id.c_str(), down ? 1 : 0, t)) {
    if (down) pressed_.insert(id);
    else pressed_.erase(id);
    pumpChord();
    return;
  }
  pumpChord();  // a member held alone may have fired just now
  if (menuIds_.count(id)) {
    if (down) pressed_.insert(id);
    else pressed_.erase(id);
    if (down && onMenuButton) onMenuButton();
    return;
  }
  routeButton(id, down, t);
}

void InputRouter::stickDirections(int slot, const char* stick, float x, float y, double t) {
  // Virtual direction ids for the diagram and capture (threshold 0.5; y > 0 = up).
  const float th = 0.5f;
  const std::pair<const char*, bool> dirs[] = {{"left", x <= -th}, {"right", x >= th}, {"up", y >= th}, {"down", y <= -th}};
  for (const auto& [d, on] : dirs) {
    std::string id = slotPrefix(slot) + stick + "." + d;
    bool& st = stickState_[id];
    if (st == on) continue;
    st = on;
    if (on) pressed_.insert(id);
    else pressed_.erase(id);
    if (on && capture_) {
      captureFrom(id, false);
    } else if (on && !ui_) {
      pressSeq_.fetch_add(1);
      lastEvent_.store(t);
    }
  }
}

void InputRouter::padAxis(int slot, int axis, float v, double t) {
  PadInfo& p = pads_[slot];
  std::string g = slotPrefix(slot);
  if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    bool right = axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    bool& state = right ? p.rt : p.lt;
    bool now = state ? v > 0.35f : v > 0.5f;  // hysteresis
    if (now == state) return;
    state = now;
    if (now) lastSlot_ = slot;
    std::string id = g + (right ? "rightTrigger" : "leftTrigger");
    if (now && capture_) {
      pressed_.insert(id);
      captureFrom(id, false);
      return;
    }
    setPressed(id, now, t, !ui_);
    return;
  }
  bool left = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY;
  if (axis == SDL_GAMEPAD_AXIS_LEFTX) p.lx = v;
  else if (axis == SDL_GAMEPAD_AXIS_LEFTY) p.ly = v;
  else if (axis == SDL_GAMEPAD_AXIS_RIGHTX) p.rx = v;
  else if (axis == SDL_GAMEPAD_AXIS_RIGHTY) p.ry = v;
  else return;
  float x = left ? p.lx : p.rx, y = -(left ? p.ly : p.ry);
  const char* stick = left ? "lstick" : "rstick";
  if (!ui_) rn_input_set_axis(in_, (g + stick).c_str(), x, y);
  stickDirections(slot, stick, x, y, t);
}

void InputRouter::handleEvent(const SDL_Event& e, double evTime, bool keyboardForUI) {
  switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: attach(e.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: detach(e.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      int s = slotOf(e.gbutton.which);
      if (s >= 0) padButton(s, e.gbutton.button, e.gbutton.down, evTime);
      break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      int s = slotOf(e.gaxis.which);
      if (s >= 0) padAxis(s, e.gaxis.axis, float(e.gaxis.value) / 32767.0f, evTime);
      break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      if (e.key.repeat) break;
      bool down = e.type == SDL_EVENT_KEY_DOWN;
      SDL_Scancode sc = e.key.scancode;
      std::string id = "kb:" + std::to_string(int(sc));
      if (down && capture_) {
        if (sc == SDL_SCANCODE_ESCAPE) cancelCapture();
        else captureFrom(id, true);
        break;
      }
      // The Quick Menu: its keys (Esc by default) and F1. Never game input.
      if (sc == SDL_SCANCODE_F1 || menuIds_.count(id)) {
        if (down && onMenuButton) onMenuButton();
        break;
      }
      bool game = !ui_ && !keyboardForUI;
      // A release always reaches the engine (a key held when the menu opened was released then).
      if (!down) {
        pressed_.erase(id);
        if (!ui_) rn_input_set_pressed(in_, id.c_str(), 0);
        break;
      }
      setPressed(id, true, evTime, game);
      break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      rn_input_release_prefix(in_, "kb:");  // the keyboard is foreground-only
      for (auto it = pressed_.begin(); it != pressed_.end();)
        it = it->compare(0, 3, "kb:") == 0 ? pressed_.erase(it) : std::next(it);
      break;
    default: break;
  }
}

}  // namespace rnl
