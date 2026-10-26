// SPDX-License-Identifier: GPL-2.0-or-later
#include "input_router.h"
#include "settings.h"
#include "support/rn_test.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace {
const char* layout = R"({"name":"fbneo:profiles","recordBytes":3,"buttons":[{"id":"p1.b1","byte":0,"bit":0},{"id":"p1.b2","byte":0,"bit":1},{"id":"p1.b3","byte":0,"bit":2},{"id":"p2.b1","byte":1,"bit":0},{"id":"p2.b2","byte":1,"bit":1},{"id":"p2.b3","byte":1,"bit":2}]})";
std::string take(char* text) {
  std::string result = text ? text : "";
  rn_string_free(text);
  return result;
}
std::string read(const std::filesystem::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
struct Devices {
  std::vector<SDL_JoystickID> ids;
  Devices() { REQUIRE(SDL_Init(SDL_INIT_GAMEPAD)); }
  ~Devices() {
    for (auto id : ids) SDL_DetachVirtualJoystick(id);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    rnf_input_set_layout_json(nullptr);
  }
  SDL_JoystickID add(const char* name, unsigned product, unsigned vendor = 0x1234) {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = Uint16(vendor);
    desc.product_id = product;
    desc.name = name;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    const auto id = SDL_AttachVirtualJoystick(&desc);
    REQUIRE(id != 0);
    REQUIRE(SDL_IsGamepad(id));
    ids.push_back(id);
    return id;
  }
  void remove(SDL_JoystickID id) {
    SDL_DetachVirtualJoystick(id);
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
  }
};
void connect(rnl::InputRouter& router, SDL_JoystickID id, bool added = true) {
  SDL_Event e{};
  e.type = added ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
  e.gdevice.which = id;
  router.handleEvent(e, 1.0, false);
}
std::set<std::string> pairs(rn_input* input) {
  const auto json = take(rn_input_save_json(input));
  rnf_input_config* config = nullptr;
  REQUIRE_EQ(rnf_input_config_parse(json.c_str(), &config), RN_OK);
  std::set<std::string> result;
  for (size_t i = 0; i < rnf_input_config_binding_count(config); ++i) {
    rnf_binding b{};
    REQUIRE(rnf_input_config_binding_get(config, i, &b));
    result.insert(std::string(b.input) + "=" + b.action);
  }
  rnf_input_config_free(config);
  return result;
}
struct Files {
  std::filesystem::path root = std::filesystem::path(RNL_TEST_TMP) / "device-profiles";
  Files() { std::filesystem::remove_all(root); std::filesystem::create_directories(root); }
  ~Files() { std::filesystem::remove_all(root); }
};
}

TEST_CASE("P device kind detection covers every name SDL type and Steam Deck exception") {
  using rnl::InputRouter;
  using rnl::DeviceKind;
  CHECK(InputRouter::detectDeviceKind(SDL_JOYSTICK_TYPE_ARCADE_STICK, "Generic USB") == DeviceKind::stick);
  const char* names[] = {"ARCADE", "Fight Stick", "FightStick", "Fighting Stick", "Fighting Commander", "HORI RAP",
    "Real Arcade", "QANBA", "Victrix", "Panthera", "Mayflash F", "8BitDo Arcade", "Astro City", "Egret", "Hitbox",
    "Hit Box", "Snack Box", "Brook", "NeoGeo Arcade"};
  for (const auto* name : names) CHECK(InputRouter::detectDeviceKind(SDL_JOYSTICK_TYPE_GAMEPAD, std::string("USB ") + name + " Controller") == DeviceKind::stick);
  CHECK(InputRouter::detectDeviceKind(SDL_JOYSTICK_TYPE_UNKNOWN, "Wireless Controller") == DeviceKind::pad);
  CHECK(InputRouter::detectDeviceKind(SDL_JOYSTICK_TYPE_GAMEPAD, "Steam Deck") == DeviceKind::pad);
  CHECK(InputRouter::detectDeviceKind(SDL_JOYSTICK_TYPE_ARCADE_STICK, "Steam Deck") == DeviceKind::pad);
}

TEST_CASE("P mixed slots edit only their kind and use P1 keyboard and turbo settings") {
  Files files;
  Devices devices;
  const auto stick = devices.add("Qanba Arcade", 1);
  const auto pad = devices.add("Generic Pad", 2);
  const auto base = files.root / "bindings.json";
  const auto padPath = files.root / "bindings-fbneo%3Aprofiles.json";
  const auto stickPath = files.root / "bindings-fbneo%3Aprofiles@stick.json";
  rnl::Settings settings;
  rnl::InputRouter router(base.string(), &settings);
  router.setLayout(layout);
  connect(router, stick);
  connect(router, pad);
  REQUIRE(router.deviceKind(0) == rnl::DeviceKind::stick);
  REQUIRE(router.deviceKind(1) == rnl::DeviceKind::pad);
  auto active = pairs(router.input());
  CHECK(active.count("gc0:face.west=p1.b1"));
  CHECK(active.count("gc1:face.south=p2.b1"));
  CHECK_FALSE(active.count("gc0:face.south=p1.b1"));
  CHECK_FALSE(active.count("gc1:face.west=p2.b1"));
  router.setEditingSlot(1);
  router.setAssignment("gc1:face.south", "p2.b2");
  router.setAssignment("kb:4", "p1.b2");
  router.setTurbo(11, 3);
  router.setButtonTurbo("p1.b1", 13, 4);
  const auto savedPad = read(padPath);
  REQUIRE(!savedPad.empty());
  router.setEditingSlot(0);
  router.setAssignment("gc0:face.west", "p1.b3");
  router.setAssignment("kb:4", "p1.b3");
  router.setTurbo(7, 2);
  router.setButtonTurbo("p1.b1", 9, 2);
  const auto savedStick = read(stickPath);
  REQUIRE(!savedStick.empty());
  CHECK_EQ(read(padPath), savedPad);
  active = pairs(router.input());
  CHECK(active.count("gc0:face.west=p1.b3"));
  CHECK(active.count("gc1:face.south=p2.b2"));
  CHECK(active.count("kb:4=p1.b3"));
  rnf_input_config* runtime = nullptr;
  REQUIRE_EQ(rnf_input_config_parse(take(rn_input_save_json(router.input())).c_str(), &runtime), RN_OK);
  CHECK_EQ(rnf_input_config_turbo_period(runtime), 7);
  int period = 0, duty = 0;
  REQUIRE(rnf_input_config_button_turbo(runtime, "p1.b1", &period, &duty));
  CHECK_EQ(period, 9);
  rnf_input_config_free(runtime);
  router.setEditingSlot(1);
  CHECK_EQ(router.turboPeriod(), 11);
  router.resetToDefaults();
  CHECK_EQ(read(stickPath), savedStick);
  rn_input_set_pressed(router.input(), "gc0:face.west", 1);
  rn_input_record record{};
  rn_input_sample_record(router.input(), 0, &record);
  REQUIRE(record.bytes[0] != 0);
  router.setKindOverride(0, rnl::KindOverride::pad);
  rn_input_sample_record(router.input(), 1, &record);
  CHECK_EQ(int(record.bytes[0]), 0);
  CHECK(router.deviceKind(0) == rnl::DeviceKind::pad);
  CHECK(pairs(router.input()).count("gc0:face.south=p1.b1"));
  router.setKindOverride(0, rnl::KindOverride::automatic);
  CHECK(router.deviceKind(0) == rnl::DeviceKind::stick);
  connect(router, stick, false);
  CHECK(router.deviceKind(0) == rnl::DeviceKind::pad);
}

TEST_CASE("P GUID overrides survive router recreation and layout changes and can return to auto") {
  Files files;
  Devices devices;
  const auto id = devices.add("Generic Pad", 3);
  rnl::Settings settings;
  const auto base = (files.root / "bindings.json").string();
  std::string guid;
  {
    rnl::InputRouter router(base, &settings);
    router.setLayout(layout);
    connect(router, id);
    guid = router.pad(0).guid;
    REQUIRE(!guid.empty());
    router.setKindOverride(0, rnl::KindOverride::stick);
    CHECK(router.deviceKind(0) == rnl::DeviceKind::stick);
  }
  CHECK(read(files.root / "controller-kinds.json").find(guid) != std::string::npos);
  {
    rnl::InputRouter router(base, &settings);
    router.setLayout(layout);
    connect(router, id);
    CHECK(router.kindOverride(0) == rnl::KindOverride::stick);
    CHECK(router.deviceKind(0) == rnl::DeviceKind::stick);
    router.setLayout(nullptr);
    CHECK_FALSE(router.usesDeviceKinds());
    router.resetToDefaults();
    std::unique_ptr<rn_input, decltype(&rn_input_free)> expected(rn_input_new(), rn_input_free);
    REQUIRE_EQ(rn_input_load_json(expected.get(), take(rnf_input_default_config_json(RNF_KEYBOARD_SDL)).c_str()), RN_OK);
    CHECK_EQ(read(files.root / "bindings.json"), take(rn_input_save_json(expected.get())));
    CHECK_FALSE(std::filesystem::exists(files.root / "bindings@stick.json"));
    router.setLayout(layout);
    CHECK(router.deviceKind(0) == rnl::DeviceKind::stick);
    router.setKindOverride(0, rnl::KindOverride::automatic);
  }
  rnl::InputRouter router(base, &settings);
  router.setLayout(layout);
  connect(router, id);
  CHECK(router.kindOverride(0) == rnl::KindOverride::automatic);
  CHECK(router.deviceKind(0) == rnl::DeviceKind::pad);
}

TEST_CASE("P existing layout bindings load unchanged as the pad profile without controllers") {
  Files files;
  Devices devices;
  rnl::Settings settings;
  const auto padPath = files.root / "bindings-fbneo%3Aprofiles.json";
  const std::string old = R"({"version":1,"bindings":[{"input":"kb:4","action":"p1.b2"},{"input":"gc0:face.west","action":"p1.b3"}],"turbo":{"period":15,"duty":4}})";
  { std::ofstream out(padPath); out << old; }
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(layout);
  CHECK(router.editingKind() == rnl::DeviceKind::pad);
  CHECK_EQ(router.turboPeriod(), 15);
  CHECK(pairs(router.input()).count("kb:4=p1.b2"));
  CHECK(pairs(router.input()).count("gc0:face.west=p1.b3"));
  CHECK_EQ(read(padPath), old);
}


TEST_CASE("P SDL stick triggers hold transport and LB LT chord leaves RB as a game button") {
  Files files;
  Devices devices;
  const auto id = devices.add("Qanba Arcade", 4);
  rnl::Settings settings;
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(layout);
  connect(router, id);
  int menus = 0;
  router.onMenuButton = [&] { ++menus; };
  auto button = [&](int b, bool down, double time) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = id;
    e.gbutton.button = b;
    e.gbutton.down = down;
    router.handleEvent(e, time, false);
  };
  auto trigger = [&](bool down, double time) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = id;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    e.gaxis.value = down ? 32767 : 0;
    router.handleEvent(e, time, false);
  };
  uint32_t edges = 0, held = 0;
  trigger(true, 2.0);
  router.tick(2.2);
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  CHECK((held & RN_HK_REWIND) != 0);
  trigger(false, 2.3);
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  CHECK_EQ(held, uint32_t(0));
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true, 3.0);
  button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true, 3.02);
  router.tick(3.2);
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  CHECK((held & RN_HK_FAST_FORWARD) != 0);
  CHECK_EQ(menus, 0);
  rn_input_record record{};
  rn_input_sample_record(router.input(), 0, &record);
  CHECK((record.bytes[0] & 4) != 0);
  button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false, 3.3);
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false, 3.31);
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  CHECK_EQ(held, uint32_t(0));
  for (bool triggerFirst : {false, true}) {
    const double time = triggerFirst ? 5.0 : 4.0;
    if (triggerFirst) {
      trigger(true, time);
      button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true, time + 0.02);
    } else {
      button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true, time);
      trigger(true, time + 0.02);
    }
    router.tick(time + 0.2);
    rn_input_poll_hotkeys(router.input(), &edges, &held);
    CHECK_EQ(held & (RN_HK_REWIND | RN_HK_FAST_FORWARD), uint32_t(0));
    trigger(false, time + 0.3);
    button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false, time + 0.31);
  }
  CHECK_EQ(menus, 2);
  router.setPausedStepMode(true);
  CHECK_FALSE(router.stepRepeatHeld());
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true, 6.0);
  CHECK(router.stepRepeatHeld()); // includes the initial chord/repeat delay
  router.tick(6.2);
  CHECK(router.stepRepeatHeld());
  router.setUIMode(true);
  CHECK_FALSE(router.stepRepeatHeld());
  router.setUIMode(false);
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false, 6.3);
  CHECK_FALSE(router.stepRepeatHeld());
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true, 7.0);
  router.tick(7.2);
  trigger(true, 7.3); // sequential follower: repeat between ticks remains busy
  router.tick(7.35);
  CHECK(router.stepRepeatHeld());
  trigger(false, 7.5);
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false, 7.6);
  CHECK_FALSE(router.stepRepeatHeld());
  button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true, 8.0);
  CHECK(router.stepRepeatHeld());
  connect(router, id, false);
  CHECK_FALSE(router.stepRepeatHeld());
}

namespace {
// After Burner II: a stick and a throttle lever (positions, with FBNeo's dead zone).
const char* kFlightLayout = R"({"name":"fbneo:flight","recordBytes":11,"buttons":[{"id":"p1.b1","label":"Vulcan","byte":3,"bit":0},{"id":"p1.b2","label":"Missile","byte":3,"bit":1},{"id":"p1.coin","byte":3,"bit":2},{"id":"p1.start","byte":3,"bit":3},{"id":"p1.up","byte":4,"bit":0},{"id":"p1.down","byte":4,"bit":1},{"id":"p1.left","byte":4,"bit":2},{"id":"p1.right","byte":4,"bit":3},{"id":"p2.coin","byte":4,"bit":4},{"id":"p2.start","byte":4,"bit":5}],"axes":[{"id":"p1.axis.x","byte":5,"deadzone":0.2},{"id":"p1.axis.y","byte":7,"deadzone":0.2},{"id":"p1.axis.ry","label":"Throttle","byte":9,"deadzone":0.2}]})";
// Arkanoid: a dial (a speed).
const char* kDialLayout = R"({"name":"fbneo:dial","recordBytes":7,"buttons":[{"id":"p1.b1","byte":3,"bit":0},{"id":"p1.coin","byte":3,"bit":1},{"id":"p1.start","byte":3,"bit":2}],"axes":[{"id":"p1.axis.x","byte":5,"relative":true,"deadzone":0.2}]})";
// Galaxy Force II: a stick, the throttle as accelerator / brake buttons.
const char* kPedalLayout = R"({"name":"fbneo:pedal","recordBytes":9,"buttons":[{"id":"p1.b1","byte":3,"bit":0},{"id":"p1.b7","label":"Brake","byte":3,"bit":1},{"id":"p1.b8","label":"Accelerate","byte":3,"bit":2}],"axes":[{"id":"p1.axis.x","byte":5},{"id":"p1.axis.y","byte":7}]})";

int16_t axisAt(const rn_input_record& r, int byte) { return int16_t(r.bytes[byte] | r.bytes[byte + 1] << 8); }
rn_input_record sample(rnl::InputRouter& router, uint64_t frame) {
  router.beginMouseFrame();
  rn_input_record r{};
  rn_input_sample_record(router.input(), frame, &r);
  return r;
}
void mouseMotion(rnl::InputRouter& router, float dx, float dy) {
  SDL_Event e{};
  e.type = SDL_EVENT_MOUSE_MOTION;
  e.motion.xrel = dx;
  e.motion.yrel = dy;
  router.handleEvent(e, 1.0, false);
}
void mouseButton(rnl::InputRouter& router, uint8_t button, bool down) {
  SDL_Event e{};
  e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  e.button.button = button;
  e.button.down = down;
  router.handleEvent(e, 1.0, false);
}
void mouseWheel(rnl::InputRouter& router, float notches) {
  SDL_Event e{};
  e.type = SDL_EVENT_MOUSE_WHEEL;
  e.wheel.y = notches;
  e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  router.handleEvent(e, 1.0, false);
}
}  // namespace

TEST_CASE("P arcade defaults: MAME's keys play every game, the mouse a game with analog controls") {
  Files files;
  Devices devices;
  rnl::Settings settings;
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(kFlightLayout);
  auto p = pairs(router.input());
  for (const char* want : {"kb:34=p1.coin", "kb:30=p1.start", "kb:40=p1.start", "kb:35=p2.coin", "kb:31=p2.start",
                           "kb:29=p1.b1", "kb:27=p1.b2", "kb:82=p1.up", "kb:81=p1.down", "kb:80=p1.left", "kb:79=p1.right",
                           "ms:left=p1.b1", "ms:right=p1.b2", "ms:x=p1.axis.x", "ms:y=p1.axis.y", "ms:wheel=p1.axis.ry"}) {
    if (!p.count(want)) std::printf("    missing: %s\n", want);
    CHECK(p.count(want));
  }
  CHECK_FALSE(router.mouseGame());  // Mouse is off by default (Settings > Controls > More)
  settings.mouseControls = true;
  CHECK(router.mouseGame());
  // Hotkeys keep their keys: Space pauses, no game button on it.
  CHECK(p.count("kb:44=hk.pause"));
  for (auto& b : p) CHECK(b.rfind("kb:44=p", 0) != 0);

  router.setLayout(kDialLayout);
  p = pairs(router.input());
  CHECK(p.count("ms:dx=p1.axis.x"));
  CHECK_FALSE(p.count("ms:x=p1.axis.x"));
  CHECK(p.count("ms:left=p1.b1"));

  router.setLayout(kPedalLayout);
  p = pairs(router.input());
  CHECK(p.count("ms:wheel.up=p1.b8"));
  CHECK(p.count("ms:wheel.down=p1.b7"));

  // No analog controls: no mouse.
  router.setLayout(layout);
  p = pairs(router.input());
  CHECK_FALSE(router.mouseGame());
  for (auto& b : p) CHECK(b.rfind("ms:", 0) != 0);
  CHECK(p.count("kb:29=p1.b1"));
}

TEST_CASE("P a profile saved before the keyboard and mouse defaults gets them, unless it binds that device") {
  Files files;
  Devices devices;
  rnl::Settings settings;
  const auto padPath = files.root / "bindings-fbneo%3Aflight.json";
  const std::string old = R"({"version":1,"bindings":[{"input":"gc0:face.south","action":"p1.b2"},{"input":"kb:44","action":"hk.pause"}],"turbo":{"period":4,"duty":2}})";
  { std::ofstream out(padPath); out << old; }
  {
    rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
    router.setLayout(kFlightLayout);
    auto p = pairs(router.input());
    CHECK(p.count("gc0:face.south=p1.b2"));  // the user's own binding stays
    CHECK(p.count("kb:34=p1.coin"));
    CHECK(p.count("ms:x=p1.axis.x"));
    CHECK_EQ(read(padPath), old);  // nothing written until the user changes something
  }
  // A keyboard binding of the user's: the keyboard is left as it is (the mouse still added).
  const std::string own = R"({"version":1,"bindings":[{"input":"kb:4","action":"p1.b1"}],"turbo":{"period":4,"duty":2}})";
  { std::ofstream out(padPath); out << own; }
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(kFlightLayout);
  auto p = pairs(router.input());
  CHECK(p.count("kb:4=p1.b1"));
  CHECK_FALSE(p.count("kb:34=p1.coin"));
  CHECK(p.count("ms:left=p1.b1"));
}

TEST_CASE("P the captured mouse: stick position, dial motion per frame, wheel lever and presses") {
  Files files;
  Devices devices;
  rnl::Settings settings;
  settings.mouseControls = true;
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(kFlightLayout);
  // Not captured (the UI has the pointer): nothing reaches the game.
  mouseMotion(router, 200, 0);
  mouseButton(router, SDL_BUTTON_LEFT, true);
  rn_input_record r = sample(router, 0);
  CHECK_EQ(axisAt(r, 5), 0);
  CHECK_EQ(r.bytes[3] & 1, 0);
  mouseButton(router, SDL_BUTTON_LEFT, false);
  router.setMouseCaptured(true);
  // 200 of the 400 pixels of a full deflection: half way, past the core's dead zone (0.2 + 0.8 * 0.5).
  mouseMotion(router, 200, -100);
  r = sample(router, 1);
  CHECK_EQ(axisAt(r, 5), int16_t(std::lround(0.6f * 32767)));
  CHECK_EQ(axisAt(r, 7), int16_t(std::lround(-(0.2f + 0.8f * 0.25f) * 32767)));
  r = sample(router, 2);  // a position: it stays
  CHECK_EQ(axisAt(r, 5), int16_t(std::lround(0.6f * 32767)));
  mouseMotion(router, 10000, 0);  // clamped at full deflection
  CHECK_EQ(axisAt(sample(router, 3), 5), 32767);
  // Buttons.
  mouseButton(router, SDL_BUTTON_LEFT, true);
  mouseButton(router, SDL_BUTTON_RIGHT, true);
  r = sample(router, 4);
  CHECK_EQ(r.bytes[3] & 3, 3);
  mouseButton(router, SDL_BUTTON_LEFT, false);
  CHECK_EQ(sample(router, 5).bytes[3] & 3, 2);
  // The wheel moves the throttle lever 0.1 a notch.
  mouseWheel(router, 2);
  CHECK_EQ(axisAt(sample(router, 6), 9), int16_t(std::lround((0.2f + 0.8f * 0.2f) * 32767)));
  // Released (the menu opened): buttons let go, the stick stays where it was once captured again.
  router.setMouseCaptured(false);
  CHECK_EQ(sample(router, 7).bytes[3] & 3, 0);
  router.setMouseCaptured(true);
  CHECK_EQ(axisAt(sample(router, 8), 5), 32767);

  // A dial: the motion of each frame (40 pixels = full speed), nothing once the mouse stops.
  router.setLayout(kDialLayout);
  router.setMouseCaptured(false);
  router.setMouseCaptured(true);
  mouseMotion(router, 4, 0);
  CHECK_EQ(axisAt(sample(router, 9), 5), int16_t(std::lround((0.2f + 0.8f * 0.1f) * 32767)));
  CHECK_EQ(axisAt(sample(router, 10), 5), 0);
  mouseMotion(router, -20, 0);
  mouseMotion(router, -20, 0);  // two events in one frame add up
  CHECK_EQ(axisAt(sample(router, 11), 5), -32767);

  // Wheel presses: a notch holds the accelerator for 8 frames, a notch the other way the brake.
  router.setLayout(kPedalLayout);
  router.setMouseCaptured(false);
  router.setMouseCaptured(true);
  mouseWheel(router, 1);
  int held = 0;
  for (int f = 0; f < 20; ++f) held += (sample(router, 20 + f).bytes[3] >> 2) & 1;
  CHECK_EQ(held, 8);
  mouseWheel(router, -1);
  CHECK_EQ((sample(router, 40).bytes[3] >> 1) & 1, 1);
}

TEST_CASE("P the mouse is off by default; a pad or a game key puts its stick back in the middle") {
  Files files;
  Devices devices;
  const auto id = devices.add("Generic Pad", 5);
  rnl::Settings settings;
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(kFlightLayout);
  connect(router, id);
  // Off: no capture wanted, and a captured mouse (the setting turned off while playing) moves nothing.
  CHECK_FALSE(router.mouseGame());
  router.setMouseCaptured(true);
  mouseMotion(router, 200, 0);
  mouseWheel(router, 2);
  rn_input_record r = sample(router, 0);
  CHECK_EQ(axisAt(r, 5), 0);
  CHECK_EQ(axisAt(r, 9), 0);
  router.setMouseCaptured(false);

  settings.mouseControls = true;
  CHECK(router.mouseGame());
  router.setMouseCaptured(true);
  const int16_t half = int16_t(std::lround(0.6f * 32767));
  auto push = [&] { mouseMotion(router, 200, 0); mouseWheel(router, 2); };
  push();
  CHECK_EQ(axisAt(sample(router, 1), 5), half);
  // A pad button: the mouse's stick and lever back in the middle (they would add to the pad's).
  SDL_Event e{};
  e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  e.gbutton.which = id;
  e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
  e.gbutton.down = true;
  router.handleEvent(e, 1, false);
  r = sample(router, 2);
  CHECK_EQ(axisAt(r, 5), 0);
  CHECK_EQ(axisAt(r, 9), 0);
  // The mouse moved again takes the stick from the middle.
  mouseMotion(router, 200, 0);
  CHECK_EQ(axisAt(sample(router, 3), 5), half);
  // A stick resting off centre (drift) keeps the mouse; pushed past half, the pad has it.
  SDL_Event a{};
  a.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
  a.gaxis.which = id;
  a.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTY;
  a.gaxis.value = 3000;
  router.handleEvent(a, 2, false);
  CHECK_EQ(axisAt(sample(router, 4), 5), half);
  a.gaxis.value = 30000;
  router.handleEvent(a, 3, false);
  CHECK_EQ(axisAt(sample(router, 5), 5), 0);
  // A key of the game.
  push();
  CHECK_EQ(axisAt(sample(router, 6), 5), half);
  SDL_Event k{};
  k.type = SDL_EVENT_KEY_DOWN;
  k.key.scancode = SDL_SCANCODE_Z;
  k.key.down = true;
  router.handleEvent(k, 4, false);
  r = sample(router, 7);
  CHECK_EQ(axisAt(r, 5), 0);
  CHECK_EQ(axisAt(r, 9), 0);
}

TEST_CASE("P players: a controller connected takes player 1 from the Steam Deck's own controls, which get it back") {
  Files files;
  Devices devices;
  rnl::Settings settings;
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(kFlightLayout);
  auto name = [&](int s) { return router.pad(s).pad ? router.pad(s).name : std::string("-"); };
  auto players = [&] { return name(0) + "|" + name(1) + "|" + name(2) + "|" + name(3) + (router.standbyConnected() ? " (deck waiting)" : ""); };
  auto plug = [&](const char* n, unsigned product, unsigned vendor) {
    auto id = devices.add(n, product, vendor);
    connect(router, id);
    return id;
  };
  auto unplug = [&](SDL_JoystickID id) {
    connect(router, id, false);
    devices.remove(id);
  };
  const auto deck = plug("Steam Deck", 0x1205, 0x28de);
  CHECK_EQ(players(), std::string("Steam Deck|-|-|-"));
  const int v0 = router.playersVersion();
  const auto a = plug("8BitDo Pro 3", 0x6009, 0x2dc8);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|-|-|- (deck waiting)"));
  CHECK(router.playersVersion() > v0);
  const auto b = plug("Pad B", 0x0b00, 0x045e);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|Pad B|-|- (deck waiting)"));
  // Player 1 disconnected: back to the deck; player 2 stays player 2.
  unplug(a);
  CHECK_EQ(players(), std::string("Steam Deck|Pad B|-|-"));
  const auto a2 = plug("8BitDo Pro 3", 0x6009, 0x2dc8);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|Pad B|-|- (deck waiting)"));
  // Steam re-creating its virtual pads: the deck leaves and comes back while another is player 1.
  unplug(deck);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|Pad B|-|-"));
  const auto deck2 = plug("Steam Deck", 0x1205, 0x28de);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|Pad B|-|- (deck waiting)"));
  // The waiting deck plays nobody: its buttons reach no player.
  SDL_Event e{};
  e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  e.gbutton.which = deck2;
  e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
  e.gbutton.down = true;
  router.handleEvent(e, 2, false);
  for (int s = 0; s < 4; ++s) CHECK(router.pad(s).lastPress < 0);
  // Player 2 leaves, another takes the free player 2; the first one back gets player 3.
  unplug(b);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|-|-|- (deck waiting)"));
  const auto c = plug("Pad C", 0x0c00, 0x046d);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|Pad C|-|- (deck waiting)"));
  const auto b2 = plug("Pad B", 0x0b00, 0x045e);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|Pad C|Pad B|- (deck waiting)"));
  // Player 2 leaves and comes back: player 2 again (it was free), not the first free one after a reshuffle.
  unplug(c);
  const auto c2 = plug("Pad C", 0x0c00, 0x046d);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|Pad C|Pad B|- (deck waiting)"));
  // A button of player 3 is seen as player 3's (the indicator).
  e.gbutton.which = b2;
  router.handleEvent(e, 3, false);
  CHECK_EQ(router.pad(2).lastPress, 3.0);
  // Players 2 and 3 both leave; player 3's controller comes back first: player 3 again, not the first free.
  unplug(c2);
  unplug(b2);
  const auto b3 = plug("Pad B", 0x0b00, 0x045e);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|-|Pad B|- (deck waiting)"));
  const auto c3 = plug("Pad C", 0x0c00, 0x046d);
  CHECK_EQ(players(), std::string("8BitDo Pro 3|Pad C|Pad B|- (deck waiting)"));
  (void)a2;
  (void)b3;
  (void)c3;
}

TEST_CASE("P the paused seek bar: L / R switch the practice section on a pad and on a stick, no repeats") {
  for (const bool stick : {false, true}) {
    Files files;
    Devices devices;
    const auto id = devices.add(stick ? "Qanba Arcade" : "Generic Pad", stick ? 4 : 5);
    rnl::Settings settings;
    rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
    router.setLayout(layout);
    connect(router, id);
    CHECK(router.deviceKind(0) == (stick ? rnl::DeviceKind::stick : rnl::DeviceKind::pad));
    std::vector<int> dirs;
    router.onPausedShoulder = [&](int dir) { dirs.push_back(dir); };
    auto button = [&](int b, bool down, double time) {
      SDL_Event e{};
      e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
      e.gbutton.which = id;
      e.gbutton.button = b;
      e.gbutton.down = down;
      router.handleEvent(e, time, false);
    };
    router.setPausedStepMode(true);
    double t = 10;
    for (int b : {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}) {
      button(b, true, t);
      router.tick(t + 0.03);
      button(b, false, t + 0.05);
      router.tick(t + 0.3);
      t += 1;
    }
    CHECK((dirs == std::vector<int>{1, 1, -1}));
    // Held: one switch, not a repeat every interval.
    dirs.clear();
    button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true, t);
    for (double h = 0.05; h < 2.0; h += 0.05) router.tick(t + h);
    button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false, t + 2.0);
    router.tick(t + 2.3);
    CHECK(dirs.size() <= 1);
    CHECK(std::count(dirs.begin(), dirs.end(), -1) == 0);
  }
}

TEST_CASE("P L3+R3 is the Undo of a retry; R3 alone still opens the menu, L3 alone does nothing") {
  Files files;
  Devices devices;
  const auto id = devices.add("Xbox Wireless Controller", 4);
  rnl::Settings settings;
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(layout);
  connect(router, id);
  int menus = 0;
  router.onMenuButton = [&] { ++menus; };
  auto button = [&](int b, bool down, double time) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = id;
    e.gbutton.button = b;
    e.gbutton.down = down;
    router.handleEvent(e, time, false);
  };
  CHECK(pairs(router.input()).count("gc0:leftThumb+gc0:rightThumb=hk.undo_take"));
  CHECK_FALSE(pairs(router.input()).count("gc0:leftThumb=hk.undo_take"));
  uint32_t edges = 0, held = 0;
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  // Both together (either order, within the chord window): the Undo, no menu.
  button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true, 1.0);
  button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true, 1.04);
  router.tick(1.2);
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  CHECK((edges & RN_HK_UNDO_TAKE) != 0);
  button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false, 1.3);
  button(SDL_GAMEPAD_BUTTON_LEFT_STICK, false, 1.31);
  router.tick(1.4);
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  CHECK_EQ(held & RN_HK_UNDO_TAKE, uint32_t(0));
  CHECK_EQ(menus, 0);
  // L3 alone (a soft stick clicked in play): nothing.
  button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true, 2.0);
  router.tick(2.2);
  button(SDL_GAMEPAD_BUTTON_LEFT_STICK, false, 2.3);
  router.tick(2.4);
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  CHECK_EQ(edges & RN_HK_UNDO_TAKE, uint32_t(0));
  CHECK_EQ(menus, 0);
  // R3 alone: the menu, held past the window or tapped.
  button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true, 3.0);
  router.tick(3.05);
  CHECK_EQ(menus, 0);  // still inside the window: it may become the chord
  router.tick(3.2);
  CHECK_EQ(menus, 1);
  button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false, 3.3);
  button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true, 4.0);
  button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false, 4.03);
  router.tick(4.2);
  CHECK_EQ(menus, 2);
  rn_input_poll_hotkeys(router.input(), &edges, &held);
  CHECK_EQ(edges & RN_HK_UNDO_TAKE, uint32_t(0));
}

TEST_CASE("P a saved arcade profile with Undo on L3 moves it to L3+R3 once (layout 9)") {
  Files files;
  rnl::Settings settings;
  settings.controllerLayoutVersion = 8;
  const auto padPath = files.root / "bindings-fbneo%3Aprofiles.json";
  { std::ofstream out(padPath); out << R"({"version":1,"bindings":[{"input":"gc0:leftThumb","action":"hk.undo_take"},{"input":"gc0:face.west","action":"p1.b3"}]})"; }
  rnl::InputRouter router((files.root / "bindings.json").string(), &settings);
  router.setLayout(layout);
  CHECK(pairs(router.input()).count("gc0:leftThumb+gc0:rightThumb=hk.undo_take"));
  CHECK_FALSE(pairs(router.input()).count("gc0:leftThumb=hk.undo_take"));
  CHECK(pairs(router.input()).count("gc0:face.west=p1.b3"));
  CHECK_EQ(settings.controllerLayoutVersion, RNF_CONTROLLER_LAYOUT_VERSION);
  CHECK(read(padPath).find("gc0:leftThumb+gc0:rightThumb") != std::string::npos);
  // A user's own L3 binding made after the upgrade stays.
  { std::ofstream out(padPath); out << R"({"version":1,"bindings":[{"input":"gc0:leftThumb","action":"hk.undo_take"}]})"; }
  rnl::InputRouter again((files.root / "bindings.json").string(), &settings);
  again.setLayout(layout);
  CHECK(pairs(again.input()).count("gc0:leftThumb=hk.undo_take"));
}
