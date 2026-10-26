// SPDX-License-Identifier: GPL-2.0-or-later
#include "input_router.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../../engine/src/util/Json.h"
#include "host_clock.h"
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
    : in_(rn_input_new()), file_(std::move(bindingsFile)), baseFile_(file_), settings_(settings), chord_(rnf_chord_new(0)) {
  profiles_[0] = rn_input_new();
  profiles_[1] = rn_input_new();
  loadKindOverrides();
}

InputRouter::~InputRouter() {
  closeAll();
  rnf_chord_free(chord_);
  rn_input_free(in_);
  for (auto* profile : profiles_) rn_input_free(profile);
}

void InputRouter::closeAll() {
  for (PadInfo& p : pads_) {
    if (p.pad) SDL_CloseGamepad(p.pad);
    p = PadInfo();
  }
  if (standby_.pad) SDL_CloseGamepad(standby_.pad);
  standby_ = PadInfo();
}

bool InputRouter::applyPlan(rnf_list* unbind, rnf_list* bind) {
  bool did = rnf_input_apply_plan(in_, unbind, bind) != 0;
  rnf_list_free(unbind);
  rnf_list_free(bind);
  return did;
}

DeviceKind InputRouter::detectDeviceKind(SDL_JoystickType type, const std::string& name) {
  std::string lower = name;
  for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (lower.find("steam deck") != std::string::npos) return DeviceKind::pad;
  if (type == SDL_JOYSTICK_TYPE_ARCADE_STICK) return DeviceKind::stick;
  for (const char* match : {"arcade", "fight stick", "fightstick", "fighting stick", "fighting commander",
       "hori rap", "real arcade", "qanba", "victrix", "panthera", "mayflash f", "8bitdo arcade",
       "astro city", "egret", "hitbox", "hit box", "snack box", "brook", "neogeo arcade"})
    if (lower.find(match) != std::string::npos) return DeviceKind::stick;
  return DeviceKind::pad;
}

KindOverride InputRouter::kindOverride(int slot) const {
  if (slot < 0 || slot >= kSlots) return KindOverride::automatic;
  auto it = kindOverrides_.find(pads_[slot].guid);
  return it == kindOverrides_.end() ? KindOverride::automatic : it->second;
}
DeviceKind InputRouter::deviceKind(int slot) const {
  if (nesLayout_ || slot < 0 || slot >= kSlots || !pads_[slot].pad) return DeviceKind::pad;
  auto override = kindOverride(slot);
  if (override != KindOverride::automatic) return override == KindOverride::stick ? DeviceKind::stick : DeviceKind::pad;
  return pads_[slot].detectedKind;
}
DeviceKind InputRouter::editingKind() const { return deviceKind(editingSlot_ < 0 ? lastSlot_ : editingSlot_); }
rn_input* InputRouter::editingInput() const { return nesLayout_ ? in_ : profiles_[editingKind() == DeviceKind::stick ? 1 : 0]; }
void InputRouter::noteLastSlot(int slot) {
  const DeviceKind previous = editingKind();
  lastSlot_ = slot;
  if (!nesLayout_ && editingSlot_ < 0 && editingKind() != previous) refreshBindings(false);
}
void InputRouter::setEditingSlot(int slot) {
  if (slot < -1 || slot >= kSlots || (slot >= 0 && editingSlot_ == slot)) return;
  editingSlot_ = slot;
  refreshBindings(false);
}
std::string InputRouter::kindFile(DeviceKind kind) const {
  if (nesLayout_) return file_;
  char* name = rnf_input_bindings_filename_for_kind(kind == DeviceKind::stick);
  std::string path = (std::filesystem::path(baseFile_).parent_path() / name).string();
  rnf_string_free(name);
  return path;
}
void InputRouter::loadKindOverrides() {
  rn::Json root;
  if (!rn::Json::parse(readFile((std::filesystem::path(baseFile_).parent_path() / "controller-kinds.json").string()), root)) return;
  for (const auto& [guid, value] : root.members()) {
    if (value.asString() == "pad") kindOverrides_[guid] = KindOverride::pad;
    else if (value.asString() == "stick") kindOverrides_[guid] = KindOverride::stick;
  }
}
void InputRouter::persistKindOverrides() {
  rn::Json root = rn::Json::object();
  for (const auto& [guid, kind] : kindOverrides_) root.set(guid, kind == KindOverride::stick ? "stick" : "pad");
  auto path = std::filesystem::path(baseFile_).parent_path() / "controller-kinds.json";
  const std::string tmp = path.string() + ".tmp";
  { std::ofstream out(tmp); out << root.dump(); }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) std::fprintf(stderr, "cannot save %s\n", path.string().c_str());
}
void InputRouter::setKindOverride(int slot, KindOverride kind) {
  if (slot < 0 || slot >= kSlots || pads_[slot].guid.empty() || nesLayout_) return;
  if (kind == KindOverride::automatic) kindOverrides_.erase(pads_[slot].guid);
  else kindOverrides_[pads_[slot].guid] = kind;
  persistKindOverrides();
  rebuildRuntime();
  refreshBindings();
  if (onPadsChanged) onPadsChanged();
}
void InputRouter::rebuildRuntime() {
  if (nesLayout_) return;
  // The engine receives a transient composition, never persisted: P1 supplies globals/keyboard,
  // while each controller prefix comes from that slot's physical device kind.
  rn::Json configs[2];
  for (int k = 0; k < 2; ++k) {
    char* text = rn_input_save_json(profiles_[k]);
    if (text) rn::Json::parse(text, configs[k]);
    rn_string_free(text);
  }
  int primary = deviceKind(0) == DeviceKind::stick ? 1 : 0;
  rn::Json runtime = configs[primary], bindings = rn::Json::array();
  for (int k = 0; k < 2; ++k) for (const auto& b : configs[k]["bindings"].items()) {
    int slot = -1;
    if (rnf_input_controller_slot(b["input"].asString().c_str(), &slot)) {
      if (k == (deviceKind(slot) == DeviceKind::stick ? 1 : 0)) bindings.push(b);
    } else if (k == primary) bindings.push(b);
  }
  runtime.set("bindings", bindings);
  mouseGame_ = false;
  for (const auto& b : bindings.items())
    mouseGame_ = mouseGame_ || (b["input"].asString().compare(0, 3, "ms:") == 0 && b["action"].asString().compare(0, 3, "hk.") != 0);
  rn::Json layout;
  stickDeadzone_ = dialDeadzone_ = 0;
  if (rn::Json::parse(layoutJson_, layout))
    for (const auto& a : layout["axes"].items())
      (a["relative"].asBool(false) ? dialDeadzone_ : stickDeadzone_) =
          std::max(a["relative"].asBool(false) ? dialDeadzone_ : stickDeadzone_, float(a["deadzone"].asDouble(0)));
  rn_input_release_all(in_);
  rn_input_load_json(in_, runtime.dump().c_str());
  setMouseAxes();
  pressed_.clear();
  stickState_.clear();
  for (auto& p : pads_) { p.lt = p.rt = false; p.lx = p.ly = p.rx = p.ry = 0; }
  rnf_chord_reset(chord_);
  confirmTap_.clear();
  swallowedStarts_.clear();
  practiceStops_.clear();
  for (const auto& [id, dir] : holdMembers_) if (onMarkerHold) onMarkerHold(dir, false);
  for (const auto& id : routedSteps_) {
    auto it = stepDirs_.find(id);
    if (it != stepDirs_.end() && onPausedStep) onPausedStep(it->second, false);
  }
  routedSteps_.clear();
}

void InputRouter::setLayout(const char* json) {
  const std::string next = json ? json : "";
  if (next == layoutJson_) return;
  if (rnf_input_set_layout_json(json) != RN_OK) return;
  const bool nes = std::strcmp(rnf_input_layout_name(), "nes") == 0;
  // The existing NES input configuration is left untouched between NES sessions.
  if (nes && nesLayout_) { layoutJson_ = next; return; }
  if (rn_input_set_layout_json(in_, json) != RN_OK) return;
  layoutJson_ = next;
  nesLayout_ = nes;
  char* filename = rnf_input_bindings_filename();
  file_ = (std::filesystem::path(baseFile_).parent_path() / filename).string();
  rnf_string_free(filename);
  rn_input_release_all(in_);
  pressed_.clear();
  stickState_.clear();
  load();
}

bool InputRouter::buttonTurbo(const std::string& action, int& period, int& duty) const {
  auto it = buttonTurbo_.find(action);
  if (it == buttonTurbo_.end()) { period = turboPeriod_; duty = turboDuty_; return false; }
  period = it->second.first;
  duty = it->second.second;
  return true;
}

void InputRouter::setButtonTurbo(const std::string& action, int period, int duty) {
  rn_input_set_button_turbo(editingInput(), action.c_str(), uint32_t(period), uint32_t(duty));
  persist();
}

void InputRouter::load() {
  if (!nesLayout_) {
    for (int k = 0; k < 2; ++k) {
      rn_input_set_layout_json(profiles_[k], layoutJson_.c_str());
      std::string text = readFile(kindFile(k ? DeviceKind::stick : DeviceKind::pad));
      if (text.empty() || rn_input_load_json(profiles_[k], text.c_str()) != RN_OK) {
        char* def = rnf_input_default_config_json_for_kind(RNF_KEYBOARD_SDL, k);
        rn_input_load_json(profiles_[k], def);
        rnf_string_free(def);
      } else if (char* more = rnf_input_add_device_defaults(text.c_str(), RNF_KEYBOARD_SDL, k)) {
        rn_input_load_json(profiles_[k], more);  // saved before the keyboard / mouse had defaults
        rnf_string_free(more);
      }
    }
    rebuildRuntime();
    refreshBindings();
    return;
  }
  std::string text = readFile(file_);
  bool loaded = !text.empty() && rn_input_load_json(in_, text.c_str()) == RN_OK;
  if (!loaded) {
    char* def = rnf_input_default_config_json_for_kind(RNF_KEYBOARD_SDL, editingKind() == DeviceKind::stick);
    rn_input_load_json(in_, def);
    rnf_string_free(def);
    if (settings_) settings_->controllerLayoutVersion = RNF_CONTROLLER_LAYOUT_VERSION;
    refreshBindings();
    return;
  }
  refreshBindings();
  // One-time upgrades of saved bindings, unless the user customised the part concerned.
  int from = settings_ ? settings_->controllerLayoutVersion : RNF_CONTROLLER_LAYOUT_VERSION;
  if (nesLayout_ && from < RNF_CONTROLLER_LAYOUT_VERSION) {
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
    if (from < 6) {  // HOME / guide is never assignable: drop bindings to it
      rnf_input_home_migration(bindingView_.data(), bindingView_.size(), &u, &b);
      changed = applyPlan(u, b) || changed;
      refreshBindings();
    }
    if (from < 7) {  // L3 = Undo of a retry (hk.undo_take), unless L3 / the undo is already bound
      rnf_input_undo_migration(bindingView_.data(), bindingView_.size(), &u, &b);
      changed = applyPlan(u, b) || changed;
      refreshBindings();
    }
    if (from < 8) {  // L slow / R pause dropped: L / R alone do nothing in play (no misfires)
      rnf_input_shoulder_step_migration(bindingView_.data(), bindingView_.size(), &u, &b);
      changed = applyPlan(u, b) || changed;
      refreshBindings();
    }
    if (settings_) settings_->controllerLayoutVersion = RNF_CONTROLLER_LAYOUT_VERSION;
    if (changed) persist();
  }
}

void InputRouter::refreshBindings(bool configureRuntime) {
  bindingPairs_.clear();
  buttonTurbo_.clear();
  char* json = rn_input_save_json(editingInput());
  rnf_input_config* c = nullptr;
  if (json && rnf_input_config_parse(json, &c) == RN_OK) {
    for (size_t i = 0, n = rnf_input_config_binding_count(c); i < n; ++i) {
      rnf_binding b{};
      if (rnf_input_config_binding_get(c, i, &b)) bindingPairs_.push_back({b.input, b.action});
    }
    for (size_t i = 0; i < rnf_input_button_count(); ++i) {
      const char* id = rnf_input_button_id(i);
      int period = 0, duty = 0;
      if (rnf_input_config_button_turbo(c, id, &period, &duty)) buttonTurbo_[id] = {period, duty};
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
  if (configureRuntime) configureRuntimeBindings();
}

void InputRouter::configureRuntimeBindings() {
  std::vector<std::pair<std::string, std::string>> pairs;
  std::vector<rnf_binding> view;
  char* json = rn_input_save_json(in_);
  rnf_input_config* config = nullptr;
  if (json && rnf_input_config_parse(json, &config) == RN_OK) {
    for (size_t i = 0; i < rnf_input_config_binding_count(config); ++i) {
      rnf_binding b{};
      if (rnf_input_config_binding_get(config, i, &b)) pairs.emplace_back(b.input, b.action);
    }
    rnf_input_config_free(config);
  }
  rn_string_free(json);
  for (const auto& p : pairs) view.push_back({p.first.c_str(), p.second.c_str()});
  stepDirs_.clear();
  rnf_list* dirs = rnf_input_paused_step_directions(view.data(), view.size());
  for (size_t i = 0, n = rnf_list_count(dirs); i < n; ++i) stepDirs_[rnf_list_a(dirs, i)] = int(rnf_list_value(dirs, i));
  rnf_list_free(dirs);
  // The Quick Menu: combos go through the chord detector, single inputs open it directly.
  rnf_chord_configure(chord_, view.data(), view.size(), "hk.menu");
  menuIds_.clear();
  gameMembers_.clear();
  for (const rnf_binding& b : view) {
    if (std::string(b.action) == "hk.menu" && !rnf_input_combo_split(b.input, nullptr, nullptr)) menuIds_.insert(b.input);
    // Game buttons and arcade transport hotkeys stay held after the chord window.
    bool heldTransport = !nesLayout_ && (std::strcmp(b.action, "hk.rewind") == 0 ||
                                         std::strcmp(b.action, "hk.fast_forward") == 0);
    if (rnf_chord_is_member(chord_, b.input) && (std::strncmp(b.action, "hk.", 3) != 0 || heldTransport))
      gameMembers_.insert(b.input);
  }
  forwardedMembers_.clear();
  holdMembers_.clear();
  updateRepeat();
}

void InputRouter::persist() {
  char* json = rn_input_save_json(editingInput());
  if (json) {
    std::string tmp = kindFile(editingKind()) + ".tmp";
    {
      std::ofstream f(tmp, std::ios::trunc);
      f << json;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, kindFile(editingKind()), ec);  // replaces the old file (also on Windows)
    if (ec) std::fprintf(stderr, "cannot save %s\n", file_.c_str());
    rn_string_free(json);
  }
  if (!nesLayout_) rebuildRuntime();
  refreshBindings();
}

void InputRouter::bind(const std::string& physical, const std::string& action) {
  rn_input_bind(editingInput(), physical.c_str(), action.c_str());
  persist();
}
void InputRouter::unbind(const std::string& physical, const std::string& action) {
  rn_input_unbind(editingInput(), physical.c_str(), action.c_str());
  persist();
}
void InputRouter::setAssignment(const std::string& physical, const std::string& action) {
  rn_input_unbind(editingInput(), physical.c_str(), nullptr);
  if (!action.empty()) rn_input_bind(editingInput(), physical.c_str(), action.c_str());
  persist();
}
void InputRouter::resetController(int slot) {
  if (!nesLayout_) {
    char* def = rnf_input_default_config_json_for_kind(RNF_KEYBOARD_SDL, editingKind() == DeviceKind::stick);
    rnf_input_config* config = nullptr;
    if (def && rnf_input_config_parse(def, &config) == RN_OK) {
      const std::string prefix = slotPrefix(slot);
      for (const auto& p : bindingPairs_)
        if (p.first.find(prefix) == 0) rn_input_unbind(editingInput(), p.first.c_str(), nullptr);
      for (size_t i = 0; i < rnf_input_config_binding_count(config); ++i) {
        rnf_binding b{};
        if (rnf_input_config_binding_get(config, i, &b) && std::string(b.input).find(prefix) == 0)
          rn_input_bind(editingInput(), b.input, b.action);
      }
      rnf_input_config_free(config);
    }
    rnf_string_free(def);
    persist();
    return;
  }
  rnf_list *u = nullptr, *b = nullptr;
  rnf_input_controller_reset_plan(bindingView_.data(), bindingView_.size(), slot, &u, &b);
  applyPlan(u, b);
  persist();
}
void InputRouter::resetToDefaults() {
  char* def = rnf_input_default_config_json_for_kind(RNF_KEYBOARD_SDL, editingKind() == DeviceKind::stick);
  rn_input_load_json(editingInput(), def);
  rnf_string_free(def);
  persist();
}
void InputRouter::setTurbo(int period, int duty) {
  int p = std::max(1, period), d = std::min(std::max(1, duty), p);
  rn_input_set_turbo(editingInput(), uint32_t(p), uint32_t(d));
  persist();
}
void InputRouter::setSOCD(const std::string& policy) {
  rn_socd v = policy == "last_wins" ? RN_SOCD_LAST_WINS : policy == "allow" ? RN_SOCD_ALLOW : RN_SOCD_NEUTRAL;
  rn_input_set_socd(editingInput(), v);
  persist();
}
void InputRouter::setAnalogThreshold(double t) {
  rn_input_set_analog_threshold(editingInput(), float(std::min(std::max(t, 0.05), 0.95)));
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
    // Keys only: the controller's cancel button cancels, anything else waits for a key.
    bool south = settings_ && settings_->southConfirm;
    if (id.size() > 3 && id.compare(id.find(':') + 1, std::string::npos, rnf_ui_cancel_element(south)) == 0) cancelCapture();
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
    confirmTap_.clear();
    forwardedMembers_.clear();
  }
  updateRepeat();
}

void InputRouter::setSeekFocus(int focus) {
  if (focus == seekFocus_) return;
  bool entering = seekFocus_ == 0 && focus > 0;
  seekFocus_ = focus;
  if (entering) {
    // The markers have the pad now: nothing stays held for the game.
    rn_input_release_all(in_);
    confirmTap_.clear();
    forwardedMembers_.clear();
    for (const std::string& id : routedSteps_) {
      auto d = stepDirs_.find(id);
      if (d != stepDirs_.end() && onPausedStep) onPausedStep(d->second, false);
    }
    routedSteps_.clear();
  }
  if (focus < 2 && !holdMembers_.empty()) {
    for (const auto& [id, dir] : holdMembers_)
      if (onMarkerHold) onMarkerHold(dir, false);
    holdMembers_.clear();
  }
  updateRepeat();
}

// Paused frame steps repeat while L / R is held (not while a marker is edited: it moves instead).
bool InputRouter::stepRepeatHeld() const {
  if (!pausedStepMode_ || ui_ || seekFocus_ >= 2) return false;
  for (const auto& id : pressed_)
    if (rnf_chord_is_member(chord_, id.c_str())) return true;
  return false;
}

void InputRouter::updateRepeat() { rnf_chord_set_repeat(chord_, pausedStepMode_ && !ui_ && seekFocus_ < 2); }

bool InputRouter::isGameMember(const std::string& id) const { return gameMembers_.count(id) > 0; }

void InputRouter::setPausedStepMode(bool paused) {
  bool changed = pausedStepMode_ != paused;
  pausedStepMode_ = paused;
  updateRepeat();
  if (!changed) return;
  if (!paused) {
    // Play resumes (the resume countdown first): a D-pad direction still held from stepping is the
    // game's again, so it counts from the first frame (docs/design/VTR_UX.md, "Countdown type-ahead").
    for (auto it = routedSteps_.begin(); it != routedSteps_.end();) {
      auto d = stepDirs_.find(*it);
      if (pressed_.count(*it)) rn_input_set_pressed(in_, it->c_str(), 1);
      if (d != stepDirs_.end() && onPausedStep) onPausedStep(d->second, false);
      it = routedSteps_.erase(it);
    }
    return;
  }
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
    confirmTap_.cancel();
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

// Paused in play (the seek bar): taps (rnl::ConfirmTap) of the UI's cancel (resume), confirm (a
// marker), up (to the markers), Y (practice the slot's section) and X (the next A/B slot). They still reach the game, so a button can
// be held for a frame advance; holding one and stepping never counts as a tap.
bool InputRouter::routePausedTap(const std::string& id, bool down) {
  size_t colon = id.find(':');
  if (colon == std::string::npos || id.compare(0, 2, "gc") != 0) return false;
  std::string el = id.substr(colon + 1);
  int slot = 0;
  rnf_input_controller_slot(id.c_str(), &slot);
  if (practicing_ && el == "face.north" && !practiceStopAvailable(slot)) return false;
  bool south = settings_ && settings_->southConfirm;
  if (down) {
    if (ui_ || !pausedStepMode_) return false;
    if (el == rnf_ui_cancel_element(south) || el == rnf_ui_confirm_element(south) || el == "dpad.up" || el == "face.north" ||
        el == "face.west")
      confirmTap_.press(id);
    return false;
  }
  if (!confirmTap_.release(id) || !pausedStepMode_ || ui_) return false;
  if (el == rnf_ui_cancel_element(south)) {
    if (onPausedResume) onPausedResume();
  } else if (onSeekInput) {
    onSeekInput(el == "dpad.up"      ? SeekInput::up
                : el == "face.north" ? SeekInput::y
                : el == "face.west"  ? SeekInput::x
                                     : SeekInput::ok,
                true);
  }
  return true;
}

void InputRouter::setPracticing(bool on) { practicing_ = on; }

bool InputRouter::practiceStopAvailable(int slot) const {
  if (nesLayout_) return true;
  const std::string physical = slotPrefix(slot) + "face.north";
  for (const auto& binding : bindingPairs_)
    if (binding.first == physical && binding.second.compare(0, 3, "hk.") != 0) return false;
  return true;
}

// Practicing: Y (face.north) stops it - while playing, paused or in the countdown. Never game input
// then (its release neither, also when practice has ended meanwhile).
bool InputRouter::routePracticeStop(const std::string& id, bool down) {
  size_t colon = id.find(':');
  if (colon == std::string::npos || id.compare(0, 2, "gc") != 0) return false;
  if (!down) return practiceStops_.erase(id) > 0;
  int slot = 0;
  rnf_input_controller_slot(id.c_str(), &slot);
  if (!practiceStopAvailable(slot)) return false;
  if (!practicing_ || ui_ || id.compare(colon + 1, std::string::npos, "face.north") != 0) return false;
  practiceStops_.insert(id);
  confirmTap_.cancel();
  if (onPracticeStop) onPracticeStop();
  return true;
}

// A marker focused / edited: the pad is the seek bar's (D-pad / left stick, confirm, cancel, X).
bool InputRouter::routeSeek(const std::string& id, bool down) {
  if (ui_ || !pausedStepMode_ || seekFocus_ == 0 || !onSeekInput) return false;
  size_t colon = id.find(':');
  if (colon == std::string::npos || id.compare(0, 2, "gc") != 0) return false;
  std::string el = id.substr(colon + 1);
  bool south = settings_ && settings_->southConfirm;
  if (el == rnf_ui_confirm_element(south)) onSeekInput(SeekInput::ok, down);
  else if (el == rnf_ui_cancel_element(south)) onSeekInput(SeekInput::cancel, down);
  else if (el == "dpad.up" || el == "lstick.up") onSeekInput(SeekInput::up, down);
  else if (el == "dpad.down" || el == "lstick.down") onSeekInput(SeekInput::down, down);
  else if (el == "dpad.left" || el == "lstick.left") onSeekInput(SeekInput::left, down);
  else if (el == "dpad.right" || el == "lstick.right") onSeekInput(SeekInput::right, down);
  else if (el == "face.west") onSeekInput(SeekInput::x, down);
  else if (el == "face.north") onSeekInput(SeekInput::y, down);
  return true;  // nothing reaches the game while a marker has the focus
}

// The chord detector's outcome: the Quick Menu, or L / R alone (on their release).
void InputRouter::pumpChord() {
  rnf_chord_event ev;
  while (rnf_chord_poll(chord_, &ev)) {
    std::string id = ev.input;
    int dir = ev.member == 0 ? -1 : 1;
    int slot = -1;
    if (!nesLayout_ && rnf_input_controller_slot(id.c_str(), &slot) && deviceKind(slot) == DeviceKind::stick) {
      if (id == slotPrefix(slot) + "leftTrigger") dir = -1;
      else if (id == slotPrefix(slot) + "leftShoulder") dir = 1;
    }
    bool paused = pausedStepMode_ && !ui_;
    switch (ev.kind) {
      case RNF_CHORD_COMBO_DOWN:
        if (onMenuButton) onMenuButton();
        break;
      case RNF_CHORD_COMBO_UP: break;
      case RNF_CHORD_ALONE_DOWN:  // held alone: a game button is held from now, a marker moves
        if (paused && seekFocus_ == 2) {
          holdMembers_[id] = dir;
          if (onMarkerHold) onMarkerHold(dir, true);
        } else if (!ui_ && !pausedStepMode_ && isGameMember(id)) {
          forwardedMembers_.insert(id);
          setPressed(id, true, ev.time, true);
        }
        break;
      case RNF_CHORD_ALONE_REPEAT:  // the paused seek bar switches sections one press at a time
        break;
      case RNF_CHORD_ALONE_UP:  // the trigger of a single press
        if (holdMembers_.erase(id)) {
          if (onMarkerHold) onMarkerHold(dir, false);
        } else if (forwardedMembers_.erase(id)) {
          setPressed(id, false, ev.time, true);
        } else if (ev.suppressed) {
          // It was held for a gesture (the other member followed it, or START): no action of its own.
        } else if (ui_) {
          if (onUiShoulder) onUiShoulder(dir);
        } else if (paused) {
          // The practice section: L back, R forward (by the shoulder itself, also on a stick, whose
          // step directions were L2 / L1); L2 / R2 alone switch nothing.
          const std::string el = id.substr(id.find(':') + 1);
          const int section = el == "leftShoulder" ? -1 : el == "rightShoulder" ? 1 : 0;
          if (ev.repeats == 0 && section && onPausedShoulder) onPausedShoulder(section);
        } else {
          // A tap of its own action (none by default since layout 8): the hotkey fires on the press edge.
          setPressed(id, true, ev.time, true);
          setPressed(id, false, ev.time, true);
        }
        break;
      // Pressed while the other member is held on its own: L held + R = +1, R held + L = -1 at the press,
      // repeated while held (EmulationController::followGesture: in play pause / slow, paused a frame
      // step). A member bound to a game button is the game's.
      case RNF_CHORD_FOLLOW_DOWN:
        if (ui_ || (paused && seekFocus_ == 2)) break;
        if (!pausedStepMode_ && isGameMember(id)) {
          forwardedMembers_.insert(id);
          setPressed(id, true, ev.time, true);
        } else if (onPauseGesture) {
          onPauseGesture(dir, false);
        }
        break;
      case RNF_CHORD_FOLLOW_REPEAT:
        if (!ui_ && seekFocus_ < 2 && !forwardedMembers_.count(id) && onPauseGesture) onPauseGesture(dir, true);
        break;
      case RNF_CHORD_FOLLOW_UP:
        if (forwardedMembers_.erase(id)) setPressed(id, false, ev.time, true);
        else if (ui_ && !ev.suppressed && onUiShoulder) onUiShoulder(dir);  // a page, on the release
        break;
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
    // Pads 2-4 (pad 1's L / R come through the chord detector): pages, on the release.
    size_t colon = id.find(':');
    std::string el = colon == std::string::npos ? id : id.substr(colon + 1);
    if (!down && onUiShoulder && (el == "leftShoulder" || el == "rightShoulder")) onUiShoulder(el == "leftShoulder" ? -1 : 1);
    return;
  }
  if (routeSeek(id, down)) {
    if (down) pressed_.insert(id);
    else pressed_.erase(id);
    return;
  }
  if (routePausedStep(id, down)) {
    if (down) pressed_.insert(id);
    else pressed_.erase(id);
    return;
  }
  // The paused seek bar: L / R switch the practice section also where they are not the chord's
  // members (an arcade stick: R1 is a game button, the menu chord L1+L2) - at the press, the game
  // being paused.
  {
    const size_t colon = id.find(':');
    const std::string el = colon == std::string::npos ? std::string() : id.substr(colon + 1);
    if (pausedStepMode_ && seekFocus_ == 0 && (el == "leftShoulder" || el == "rightShoulder") && id.compare(0, 2, "gc") == 0) {
      if (down) {
        pressed_.insert(id);
        confirmTap_.cancel();
        if (onPausedShoulder) onPausedShoulder(el == "leftShoulder" ? -1 : 1);
      } else {
        pressed_.erase(id);
      }
      return;
    }
  }
  setPressed(id, down, t, true);
  routePausedTap(id, down);
}

void InputRouter::setPressed(const std::string& id, bool down, double t, bool game) {
  if (down) {
    pressed_.insert(id);
    confirmTap_.cancel();
  } else {
    pressed_.erase(id);
  }
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

namespace {
// Valve's own controls (not Steam's virtual gamepad, 0x11ff, which wraps every controller under Steam Input):
// the Steam Deck's, the Steam Controller's. Under Steam the real device is known from Steam's virtual gamepad
// info (SteamVirtualGamepadInfo; the Flatpak may read that file), else from its name.
bool isBuiltInControls(SDL_Gamepad* pad, const std::string& name) {
  const Uint16 vendor = SDL_GetGamepadVendor(pad), product = SDL_GetGamepadProduct(pad);
  if (vendor == 0x28de && product != 0x11ff &&
      (product == 0x1205 || product == 0x1102 || product == 0x1142 || product == 0x1106 || product == 0x1101))
    return true;
  std::string lower = name;
  for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
  return lower.find("steam deck") != std::string::npos || lower.find("steam controller") != std::string::npos;
}
}  // namespace

void InputRouter::playersMoved() {
  ++playersVersion_;
  playersChanged_ = rnl::nowSeconds();
  if (!nesLayout_) { rebuildRuntime(); refreshBindings(); }
  if (onPadsChanged) onPadsChanged();
}

void InputRouter::attach(SDL_JoystickID id) {
  if (slotOf(id) >= 0 || (standby_.pad && standby_.id == id)) return;
  PadInfo p;
  p.pad = SDL_OpenGamepad(id);
  if (!p.pad) return;
  p.id = id;
  const char* n = SDL_GetGamepadName(p.pad);
  p.name = n ? n : "";
  char guid[33]{};
  SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(p.pad)), guid, sizeof(guid));
  p.guid = guid;
  const char* serial = SDL_GetGamepadSerial(p.pad);
  p.key = serial && *serial ? std::string("serial:") + serial : p.guid + "/" + p.name;
  p.builtIn = isBuiltInControls(p.pad, p.name);
  p.detectedKind = detectDeviceKind(SDL_GetJoystickType(SDL_GetGamepadJoystick(p.pad)), p.name);
  p.family = rnf_sdl_controller_family(int(SDL_GetGamepadType(p.pad)), p.name.c_str());
  for (int b : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH}) {
    const char* l = rnf_sdl_button_label_text(int(SDL_GetGamepadButtonLabel(p.pad, SDL_GamepadButton(b))));
    if (l) p.labels[rnf_sdl_button_element(b)] = l;
  }
  // Player 1: another controller takes over from the built-in controls (they wait aside); the built-in
  // controls take it only when it is free. Players 2-4: the controller's former player when free, else the
  // first free one.
  int slot = -1;
  if (p.builtIn) {
    if (!pads_[0].pad) slot = 0;
    else if (!standby_.pad) {
      standby_ = std::move(p);
      std::fprintf(stderr, "gamepad: %s (built-in, waiting: player 1 is %s)\n", standby_.name.c_str(), pads_[0].name.c_str());
      playersMoved();
      return;
    }
  } else if (!pads_[0].pad || pads_[0].builtIn) {
    if (pads_[0].pad) {
      releaseSlot(0);
      standby_ = std::move(pads_[0]);
      pads_[0] = PadInfo();
    }
    slot = 0;
  }
  if (slot < 0) {
    auto f = formerSlot_.find(p.key);
    if (f != formerSlot_.end() && f->second > 0 && f->second < kSlots && !pads_[f->second].pad) slot = f->second;
    for (int s = 1; slot < 0 && s < kSlots; ++s)
      if (!pads_[s].pad) slot = s;
  }
  if (slot < 0) {  // four players already
    SDL_CloseGamepad(p.pad);
    return;
  }
  pads_[slot] = std::move(p);
  std::fprintf(stderr, "gamepad: %s (slot %d, type %d%s)\n", pads_[slot].name.c_str(), slot,
               int(SDL_GetGamepadType(pads_[slot].pad)), pads_[slot].builtIn ? ", built-in" : "");
  playersMoved();
}

void InputRouter::releaseSlot(int s) {
  if (lastSlot_ == s) lastSlot_ = -1;
  std::string prefix = slotPrefix(s);
  rn_input_release_prefix(in_, prefix.c_str());
  rnf_chord_reset(chord_);
  for (auto* set : {&swallowedStarts_, &practiceStops_})
    for (auto it = set->begin(); it != set->end();) it = it->compare(0, prefix.size(), prefix) == 0 ? set->erase(it) : std::next(it);
  confirmTap_.clear();
  for (const auto& [m, dir] : holdMembers_)
    if (onMarkerHold) onMarkerHold(dir, false);
  holdMembers_.clear();
  for (const std::string& m : forwardedMembers_) rn_input_set_pressed(in_, m.c_str(), 0);
  forwardedMembers_.clear();
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
}

void InputRouter::detach(SDL_JoystickID id) {
  if (standby_.pad && standby_.id == id) {
    SDL_CloseGamepad(standby_.pad);
    standby_ = PadInfo();
    playersMoved();
    return;
  }
  int s = slotOf(id);
  if (s < 0) return;
  std::string name = pads_[s].name;
  if (s > 0 && !pads_[s].key.empty()) formerSlot_[pads_[s].key] = s;
  releaseSlot(s);
  SDL_CloseGamepad(pads_[s].pad);
  pads_[s] = PadInfo();
  if (s == 0 && standby_.pad) {  // player 1 back to the built-in controls
    pads_[0] = std::move(standby_);
    standby_ = PadInfo();
    std::fprintf(stderr, "gamepad: %s (slot 0 again)\n", pads_[0].name.c_str());
  }
  playersMoved();
  if (onDisconnect) onDisconnect(name);
}

void InputRouter::padButton(int slot, int button, bool down, double t) {
  const char* el = rnf_sdl_button_element(button);
  if (!el || rnf_input_element_ignored(el)) return;  // HOME / guide: the system's (Steam, Game Bar)
  std::string id = slotPrefix(slot) + el;
  if (down) {
    noteLastSlot(slot);
    pads_[slot].lastPress = t;
  }
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
    if (down) {
      pressed_.insert(id);
      confirmTap_.cancel();  // L / R step while paused: a held confirm is no longer a tap
    } else {
      pressed_.erase(id);
    }
    pumpChord();
    return;
  }
  pumpChord();  // a member held alone may have fired just now
  // L or R held + START: pause (the seek bar). START never reaches the game then, nor its release;
  // the held member does nothing of its own when released.
  if (std::strcmp(el, "menu") == 0 && !ui_) {
    if (down && rnf_chord_claim_held(chord_, slotPrefix(slot).c_str(), t) > 0) {
      pumpChord();
      pressed_.insert(id);
      confirmTap_.cancel();
      swallowedStarts_.insert(id);
      const std::string pre = slotPrefix(slot);
      const bool leftHeld = pressed_.count(pre + "leftShoulder") && !pressed_.count(pre + "rightShoulder");
      if (practicing_ && leftHeld && onPracticeToA) onPracticeToA();
      else if (onPauseGesture) onPauseGesture(0, false);
      return;
    }
  }
  if (!down && swallowedStarts_.erase(id)) {
    pressed_.erase(id);
    return;
  }
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
    if (on) {
      pressed_.insert(id);
      confirmTap_.cancel();
    } else {
      pressed_.erase(id);
    }
    if (on && capture_) {
      captureFrom(id, false);
    } else if (routeSeek(id, on)) {
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
    if (now) {
      noteLastSlot(slot);
      pads_[slot].lastPress = t;
    }
    std::string id = g + (right ? "rightTrigger" : "leftTrigger");
    if (now && capture_) {
      pressed_.insert(id);
      captureFrom(id, false);
      return;
    }
    if (!nesLayout_ && rnf_chord_feed(chord_, id.c_str(), now ? 1 : 0, t)) {
      if (now) { pressed_.insert(id); confirmTap_.cancel(); }
      else pressed_.erase(id);
      pumpChord();
    } else {
      if (!nesLayout_) pumpChord();
      setPressed(id, now, t, !ui_);
    }
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
  if (!ui_) {
    rn_input_set_axis(in_, (g + stick).c_str(), x, y);
    if (!nesLayout_) {
      rn_input_set_axis_value(in_, (g + stick + ".x").c_str(), x);
      rn_input_set_axis_value(in_, (g + stick + ".-x").c_str(), -x);
      rn_input_set_axis_value(in_, (g + stick + ".y").c_str(), y);
      rn_input_set_axis_value(in_, (g + stick + ".-y").c_str(), -y);
    }
  }
  stickDirections(slot, stick, x, y, t);
}

void InputRouter::handleEvent(const SDL_Event& e, double evTime, bool keyboardForUI) {
  switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: attach(e.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: detach(e.gdevice.which); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      int s = slotOf(e.gbutton.which);
      if (s >= 0 && e.gbutton.down && !ui_) centerMouse();
      if (s >= 0) padButton(s, e.gbutton.button, e.gbutton.down, evTime);
      break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      int s = slotOf(e.gaxis.which);
      // A stick pushed or a trigger pulled on purpose (past half: a resting stick's drift does not
      // take the mouse away).
      if (s >= 0 && !ui_ && std::abs(int(e.gaxis.value)) > 16384) centerMouse();
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
      if (game) centerMouse();
      setPressed(id, true, evTime, game);
      break;
    }
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL: mouseEvent(e, evTime); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      rn_input_release_prefix(in_, "kb:");  // the keyboard is foreground-only
      for (auto it = pressed_.begin(); it != pressed_.end();)
        it = it->compare(0, 3, "kb:") == 0 ? pressed_.erase(it) : std::next(it);
      break;
    default: break;
  }
}

namespace {
// A full stick deflection per this much motion (pixels; relative mode reports raw counts), the
// dial's full speed at this much motion in one frame, a lever's travel per wheel notch, and how
// long a wheel notch presses its button.
constexpr float kStickPixels = 400, kDialPixels = 40, kLeverNotch = 0.1f;
constexpr int kWheelPressFrames = 8;

// v (-1..1) past the core's dead zone dz, so the smallest motion already moves the game.
float pastDeadzone(float v, float dz) {
  if (v == 0) return 0;
  const float a = std::min(1.f, std::fabs(v));
  return std::copysign(dz + (1 - dz) * a, v);
}
}  // namespace

bool InputRouter::mouseOn() const { return !settings_ || settings_->mouseControls; }
bool InputRouter::mouseGame() const { return mouseGame_ && mouseOn(); }

void InputRouter::setMouseAxes() {
  if (!mouseOn()) mouseX_ = mouseY_ = mouseLever_ = 0;
  rn_input_set_axis_value(in_, "ms:x", pastDeadzone(mouseX_, stickDeadzone_));
  rn_input_set_axis_value(in_, "ms:y", pastDeadzone(mouseY_, stickDeadzone_));
  rn_input_set_axis_value(in_, "ms:wheel", pastDeadzone(mouseLever_, stickDeadzone_));
}

void InputRouter::centerMouse() {
  if (mouseX_ == 0 && mouseY_ == 0 && mouseLever_ == 0) return;
  mouseX_ = mouseY_ = mouseLever_ = 0;
  setMouseAxes();
}

void InputRouter::setMouseCaptured(bool on) {
  if (on == mouseCaptured_) return;
  mouseCaptured_ = on;
  mouseDx_ = mouseDy_ = 0;
  wheelPress_ = 0;
  for (const char* id : {"ms:left", "ms:right", "ms:middle", "ms:wheel.up", "ms:wheel.down"}) {
    pressed_.erase(id);
    rn_input_set_pressed(in_, id, 0);
  }
  rn_input_set_axis_value(in_, "ms:dx", 0);
  rn_input_set_axis_value(in_, "ms:dy", 0);
  setMouseAxes();  // the stick and the lever stay where they were (a menu released them)
}

void InputRouter::beginMouseFrame() {
  if (!mouseGame()) return;
  rn_input_set_axis_value(in_, "ms:dx", pastDeadzone(mouseDx_ / kDialPixels, dialDeadzone_));
  rn_input_set_axis_value(in_, "ms:dy", pastDeadzone(mouseDy_ / kDialPixels, dialDeadzone_));
  mouseDx_ = mouseDy_ = 0;
  rn_input_set_pressed(in_, "ms:wheel.up", wheelPress_ > 0);
  rn_input_set_pressed(in_, "ms:wheel.down", wheelPress_ < 0);
  wheelPress_ += wheelPress_ > 0 ? -1 : wheelPress_ < 0 ? 1 : 0;
}

void InputRouter::mouseEvent(const SDL_Event& e, double t) {
  const bool game = mouseCaptured_ && !ui_;
  switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
      if (!game || e.motion.which == SDL_TOUCH_MOUSEID) break;
      mouseDx_ += e.motion.xrel;
      mouseDy_ += e.motion.yrel;
      mouseX_ = std::clamp(mouseX_ + e.motion.xrel / kStickPixels, -1.f, 1.f);
      mouseY_ = std::clamp(mouseY_ + e.motion.yrel / kStickPixels, -1.f, 1.f);
      setMouseAxes();
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      if (e.button.which == SDL_TOUCH_MOUSEID) break;
      const char* id = e.button.button == SDL_BUTTON_LEFT ? "ms:left" : e.button.button == SDL_BUTTON_RIGHT ? "ms:right"
                       : e.button.button == SDL_BUTTON_MIDDLE ? "ms:middle" : nullptr;
      if (!id) break;
      if (!e.button.down) {  // a release always reaches the engine
        pressed_.erase(id);
        rn_input_set_pressed(in_, id, 0);
      } else if (game) {
        setPressed(id, true, t, true);
      }
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      if (!game) break;
      const float notches = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
      mouseLever_ = std::clamp(mouseLever_ + notches * kLeverNotch, -1.f, 1.f);
      setMouseAxes();
      const int presses = int(std::lround(notches)) * kWheelPressFrames;
      if (presses != 0 && (wheelPress_ == 0 || (wheelPress_ > 0) == (presses > 0))) wheelPress_ += presses;
      else if (presses != 0) wheelPress_ = presses;  // the other way: that direction from now
      break;
    }
    default: break;
  }
}

void InputRouter::releaseControllers() {
  rn_input_release_prefix(in_, "gc");
  for (auto it = pressed_.begin(); it != pressed_.end();) it = it->compare(0, 2, "gc") == 0 ? pressed_.erase(it) : std::next(it);
  for (auto& p : pads_) { p.lt = p.rt = false; p.lx = p.ly = p.rx = p.ry = 0; }
  stickState_.clear();
  rnf_chord_reset(chord_);
  confirmTap_.clear();
}

}  // namespace rnl
