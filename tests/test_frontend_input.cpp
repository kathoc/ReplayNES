// Frontend core: input catalog, default bindings, layout migrations, positional face buttons
// (GameController by family + glyph, SDL3, XInput), display names and the diagram geometry /
// assignment summaries. Ported from the macOS ControllerLayoutTests / PlaybackLogicTests.
#include <algorithm>
#include <map>
#include <set>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

Plan plan(void (*fn)(const rnf_binding*, size_t, rnf_list**, rnf_list**), const Pairs& c) {
  Bindings b{c};
  auto v = b.view();
  rnf_list *u = nullptr, *n = nullptr;
  fn(v.data(), v.size(), &u, &n);
  return {listPairs(u), listPairs(n)};
}

Plan resetPlan(const Pairs& c, int slot) {
  Bindings b{c};
  auto v = b.view();
  rnf_list *u = nullptr, *n = nullptr;
  rnf_input_controller_reset_plan(v.data(), v.size(), slot, &u, &n);
  return {listPairs(u), listPairs(n)};
}

std::map<std::string, int> gcPositions(const char* category, const char* vendor = nullptr,
                                       std::vector<const char*> symbols = {nullptr, nullptr, nullptr, nullptr}) {
  int pos[4];
  rnf_gc_face_positions(category, vendor, symbols.data(), pos);
  return {{"buttonA", pos[0]}, {"buttonB", pos[1]}, {"buttonX", pos[2]}, {"buttonY", pos[3]}};
}

Plan translation(const Pairs& c, int slot, const std::map<std::string, int>& positions) {
  Bindings b{c};
  auto v = b.view();
  std::vector<const char*> names;
  std::vector<int> pos;
  for (auto& p : positions) { names.push_back(p.first.c_str()); pos.push_back(p.second); }
  rnf_list *u = nullptr, *n = nullptr;
  rnf_input_legacy_face_translation(v.data(), v.size(), slot, names.data(), pos.data(), pos.size(), &u, &n);
  return {listPairs(u), listPairs(n)};
}

// The defaults as saved by 0.2.0 (GameController face names).
Pairs layout2Defaults() {
  Pairs c = defaultConfig();
  removeIf(c, [](const std::pair<std::string, std::string>& b) { return b.first.find(":face.") != std::string::npos; });
  for (int slot : {0, 1}) {
    auto l = listPairs(rnf_input_legacy_face_defaults(slot));
    c.insert(c.end(), l.begin(), l.end());
  }
  return c;
}

bool contains(const Pairs& c, const std::string& input, const std::string& action) {
  return std::find(c.begin(), c.end(), std::make_pair(input, action)) != c.end();
}

std::string badge(const Pairs& c, const char* element, int slot) {
  Bindings b{c};
  auto v = b.view();
  char* s = rnf_input_element_badge(v.data(), v.size(), element, slot);
  return s ? take(s) : "<nil>";
}

std::string group(const Pairs& c, const char* g, int slot) {
  Bindings b{c};
  auto v = b.view();
  char* text = nullptr;
  rnf_group_summary s = rnf_input_group_summary(v.data(), v.size(), g, slot, &text);
  std::string t = take(text);
  return s == RNF_GROUP_NONE ? "none" : s == RNF_GROUP_CUSTOM ? "custom" : "movement:" + t;
}

}  // namespace

// ------------------------------------------------------------------ catalog + defaults

TEST_CASE("action catalog order and labels") {
  REQUIRE_EQ(rnf_input_action_count(), size_t(32));
  rnf_action_info a;
  REQUIRE(rnf_input_action_get(0, &a));
  CHECK_EQ(std::string(a.id), "p1.up");
  CHECK_EQ(a.group, RNF_GROUP_PLAYER1);
  REQUIRE(rnf_input_action_get(10, &a));
  CHECK_EQ(std::string(a.id), "p2.up");
  REQUIRE(rnf_input_action_get(20, &a));
  CHECK_EQ(std::string(a.id), "hk.rewind");
  CHECK_EQ(a.group, RNF_GROUP_HOTKEY);
  CHECK_EQ(take(rnf_input_action_label("p1.a")), "A");
  CHECK_EQ(take(rnf_input_action_label("p2.turbo_b")), "Turbo B");
  CHECK_EQ(take(rnf_input_action_label("hk.slow")), "Slow motion (normal ⇔ 1/2)");
  CHECK(rnf_input_action_label("nope") == nullptr);
  CHECK_EQ(take(rnf_input_group_title(RNF_GROUP_HOTKEY)), "Hotkeys");
}

TEST_CASE("default face buttons are positional") {
  auto b = asSet(defaultBindings());
  for (auto [slot, p] : {std::make_pair(0, "p1"), std::make_pair(1, "p2")}) {
    std::string g = "gc" + std::to_string(slot) + ":";
    CHECK(b.count(g + "face.east=" + p + ".a"));
    CHECK(b.count(g + "face.south=" + p + ".b"));
    CHECK(b.count(g + "face.north=" + p + ".turbo_a"));
    CHECK(b.count(g + "face.west=" + p + ".turbo_b"));
  }
  for (auto& e : defaultBindings())
    for (const char* n : {"buttonA", "buttonB", "buttonX", "buttonY"})
      CHECK(e.first.find(std::string(":") + n) == std::string::npos);
}

TEST_CASE("default controller hotkeys") {
  auto b = asSet(defaultBindings());
  CHECK(b.count("gc0:leftTrigger=hk.rewind"));
  CHECK(b.count("gc0:rightTrigger=hk.fast_forward"));
  CHECK(b.count("gc0:leftShoulder=hk.slow"));
  CHECK(b.count("gc0:rightShoulder=hk.pause"));
  for (auto k : {"kb:51=hk.rewind", "kb:48=hk.fast_forward", "kb:49=hk.pause", "kb:37=hk.slow", "kb:43=hk.step_back",
                 "kb:47=hk.frame_advance"})
    CHECK(b.count(k));
  CHECK_FALSE(b.count("gc0:leftShoulder=hk.rewind"));
  CHECK_FALSE(b.count("gc0:rightTrigger=hk.frame_advance"));
  for (auto& e : defaultBindings()) CHECK_FALSE(e.first.find("dpad") != std::string::npos && e.second.rfind("hk.", 0) == 0);
}

TEST_CASE("SDL keyboard defaults mirror the macOS keys") {
  auto mac = defaultBindings(RNF_KEYBOARD_MACOS), sdl = defaultBindings(RNF_KEYBOARD_SDL);
  REQUIRE_EQ(mac.size(), sdl.size());
  for (size_t i = 0; i < mac.size(); ++i) {
    CHECK_EQ(mac[i].second, sdl[i].second);
    if (mac[i].first.rfind("gc", 0) == 0) CHECK_EQ(mac[i].first, sdl[i].first);
  }
  auto s = asSet(sdl);
  CHECK(s.count("kb:82=p1.up"));     // SDL_SCANCODE_UP
  CHECK(s.count("kb:27=p1.a"));      // X
  CHECK(s.count("kb:44=hk.pause"));  // Space
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_SDL, "kb:82", nullptr)), "Key ↑");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_SDL, "kb:226", nullptr)), "Key Left Alt");
}

TEST_CASE("defaults load into the engine and hotkeys are not game input") {
  rn_input* h = rn_input_new();
  char* json = rnf_input_default_config_json(RNF_KEYBOARD_MACOS);
  REQUIRE_EQ(rn_input_load_json(h, json), RN_OK);
  rnf_string_free(json);
  uint32_t e = 0, held = 0;
  uint8_t p1 = 0, p2 = 0;
  rn_input_poll_hotkeys(h, &e, &held);
  rn_input_set_pressed(h, "gc0:leftTrigger", 1);
  rn_input_poll_hotkeys(h, &e, &held);
  CHECK(held & RN_HK_REWIND);
  rn_input_sample_game(h, 0, &p1, &p2);
  CHECK_EQ(p1, 0);
  rn_input_set_pressed(h, "gc0:leftTrigger", 0);
  rn_input_set_pressed(h, "gc0:rightTrigger", 1);
  rn_input_poll_hotkeys(h, &e, &held);
  CHECK(held & RN_HK_FAST_FORWARD);
  rn_input_set_pressed(h, "gc0:rightTrigger", 0);
  rn_input_set_pressed(h, "gc0:leftShoulder", 1);
  rn_input_poll_hotkeys(h, &e, &held);
  CHECK(e & RN_HK_SLOW);
  rn_input_free(h);
}

TEST_CASE("config parse") {
  rnf_input_config* c = nullptr;
  REQUIRE_EQ(rnf_input_config_parse(R"({"bindings":[{"input":"kb:1","action":"p1.a"},{"input":3}],"socd":"allow"})", &c), RN_OK);
  CHECK_EQ(rnf_input_config_binding_count(c), size_t(1));
  CHECK_EQ(rnf_input_config_turbo_period(c), 2);
  CHECK_EQ(rnf_input_config_turbo_duty(c), 1);
  CHECK_EQ(std::string(rnf_input_config_socd(c)), "allow");
  CHECK_EQ(rnf_input_config_analog_threshold(c), 0.5);
  rnf_input_config_free(c);
  CHECK_EQ(rnf_input_config_parse("[1]", &c), RN_ERR_CORRUPT);
  CHECK(c == nullptr);
  REQUIRE_EQ(rnf_input_config_parse(R"({"turbo":{"period":4,"duty":2},"analogThreshold":0.25})", &c), RN_OK);
  CHECK_EQ(rnf_input_config_turbo_period(c), 4);
  CHECK_EQ(rnf_input_config_turbo_duty(c), 2);
  CHECK_EQ(rnf_input_config_analog_threshold(c), 0.25);
  rnf_input_config_free(c);
}

TEST_CASE("paused D-pad step directions") {
  Bindings b{defaultConfig()};
  auto v = b.view();
  rnf_list* l = rnf_input_paused_step_directions(v.data(), v.size());
  std::map<std::string, int64_t> d;
  for (size_t i = 0; i < rnf_list_count(l); ++i) d[rnf_list_a(l, i)] = rnf_list_value(l, i);
  rnf_list_free(l);
  CHECK_EQ(d["gc0:dpad.left"], -1);
  CHECK_EQ(d["gc0:dpad.right"], 1);
  CHECK_EQ(d["gc1:dpad.right"], 1);
  CHECK_FALSE(d.count("gc0:lstick.left"));
  CHECK_FALSE(d.count("kb:123"));
  CHECK_FALSE(d.count("gc0:dpad.up"));
}

// ------------------------------------------------------------------ migrations

TEST_CASE("controller layout migration (layout 1 -> 2)") {
  Pairs legacy = defaultConfig();
  auto hk = listPairs(rnf_input_controller_hotkeys());
  auto old = listPairs(rnf_input_legacy_controller_hotkeys());
  for (auto& p : hk) legacy.erase(std::remove(legacy.begin(), legacy.end(), p), legacy.end());
  legacy.insert(legacy.end(), old.begin(), old.end());
  Plan m = plan(rnf_input_controller_layout_migration, legacy);
  CHECK(m.bind == hk);
  CHECK_EQ(m.unbind.size(), size_t(4));
  CHECK(plan(rnf_input_controller_layout_migration, defaultConfig()).bind.empty());
  Pairs custom = legacy;
  custom.emplace_back("gc0:buttonY", "hk.bookmark");
  CHECK(plan(rnf_input_controller_layout_migration, custom).bind.empty());
}

TEST_CASE("untouched layout-2 defaults become positional defaults") {
  Plan m = plan(rnf_input_face_layout_migration, layout2Defaults());
  CHECK_EQ(m.unbind.size(), size_t(8));
  CHECK(asSet(applying(m, layout2Defaults())) == asSet(defaultConfig()));
  CHECK(plan(rnf_input_face_layout_migration, defaultConfig()).bind.empty());
}

TEST_CASE("customised face buttons are kept per slot") {
  Pairs c = layout2Defaults();
  removeIf(c, [](const std::pair<std::string, std::string>& b) { return b.first == "gc0:buttonA" || b.first == "gc0:buttonB"; });
  c.emplace_back("gc0:buttonA", "p1.a");
  c.emplace_back("gc0:buttonB", "p1.b");
  Plan m = plan(rnf_input_face_layout_migration, c);
  for (auto& u : m.unbind) CHECK(u.first.rfind("gc1:", 0) == 0);
  CHECK_EQ(m.bind.size(), size_t(4));
  Pairs migrated = applying(m, c);
  CHECK(contains(migrated, "gc0:buttonA", "p1.a"));

  Plan pro = translation(migrated, 0, gcPositions("Switch Pro Controller"));
  auto afterPro = asSet(applying(pro, migrated));
  CHECK(afterPro.count("gc0:face.east=p1.a"));
  CHECK(afterPro.count("gc0:face.south=p1.b"));
  CHECK(afterPro.count("gc0:face.west=p1.turbo_a"));
  CHECK(afterPro.count("gc0:face.north=p1.turbo_b"));
  for (auto& s : afterPro) CHECK(s.rfind("gc0:button", 0) != 0);
  Plan xbox = translation(migrated, 0, gcPositions("Xbox One"));
  CHECK(asSet(applying(xbox, migrated)).count("gc0:face.south=p1.a"));
  CHECK((translation(applying(pro, migrated), 0, {}).bind.empty()));
}

static Pairs layout3Triggers(Pairs c) {
  // The layout-3 defaults: R2 rewind, L2 fast-forward.
  removeIf(c, [](const std::pair<std::string, std::string>& b) {
    return b.first == "gc0:leftTrigger" || b.first == "gc0:rightTrigger";
  });
  c.emplace_back("gc0:rightTrigger", "hk.rewind");
  c.emplace_back("gc0:leftTrigger", "hk.fast_forward");
  return c;
}

TEST_CASE("trigger swap migration (layout 3 -> 4)") {
  CHECK_EQ(RNF_CONTROLLER_LAYOUT_VERSION, 4);
  Pairs old = layout3Triggers(defaultConfig());
  Plan m = plan(rnf_input_trigger_swap_migration, old);
  CHECK_EQ(m.unbind.size(), size_t(2));
  CHECK_EQ(m.bind.size(), size_t(2));
  CHECK(asSet(applying(m, old)) == asSet(defaultConfig()));
  // Already the new defaults: nothing to do.
  CHECK(plan(rnf_input_trigger_swap_migration, defaultConfig()).bind.empty());
  // Customised triggers are kept: another action on a trigger, or a trigger rebound.
  Pairs extra = old;
  extra.emplace_back("gc0:rightTrigger", "hk.bookmark");
  CHECK(plan(rnf_input_trigger_swap_migration, extra).bind.empty());
  Pairs rebound = old;
  removeIf(rebound, [](const std::pair<std::string, std::string>& b) { return b.first == "gc0:leftTrigger"; });
  rebound.emplace_back("gc0:leftTrigger", "hk.step_back");
  CHECK(plan(rnf_input_trigger_swap_migration, rebound).bind.empty());
  // Pad 2 is not touched.
  for (auto& b : m.bind) CHECK(b.first.rfind("gc0:", 0) == 0);
}

TEST_CASE("full chain from 0.1.x") {
  Pairs c = layout2Defaults();
  auto hk = listPairs(rnf_input_controller_hotkeys());
  auto old = listPairs(rnf_input_legacy_controller_hotkeys());
  for (auto& p : hk) c.erase(std::remove(c.begin(), c.end(), p), c.end());
  c.insert(c.end(), old.begin(), old.end());
  c = applying(plan(rnf_input_controller_layout_migration, c), c);
  c = applying(plan(rnf_input_face_layout_migration, c), c);
  c = applying(plan(rnf_input_trigger_swap_migration, c), c);
  CHECK(asSet(c) == asSet(defaultConfig()));
  // From layout 3 (0.2.x): the triggers swap.
  Pairs l3 = layout3Triggers(defaultConfig());
  l3 = applying(plan(rnf_input_trigger_swap_migration, l3), l3);
  CHECK(asSet(l3) == asSet(defaultConfig()));
}

TEST_CASE("per-controller reset") {
  Pairs c = defaultConfig();
  removeIf(c, [](const std::pair<std::string, std::string>& b) { return b.first == "gc0:face.east"; });
  c.emplace_back("gc0:face.east", "hk.bookmark");
  c.emplace_back("gc1:face.east", "hk.save");
  c.emplace_back("kb:1", "hk.save");
  auto s = asSet(applying(resetPlan(c, 0), c));
  CHECK(s.count("gc0:face.east=p1.a"));
  CHECK_FALSE(s.count("gc0:face.east=hk.bookmark"));
  CHECK(s.count("gc1:face.east=hk.save"));
  CHECK(s.count("kb:1=hk.save"));
  std::set<std::string> gc0, want;
  for (auto& e : s) if (e.rfind("gc0:", 0) == 0) gc0.insert(e);
  for (auto& e : asSet(defaultBindings())) if (e.rfind("gc0:", 0) == 0) want.insert(e);
  CHECK(gc0 == want);
}

TEST_CASE("plans apply to an engine input table") {
  rn_input* h = rn_input_new();
  char* json = rnf_input_default_config_json(RNF_KEYBOARD_MACOS);
  rn_input_load_json(h, json);
  rnf_string_free(json);
  rnf_list* u = rnf_input_legacy_controller_hotkeys();
  rnf_list* none = nullptr;
  CHECK(rnf_input_apply_plan(h, nullptr, u));
  CHECK_FALSE(rnf_input_apply_plan(h, none, none));
  rnf_list_free(u);
  rn_input_free(h);
}

// ------------------------------------------------------------------ controllers

TEST_CASE("family from product category") {
  CHECK_EQ(rnf_controller_family_from("Switch Pro Controller", nullptr), RNF_FAMILY_NINTENDO);
  CHECK_EQ(rnf_controller_family_from("Nintendo Switch Joy-Con (L/R)", nullptr), RNF_FAMILY_NINTENDO);
  CHECK_EQ(rnf_controller_family_from("HID", "Pro Controller"), RNF_FAMILY_NINTENDO);
  CHECK_EQ(rnf_controller_family_from("Xbox One", nullptr), RNF_FAMILY_XBOX);
  CHECK_EQ(rnf_controller_family_from("DualSense", nullptr), RNF_FAMILY_PLAYSTATION);
  CHECK_EQ(rnf_controller_family_from("DualShock 4", nullptr), RNF_FAMILY_PLAYSTATION);
  CHECK_EQ(rnf_controller_family_from("MFi", nullptr), RNF_FAMILY_GENERIC);
  CHECK_EQ(rnf_controller_family_from("Steam Deck", nullptr), RNF_FAMILY_STEAM_DECK);
  CHECK_EQ(take(rnf_controller_family_title(RNF_FAMILY_NINTENDO)), "Nintendo (Pro Controller / Joy-Con)");
  CHECK_EQ(take(rnf_controller_family_title(RNF_FAMILY_GENERIC)), "Other");
}

TEST_CASE("GameController face names to positions") {
  for (const char* cat : {"Switch Pro Controller", "Nintendo Switch Joy-Con (L/R)"}) {
    auto m = gcPositions(cat);
    CHECK_EQ(m["buttonA"], RNF_FACE_EAST);
    CHECK_EQ(m["buttonB"], RNF_FACE_SOUTH);
    CHECK_EQ(m["buttonX"], RNF_FACE_NORTH);
    CHECK_EQ(m["buttonY"], RNF_FACE_WEST);
  }
  for (const char* cat : {"Xbox One", "DualSense", "DualShock 4", "MFi", "HID"}) {
    auto m = gcPositions(cat);
    CHECK_EQ(m["buttonA"], RNF_FACE_SOUTH);
    CHECK_EQ(m["buttonB"], RNF_FACE_EAST);
    CHECK_EQ(m["buttonX"], RNF_FACE_WEST);
    CHECK_EQ(m["buttonY"], RNF_FACE_NORTH);
  }
  auto single = gcPositions("Nintendo Switch Joy-Con (R)");
  std::set<int> all;
  for (auto& e : single) all.insert(e.second);
  CHECK_EQ(all.size(), size_t(4));
  CHECK_EQ(single["buttonA"], RNF_FACE_SOUTH);
  CHECK_EQ(std::string(rnf_gc_label_from_symbol("a.circle")), "A");
  CHECK_EQ(std::string(rnf_gc_label_from_symbol("xmark.circle.fill")), "✕");
  CHECK(rnf_gc_label_from_symbol(nullptr) == nullptr);
  CHECK(rnf_gc_label_from_symbol("house") == nullptr);
}

TEST_CASE("Nintendo positions follow reported glyphs") {
  auto genuine = gcPositions("Switch Pro Controller", nullptr, {"a.circle", "b.circle", "x.circle", "y.circle"});
  CHECK_EQ(genuine["buttonA"], RNF_FACE_EAST);
  CHECK_EQ(genuine["buttonB"], RNF_FACE_SOUTH);
  CHECK_EQ(genuine["buttonX"], RNF_FACE_NORTH);
  CHECK_EQ(genuine["buttonY"], RNF_FACE_WEST);
  auto eightBitDo = gcPositions("Switch Pro Controller", nullptr, {"b.circle", "a.circle", "y.circle", "x.circle"});
  CHECK_EQ(eightBitDo["buttonA"], RNF_FACE_SOUTH);
  CHECK_EQ(eightBitDo["buttonB"], RNF_FACE_EAST);
  CHECK_EQ(eightBitDo["buttonX"], RNF_FACE_WEST);
  CHECK_EQ(eightBitDo["buttonY"], RNF_FACE_NORTH);
  auto partial = gcPositions("Switch Pro Controller", nullptr, {"b.circle", nullptr, nullptr, nullptr});
  CHECK_EQ(partial["buttonA"], RNF_FACE_EAST);
}

TEST_CASE("printed buttons reach the right NES buttons") {
  auto nesBits = [](const char* category, int button) {
    rn_input* h = rn_input_new();
    char* json = rnf_input_default_config_json(RNF_KEYBOARD_MACOS);
    rn_input_load_json(h, json);
    rnf_string_free(json);
    int pos[4];
    rnf_gc_face_positions(category, nullptr, nullptr, pos);
    std::string id = std::string("gc0:") + rnf_face_position_element(rnf_face_position(pos[button]));
    rn_input_set_pressed(h, id.c_str(), 1);
    uint8_t p1 = 0, p2 = 0;
    rn_input_sample_game(h, 0, &p1, &p2);
    rn_input_free(h);
    return p1;
  };
  CHECK_EQ(nesBits("Switch Pro Controller", 0), RN_BTN_A);
  CHECK_EQ(nesBits("Switch Pro Controller", 1), RN_BTN_B);
  CHECK_EQ(nesBits("Xbox One", 1), RN_BTN_A);
  CHECK_EQ(nesBits("Xbox One", 0), RN_BTN_B);
  CHECK_EQ(nesBits("DualSense", 1), RN_BTN_A);
}

TEST_CASE("SDL3 and XInput mappings are positional") {
  CHECK_EQ(std::string(rnf_sdl_button_element(0)), "face.south");
  CHECK_EQ(std::string(rnf_sdl_button_element(1)), "face.east");
  CHECK_EQ(std::string(rnf_sdl_button_element(4)), "options");  // BACK / View / -
  CHECK_EQ(std::string(rnf_sdl_button_element(6)), "menu");     // START
  CHECK_EQ(std::string(rnf_sdl_button_element(14)), "dpad.right");
  CHECK_EQ(std::string(rnf_sdl_button_element(16)), "rightPaddle1");
  CHECK(rnf_sdl_button_element(99) == nullptr);
  CHECK_EQ(std::string(rnf_sdl_axis_element(1, 0)), "lstick.up");
  CHECK_EQ(std::string(rnf_sdl_axis_element(5, 1)), "rightTrigger");
  CHECK(rnf_sdl_axis_element(4, 0) == nullptr);
  CHECK_EQ(rnf_sdl_controller_family(1, "Steam Deck"), RNF_FAMILY_STEAM_DECK);
  CHECK_EQ(rnf_sdl_controller_family(3, "Xbox Wireless Controller"), RNF_FAMILY_XBOX);
  CHECK_EQ(rnf_sdl_controller_family(6, "DualSense Wireless Controller"), RNF_FAMILY_PLAYSTATION);
  CHECK_EQ(rnf_sdl_controller_family(7, "Pro Controller"), RNF_FAMILY_NINTENDO);
  CHECK_EQ(rnf_sdl_controller_family(1, "8BitDo Pro 3"), RNF_FAMILY_GENERIC);
  CHECK_EQ(std::string(rnf_sdl_button_label_text(5)), "✕");
  CHECK_EQ(std::string(rnf_xinput_button_element(0x1000)), "face.south");
  CHECK_EQ(std::string(rnf_xinput_button_element(0x2000)), "face.east");
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_STEAM_DECK, "home")), "STEAM");
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_STEAM_DECK, "leftPaddle1")), "L4");
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_STEAM_DECK, "face.east")), "B");
  // NES A on the Deck is its B (east) button, like on Xbox pads.
  CHECK(asSet(defaultBindings(RNF_KEYBOARD_SDL)).count(std::string("gc0:") + rnf_sdl_button_element(1) + "=p1.a"));
}

TEST_CASE("display names") {
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "kb:126", nullptr)), "Key ↑");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "kb:56", nullptr)), "Key Left Shift");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "kb:200", nullptr)), "Key #200");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "gc0:face.east", nullptr)), "Pad 1 Right Button");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "gc0:face.east", "A")), "Pad 1 Right Button(A)");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "gc1:dpad.up", "A")), "Pad 2 D-pad ↑");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "gc1:leftTrigger", nullptr)), "Pad 2 ZL(L2/LT)");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "gc0:mystery", nullptr)), "Pad 1 mystery");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "midi:7", nullptr)), "midi:7");
  int slot = -1;
  CHECK(rnf_input_controller_slot("gc3:face.east", &slot));
  CHECK_EQ(slot, 3);
  CHECK_FALSE(rnf_input_controller_slot("kb:1", &slot));
}

// ------------------------------------------------------------------ diagram

TEST_CASE("diagram elements per family") {
  std::set<std::string> expected = {"leftShoulder", "rightShoulder", "leftTrigger", "rightTrigger", "menu", "options",
                                    "home", "leftThumb", "rightThumb"};
  for (int p = 0; p < 4; ++p) expected.insert(rnf_face_position_element(rnf_face_position(p)));
  for (auto g : {"dpad", "lstick", "rstick"})
    for (auto d : {"up", "down", "left", "right"}) expected.insert(std::string(g) + "." + d);
  for (auto& b : defaultBindings())
    if (b.first.rfind("gc", 0) == 0) CHECK(expected.count(b.first.substr(b.first.find(':') + 1)));

  for (auto f : {RNF_FAMILY_NINTENDO, RNF_FAMILY_XBOX, RNF_FAMILY_PLAYSTATION, RNF_FAMILY_GENERIC, RNF_FAMILY_STEAM_DECK}) {
    std::vector<rnf_diagram_element> el(rnf_diagram_element_count(f));
    for (size_t i = 0; i < el.size(); ++i) rnf_diagram_element_get(f, i, &el[i]);
    std::set<std::string> names;
    for (auto& e : el) names.insert(e.element);
    CHECK(names == expected);
    CHECK_EQ(el.size(), expected.size());
    auto minX = [](const rnf_diagram_element& e) { return e.cx - e.width / 2; };
    auto minY = [](const rnf_diagram_element& e) { return e.cy - e.height / 2; };
    for (auto& e : el) {
      CHECK(minX(e) >= 0);
      CHECK(minY(e) >= 0);
      CHECK(minX(e) + e.width <= RNF_DIAGRAM_CANVAS_WIDTH);
      CHECK(minY(e) + e.height <= RNF_DIAGRAM_CANVAS_HEIGHT);
    }
    for (size_t i = 0; i < el.size(); ++i)
      for (size_t j = i + 1; j < el.size(); ++j) {
        const auto &a = el[i], &b = el[j];
        bool overlap = minX(a) < minX(b) + b.width && minX(b) < minX(a) + a.width && minY(a) < minY(b) + b.height &&
                       minY(b) < minY(a) + a.height;
        if (overlap) FAIL(f << ": " << a.element << " overlaps " << b.element);
      }
    auto at = [&](const char* n) {
      for (auto& e : el) if (std::string(e.element) == n) return e;
      return rnf_diagram_element{};
    };
    CHECK(at("face.east").cx > at("face.west").cx);
    CHECK(at("face.north").cy < at("face.south").cy);
  }
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_NINTENDO, "face.east")), "A");
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_NINTENDO, "face.south")), "B");
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_XBOX, "face.east")), "B");
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_PLAYSTATION, "face.east")), "○");
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_NINTENDO, "leftTrigger")), "ZL");
  CHECK_EQ(std::string(rnf_controller_family_label(RNF_FAMILY_XBOX, "rightTrigger")), "RT");
  CHECK_EQ(take(rnf_input_element_title("face.east", RNF_FAMILY_NINTENDO, nullptr)), "Right Button (A)");
  CHECK_EQ(take(rnf_input_element_title("dpad.up", RNF_FAMILY_XBOX, nullptr)), "D-pad ↑");
  CHECK_EQ(take(rnf_input_element_title("leftTrigger", RNF_FAMILY_NINTENDO, nullptr)), "ZL");
  CHECK_EQ(take(rnf_input_element_title("face.south", RNF_FAMILY_XBOX, "X")), "Bottom Button (X)");
  rnf_diagram_info info;
  rnf_diagram_info_get(RNF_FAMILY_PLAYSTATION, &info);
  CHECK(info.has_touchpad);
  rnf_diagram_info_get(RNF_FAMILY_XBOX, &info);
  CHECK_FALSE(info.has_touchpad);
  CHECK_EQ(info.dpad_anchor_y, info.dpad_y + RNF_DIAGRAM_DPAD_ARM * 1.5 + 4);
}

TEST_CASE("diagram badges show game buttons and hotkeys") {
  Pairs c = defaultConfig();
  CHECK_EQ(badge(c, "face.east", 0), "A");
  CHECK_EQ(badge(c, "face.north", 0), "Turbo A");
  CHECK_EQ(badge(c, "leftTrigger", 0), "Rewind");
  CHECK_EQ(badge(c, "rightTrigger", 0), "Fast Fwd");
  CHECK_EQ(badge(c, "leftShoulder", 0), "Slow");
  CHECK_EQ(badge(c, "rightShoulder", 0), "Pause");
  CHECK_EQ(badge(c, "menu", 0), "START");
  CHECK_EQ(badge(c, "home", 0), "<nil>");
  CHECK_EQ(group(c, "dpad", 0), "movement:Move");
  CHECK_EQ(group(c, "lstick", 0), "movement:Move");
  CHECK_EQ(group(c, "rstick", 0), "none");
  CHECK_EQ(badge(c, "face.east", 1), "A");
  CHECK_EQ(badge(c, "rightTrigger", 1), "<nil>");
  CHECK_EQ(take(rnf_input_action_short_label("p2.a", 0)), "2P A");
  CHECK_EQ(take(rnf_input_action_short_label("weird", 0)), "weird");
  Pairs custom = c;
  removeIf(custom, [](const std::pair<std::string, std::string>& b) { return b.first == "gc0:dpad.up"; });
  custom.emplace_back("gc0:dpad.up", "hk.bookmark");
  CHECK_EQ(group(custom, "dpad", 0), "custom");
  custom.emplace_back("gc0:home", "hk.save");
  custom.emplace_back("gc0:home", "p1.select");
  CHECK_EQ(badge(custom, "home", 0), "SELECT · Save");  // catalog order: game actions first
  Pairs p2 = c;
  for (auto& e : p2) if (e.first.rfind("gc0:dpad.", 0) == 0) e.second.replace(0, 2, "p2");
  CHECK_EQ(group(p2, "dpad", 0), "movement:2P Move");
}
