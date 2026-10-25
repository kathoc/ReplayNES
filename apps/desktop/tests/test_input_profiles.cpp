// SPDX-License-Identifier: GPL-2.0-or-later
#include "input_router.h"
#include "settings.h"
#include "support/rn_test.h"
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
  SDL_JoystickID add(const char* name, unsigned product) {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x1234;
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
