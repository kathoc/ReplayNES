// Frontend core: the L+R chord detector, the Quick Menu binding (hk.menu, layout 4 -> 5) and the
// quick menu model (docs/design/UI_REDESIGN.md): tree, features, navigation, no-scroll invariant.
#include <algorithm>
#include <string>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

const char* L = "gc0:leftShoulder";
const char* R = "gc0:rightShoulder";
const char* LR = "gc0:leftShoulder+gc0:rightShoulder";

struct Ev {
  rnf_chord_kind kind;
  std::string input;
  double time;
  int repeats;
};

std::vector<Ev> drain(rnf_chord* c) {
  std::vector<Ev> v;
  rnf_chord_event e;
  while (rnf_chord_poll(c, &e)) v.push_back({e.kind, e.input, e.time, e.repeats});
  return v;
}

std::string str(const std::vector<Ev>& v) {
  std::string s;
  for (auto& e : v) {
    const char* k = e.kind == RNF_CHORD_COMBO_DOWN ? "COMBO" : e.kind == RNF_CHORD_COMBO_UP ? "combo-up"
                    : e.kind == RNF_CHORD_ALONE_DOWN ? "DOWN" : e.kind == RNF_CHORD_ALONE_REPEAT ? "rep" : "up";
    s += std::string(s.empty() ? "" : " ") + k + ":" + (e.input == LR ? "LR" : e.input == L ? "L" : e.input == R ? "R" : e.input);
  }
  return s;
}

rnf_chord* menuChord() {
  rnf_chord* c = rnf_chord_new(0);
  Bindings b{defaultConfig()};
  auto v = b.view();
  REQUIRE_EQ(rnf_chord_configure(c, v.data(), v.size(), nullptr), size_t(1));
  return c;
}

}  // namespace

// ------------------------------------------------------------------ chord detector

TEST_CASE("chord: L then R within 100 ms opens the menu once; nothing fires alone") {
  rnf_chord* c = menuChord();
  CHECK(rnf_chord_is_member(c, L));
  CHECK(rnf_chord_is_member(c, R));
  CHECK_FALSE(rnf_chord_is_member(c, "gc0:face.south"));
  CHECK_EQ(rnf_chord_feed(c, L, 1, 1.000), 1);
  CHECK_EQ(rnf_chord_deadline(c), 1.100);
  rnf_chord_tick(c, 1.050);
  CHECK(drain(c).empty());
  CHECK_EQ(rnf_chord_feed(c, R, 1, 1.080), 1);
  CHECK_EQ(str(drain(c)), "COMBO:LR");
  CHECK_EQ(rnf_chord_deadline(c), 0.0);
  rnf_chord_tick(c, 2.0);  // held long: still nothing alone
  CHECK(drain(c).empty());
  rnf_chord_feed(c, L, 0, 2.1);
  CHECK(drain(c).empty());  // R still down
  rnf_chord_feed(c, R, 0, 2.2);
  CHECK_EQ(str(drain(c)), "combo-up:LR");
  rnf_chord_free(c);
}

TEST_CASE("chord: either order, and exactly at the window edge") {
  rnf_chord* c = menuChord();
  rnf_chord_feed(c, R, 1, 5.0);
  rnf_chord_feed(c, L, 1, 5.099);
  CHECK_EQ(str(drain(c)), "COMBO:LR");
  rnf_chord_feed(c, R, 0, 5.2);
  rnf_chord_feed(c, L, 0, 5.2);
  CHECK_EQ(str(drain(c)), "combo-up:LR");
  // 100 ms or more apart: R alone (at press + 100 ms), then L on its own.
  rnf_chord_feed(c, R, 1, 6.0);
  rnf_chord_feed(c, L, 1, 6.11);
  auto ev = drain(c);
  CHECK_EQ(str(ev), "DOWN:R");
  REQUIRE_EQ(ev.size(), size_t(1));
  CHECK(near(ev[0].time, 6.1, 1e-9));
  rnf_chord_feed(c, L, 0, 6.15);
  CHECK_EQ(str(drain(c)), "DOWN:L up:L");
  rnf_chord_feed(c, R, 0, 6.3);
  CHECK_EQ(str(drain(c)), "up:R");
  rnf_chord_free(c);
}

TEST_CASE("chord: a single press is triggered by its release (ALONE_UP), tap or hold") {
  rnf_chord* c = menuChord();
  CHECK_EQ(rnf_chord_repeat(c), 0);
  rnf_chord_feed(c, R, 1, 1.0);
  rnf_chord_tick(c, 1.03);
  CHECK(drain(c).empty());
  rnf_chord_feed(c, R, 0, 1.04);  // tap: DOWN + UP together at the release (UP = pause)
  auto ev = drain(c);
  CHECK_EQ(str(ev), "DOWN:R up:R");
  CHECK(near(ev[1].time, 1.04, 1e-9));
  CHECK_EQ(ev[1].repeats, 0);
  rnf_chord_feed(c, L, 1, 2.0);  // hold: "held alone" when the window ends, the trigger only at the release
  rnf_chord_tick(c, 2.099);
  CHECK(drain(c).empty());
  rnf_chord_tick(c, 2.116);
  ev = drain(c);
  CHECK_EQ(str(ev), "DOWN:L");
  CHECK(near(ev[0].time, 2.1, 1e-9));
  rnf_chord_tick(c, 3.0);
  CHECK(drain(c).empty());  // no repeats unless asked for
  CHECK_EQ(rnf_chord_deadline(c), 0.0);
  rnf_chord_feed(c, L, 0, 3.1);
  ev = drain(c);
  CHECK_EQ(str(ev), "up:L");
  CHECK(near(ev[0].time, 3.1, 1e-9));
  CHECK_EQ(ev[0].repeats, 0);
  rnf_chord_free(c);
}

TEST_CASE("chord: repeat mode (paused frame steps): held alone >= 400 ms repeats, never in a chord") {
  rnf_chord* c = menuChord();
  rnf_chord_set_repeat(c, 1);
  CHECK_EQ(rnf_chord_repeat(c), 1);
  // A short press: one trigger at the release, no repeat.
  rnf_chord_feed(c, R, 1, 1.0);
  rnf_chord_tick(c, 1.2);
  CHECK_EQ(str(drain(c)), "DOWN:R");
  CHECK(near(rnf_chord_deadline(c), 1.4, 1e-9));
  rnf_chord_feed(c, R, 0, 1.35);
  auto ev = drain(c);
  CHECK_EQ(str(ev), "up:R");
  CHECK_EQ(ev[0].repeats, 0);
  // Held: repeats from press + 400 ms every 50 ms; the release reports them (no extra step).
  rnf_chord_feed(c, L, 1, 2.0);
  rnf_chord_tick(c, 2.39);
  CHECK_EQ(str(drain(c)), "DOWN:L");
  rnf_chord_tick(c, 2.40);
  ev = drain(c);
  CHECK_EQ(str(ev), "rep:L");
  CHECK_EQ(ev[0].repeats, 1);
  CHECK(near(rnf_chord_deadline(c), 2.45, 1e-9));
  rnf_chord_tick(c, 2.45);
  rnf_chord_tick(c, 2.50);
  CHECK_EQ(str(drain(c)), "rep:L rep:L");
  rnf_chord_tick(c, 2.80);  // a stall: one repeat, then from now on
  ev = drain(c);
  CHECK_EQ(str(ev), "rep:L");
  CHECK(near(rnf_chord_deadline(c), 2.85, 1e-9));
  rnf_chord_feed(c, L, 0, 2.82);
  ev = drain(c);
  CHECK_EQ(str(ev), "up:L");
  CHECK_EQ(ev[0].repeats, 4);
  // A chord never repeats, however long it is held.
  rnf_chord_feed(c, L, 1, 3.0);
  rnf_chord_feed(c, R, 1, 3.05);
  rnf_chord_tick(c, 4.0);
  CHECK_EQ(str(drain(c)), "COMBO:LR");
  rnf_chord_feed(c, L, 0, 4.1);
  rnf_chord_feed(c, R, 0, 4.1);
  CHECK_EQ(str(drain(c)), "combo-up:LR");
  // Turned off while held: no more repeats.
  rnf_chord_feed(c, R, 1, 5.0);
  rnf_chord_tick(c, 5.45);
  CHECK_EQ(str(drain(c)), "DOWN:R rep:R");
  rnf_chord_set_repeat(c, 0);
  rnf_chord_tick(c, 6.0);
  CHECK(drain(c).empty());
  rnf_chord_feed(c, R, 0, 6.1);
  ev = drain(c);
  CHECK_EQ(str(ev), "up:R");
  CHECK_EQ(ev[0].repeats, 1);
  rnf_chord_free(c);
}

TEST_CASE("chord: repeat-safe (key repeat, re-pressing while held, one menu per chord)") {
  rnf_chord* c = menuChord();
  rnf_chord_feed(c, L, 1, 1.0);
  rnf_chord_feed(c, L, 1, 1.02);  // repeat of a held member: ignored (no new window)
  rnf_chord_feed(c, R, 1, 1.05);
  rnf_chord_feed(c, R, 1, 1.06);
  CHECK_EQ(str(drain(c)), "COMBO:LR");
  // R released and pressed again while L is held: the chord is still the same one.
  rnf_chord_feed(c, R, 0, 1.2);
  rnf_chord_feed(c, R, 1, 1.25);
  rnf_chord_tick(c, 1.5);
  CHECK(drain(c).empty());
  rnf_chord_feed(c, L, 0, 1.6);
  rnf_chord_feed(c, R, 0, 1.7);
  CHECK_EQ(str(drain(c)), "combo-up:LR");
  // A release without a press, a press after everything is up: normal again.
  rnf_chord_feed(c, L, 0, 2.0);
  CHECK(drain(c).empty());
  rnf_chord_feed(c, L, 1, 3.0);
  rnf_chord_feed(c, R, 1, 3.01);
  CHECK_EQ(str(drain(c)), "COMBO:LR");
  rnf_chord_free(c);
}

TEST_CASE("chord: held alone, then the other comes late: no menu") {
  rnf_chord* c = menuChord();
  rnf_chord_feed(c, L, 1, 1.0);
  rnf_chord_tick(c, 1.2);
  CHECK_EQ(str(drain(c)), "DOWN:L");
  rnf_chord_feed(c, R, 1, 1.3);  // L held alone already: R starts its own window
  rnf_chord_feed(c, R, 0, 1.35);
  CHECK_EQ(str(drain(c)), "DOWN:R up:R");
  // Non-members pass through and still fire due timeouts.
  rnf_chord_feed(c, R, 1, 2.0);
  CHECK_EQ(rnf_chord_feed(c, "gc0:face.south", 1, 2.5), 0);
  CHECK_EQ(str(drain(c)), "DOWN:R");  // L is still held alone
  rnf_chord_reset(c);
  CHECK(drain(c).empty());
  rnf_chord_feed(c, R, 0, 2.6);  // forgotten by the reset
  CHECK(drain(c).empty());
  rnf_chord_free(c);
}

TEST_CASE("chord: combos from bindings, manual combos") {
  rnf_chord* c = rnf_chord_new(0.05);
  Pairs p = {{"gc1:leftThumb+gc1:rightThumb", "hk.menu"}, {"gc0:leftShoulder", "hk.slow"}, {"kb:41", "hk.menu"},
             {"gc0:face.north+gc0:face.west", "hk.bookmark"}};
  Bindings b{p};
  auto v = b.view();
  CHECK_EQ(rnf_chord_configure(c, v.data(), v.size(), nullptr), size_t(1));
  CHECK(rnf_chord_is_member(c, "gc1:rightThumb"));
  CHECK_FALSE(rnf_chord_is_member(c, "kb:41"));
  CHECK_EQ(rnf_chord_configure(c, v.data(), v.size(), "hk.bookmark"), size_t(1));
  CHECK(rnf_chord_is_member(c, "gc0:face.west"));
  CHECK_EQ(rnf_chord_add(c, "gc0:face.west", "gc0:face.east"), -1);  // a member already used
  CHECK_EQ(rnf_chord_add(c, "gc2:a", "gc2:a"), -1);
  CHECK_EQ(rnf_chord_add(c, "gc2:a", "gc2:b"), 1);
  CHECK_EQ(rnf_chord_combo_count(c), size_t(2));
  rnf_chord_feed(c, "gc2:b", 1, 0.0);
  rnf_chord_feed(c, "gc2:a", 1, 0.049);
  rnf_chord_event e;
  REQUIRE(rnf_chord_poll(c, &e));
  CHECK_EQ(std::string(e.input), "gc2:a+gc2:b");
  CHECK_EQ(e.member, -1);
  rnf_chord_free(c);
}

// ------------------------------------------------------------------ hk.menu binding

TEST_CASE("Quick Menu defaults: L+R combo for pad 1, Esc on the keyboard") {
  auto mac = asSet(defaultBindings(RNF_KEYBOARD_MACOS)), sdl = asSet(defaultBindings(RNF_KEYBOARD_SDL));
  CHECK(mac.count(std::string(LR) + "=hk.menu"));
  CHECK(sdl.count(std::string(LR) + "=hk.menu"));
  CHECK(mac.count("kb:53=hk.menu"));
  CHECK(sdl.count("kb:41=hk.menu"));
  // R still pauses, L still slows down.
  CHECK(sdl.count("gc0:rightShoulder=hk.pause"));
  CHECK(sdl.count("gc0:leftShoulder=hk.slow"));
  CHECK_EQ(take(rnf_input_action_label("hk.menu")), "Quick Menu");
  // The engine accepts the action and the combo id; it is a hotkey (never game input).
  rn_input* in = rn_input_new();
  char* json = rnf_input_default_config_json(RNF_KEYBOARD_SDL);
  REQUIRE_EQ(rn_input_load_json(in, json), RN_OK);
  rnf_string_free(json);
  rn_input_set_pressed(in, LR, 1);
  uint32_t edges = 0, held = 0;
  rn_input_poll_hotkeys(in, &edges, &held);
  CHECK(edges & RN_HK_MENU);
  CHECK(held & RN_HK_MENU);
  uint8_t p1 = 0, p2 = 0;
  rn_input_sample_game(in, 0, &p1, &p2);
  CHECK_EQ(p1, uint8_t(0));
  rn_input_free(in);
}

TEST_CASE("combo ids: display names, split") {
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_SDL, LR, nullptr)), "Pad 1 L(L1/LB) + R(R1/RB)");
  CHECK_EQ(take(rnf_input_combo_id("gc0:a", "gc0:b")), "gc0:a+gc0:b");
  CHECK(rnf_input_combo_id("", "gc0:b") == nullptr);
  char *a = nullptr, *b = nullptr;
  REQUIRE(rnf_input_combo_split(LR, &a, &b));
  CHECK_EQ(take(a), L);
  CHECK_EQ(take(b), R);
  CHECK_FALSE(rnf_input_combo_split("gc0:leftShoulder", nullptr, nullptr));
  CHECK_FALSE(rnf_input_combo_split("kb:41", nullptr, nullptr));
}

TEST_CASE("menu migration (layout 4 -> 5)") {
  auto plan5 = [](const Pairs& c, rnf_keyboard_scheme s) {
    Bindings b{c};
    auto v = b.view();
    rnf_list *u = nullptr, *n = nullptr;
    rnf_input_menu_migration(v.data(), v.size(), s, &u, &n);
    return Plan{listPairs(u), listPairs(n)};
  };
  Pairs layout4 = defaultConfig();
  removeIf(layout4, [](const std::pair<std::string, std::string>& b) { return b.second == "hk.menu"; });
  Plan m = plan5(layout4, RNF_KEYBOARD_MACOS);
  CHECK(m.unbind.empty());
  CHECK(asSet(applying(m, layout4)) == asSet(defaultConfig()));
  // R keeps pausing (nothing unbound).
  CHECK(asSet(applying(m, layout4)).count("gc0:rightShoulder=hk.pause"));
  // Already migrated: nothing.
  CHECK(plan5(defaultConfig(), RNF_KEYBOARD_MACOS).bind.empty());
  // Esc in use for something else: only the combo.
  Pairs escUsed = layout4;
  escUsed.emplace_back("kb:41", "hk.bookmark");
  Plan e = plan5(escUsed, RNF_KEYBOARD_SDL);
  REQUIRE_EQ(e.bind.size(), size_t(1));
  CHECK_EQ(e.bind[0].first, LR);
  // The user bound the menu to a pad button already: no combo, no Esc.
  Pairs own = layout4;
  own.emplace_back("gc0:home", "hk.menu");
  CHECK(plan5(own, RNF_KEYBOARD_SDL).bind.empty());
}

// ------------------------------------------------------------------ menu model

namespace {

const uint32_t kAll = 0xffffffffu;
const uint32_t kDesktopLinux = RNF_MENU_FEATURE_EXPORT | RNF_MENU_FEATURE_CRT | RNF_MENU_FEATURE_STEAM | RNF_MENU_FEATURE_OSK |
                               RNF_MENU_FEATURE_UPDATES | RNF_MENU_FEATURE_QUIT | RNF_MENU_FEATURE_FULLSCREEN |
                               RNF_MENU_FEATURE_UI_SCALE;

std::vector<std::string> itemIds(rnf_menu* m, const char* page) {
  std::vector<std::string> v;
  int p = rnf_menu_page_find(m, page);
  if (p < 0) return v;
  rnf_menu_item_info it;
  for (size_t i = 0; rnf_menu_item_get(m, size_t(p), i, &it); ++i) v.push_back(it.id);
  return v;
}

std::string pageId(rnf_menu* m) {
  rnf_menu_page_info pi;
  rnf_menu_page_get(m, rnf_menu_current(m), &pi);
  return pi.id;
}

std::string crumbs(rnf_menu* m) {
  rnf_menu_crumb c[8];
  size_t n = rnf_menu_breadcrumb(m, c, 8);
  std::string s;
  for (size_t i = 0; i < n && i < 8; ++i) s += std::string(i ? " > " : "") + rnf_menu_text(c[i].label);
  return s;
}

}  // namespace

TEST_CASE("menu: no page ever needs to scroll (<= 6 items, practice 4 x 2), labels / icons set") {
  for (uint32_t f : {0u, kDesktopLinux, kAll}) {
    rnf_menu* m = rnf_menu_new(f);
    REQUIRE(m);
    for (size_t p = 0; p < rnf_menu_page_count(m); ++p) {
      rnf_menu_page_info pi;
      REQUIRE(rnf_menu_page_get(m, p, &pi));
      CHECK(std::strlen(pi.title) > 0);
      CHECK(std::strlen(pi.icon) > 0);
      if (pi.kind == RNF_MENU_PAGE_CARDS) {
        CHECK_EQ(pi.count, size_t(RNF_MENU_MAX_CARDS));
        CHECK_EQ(pi.columns, 4);
        rnf_menu_set_count(m, p, 50);
        rnf_menu_page_get(m, p, &pi);
        CHECK(pi.count <= size_t(RNF_MENU_MAX_CARDS));
        continue;
      }
      if (pi.kind == RNF_MENU_PAGE_LIST || pi.kind == RNF_MENU_PAGE_CUSTOM) continue;
      if (pi.count > size_t(RNF_MENU_MAX_ITEMS)) MESSAGE(std::string("too many items: ") + pi.id);
      CHECK(pi.count <= size_t(RNF_MENU_MAX_ITEMS));
      CHECK(pi.count >= 1);
      if (pi.kind == RNF_MENU_PAGE_TILES) CHECK_EQ(size_t(pi.columns), pi.count);
      rnf_menu_item_info it;
      for (size_t i = 0; i < pi.count; ++i) {
        REQUIRE(rnf_menu_item_get(m, p, i, &it));
        CHECK(std::strlen(it.label) > 0);
        CHECK(std::strlen(it.icon) > 0);
        if (it.kind == RNF_MENU_ITEM_PAGE) CHECK(rnf_menu_page_find(m, it.target) >= 0);
        if (it.kind == RNF_MENU_ITEM_CHOICE) {
          CHECK(it.choice_count >= 2);
          for (size_t c = 0; c < it.choice_count; ++c) CHECK(std::strlen(rnf_menu_text(rnf_menu_item_choice(m, p, i, c))) > 0);
          CHECK(rnf_menu_item_choice(m, p, i, it.choice_count) == nullptr);
        }
      }
    }
    // Depth: every chain of pages fits in RNF_MENU_MAX_DEPTH levels (pushing deeper is refused).
    REQUIRE(rnf_menu_open(m, "quick"));
    rnf_menu_free(m);
  }
}

TEST_CASE("menu: the Quick Menu is six tiles in one row, Resume first") {
  rnf_menu* m = rnf_menu_new(kDesktopLinux);
  CHECK((itemIds(m, "quick") == std::vector<std::string>{"resume", "retry", "practice", "share", "settings", "game"}));
  CHECK((itemIds(m, "retry") ==
        std::vector<std::string>{"retry.rerecord", "retry.undo", "retry.takes", "retry.bookmarks", "retry.playback",
                                 "retry.restart"}));
  CHECK((itemIds(m, "share") == std::vector<std::string>{"share.export"}));
  CHECK((itemIds(m, "game") == std::vector<std::string>{"game.library", "game.save", "game.save_as", "game.open", "game.reset"}));
  CHECK((itemIds(m, "settings.display") == std::vector<std::string>{"display.size", "display.crt", "display.flash", "display.vtr",
                                                                   "display.shape", "display.crt_detail"}));
  CHECK((itemIds(m, "display.shape") == std::vector<std::string>{"display.par87", "display.overscan"}));
  CHECK((itemIds(m, "settings.sound") == std::vector<std::string>{"sound.volume"}));
  rnf_menu_free(m);
  // Features: no CRT -> no CRT row / details page; no export and no stream -> no Share tile.
  m = rnf_menu_new(0);
  CHECK((itemIds(m, "quick") == std::vector<std::string>{"resume", "retry", "practice", "settings", "game"}));
  CHECK(rnf_menu_page_find(m, "display.crt") < 0);
  CHECK((itemIds(m, "settings.display") ==
        std::vector<std::string>{"display.size", "display.flash", "display.vtr", "display.shape"}));
  CHECK((itemIds(m, "settings.system") == std::vector<std::string>{"system.language", "system.about", "system.detail"}));
  rnf_menu_free(m);
  // Resume Countdown: System > More (Controls and Controls > More are full on Linux).
  for (uint32_t f : {0u, kDesktopLinux, kAll}) {
    rnf_menu* d = rnf_menu_new(f);
    std::vector<std::string> ids = itemIds(d, "system.detail");
    CHECK(std::count(ids.begin(), ids.end(), std::string("system.resume_countdown")) == 1);
    CHECK(ids.size() <= size_t(RNF_MENU_MAX_ITEMS));
    std::vector<std::string> controls = itemIds(d, "settings.controls");
    CHECK(std::count(controls.begin(), controls.end(), std::string("system.resume_countdown")) == 0);
    rnf_menu_free(d);
  }
  m = rnf_menu_new(RNF_MENU_FEATURE_STREAM | RNF_MENU_FEATURE_SLOW_AUDIO | RNF_MENU_FEATURE_LOW_LATENCY);
  CHECK((itemIds(m, "share") == std::vector<std::string>{"share.stream"}));
  CHECK((itemIds(m, "settings.sound") == std::vector<std::string>{"sound.volume", "sound.slow", "sound.low_latency"}));
  rnf_menu_free(m);
}

TEST_CASE("menu: breadcrumb segments navigate back (pointer)") {
  rnf_menu* m = rnf_menu_new(kDesktopLinux);
  REQUIRE(rnf_menu_open(m, nullptr));
  CHECK_EQ(rnf_menu_crumb_select(m, 0), RNF_MENU_EVENT_NONE);  // no crumbs on the Quick Menu
  REQUIRE_EQ(rnf_menu_push(m, "settings.controls"), RNF_MENU_EVENT_PUSHED);
  rnf_menu_set_focus(m, 2);
  REQUIRE_EQ(rnf_menu_push(m, "controls.detail"), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(crumbs(m), "Settings > Controls > More Controls");
  CHECK_EQ(rnf_menu_crumb_select(m, 2), RNF_MENU_EVENT_NONE);  // the current page
  CHECK_EQ(rnf_menu_crumb_select(m, 9), RNF_MENU_EVENT_NONE);
  CHECK_EQ(rnf_menu_depth(m), size_t(3));
  // "Controls": back one level, its focus restored.
  CHECK_EQ(rnf_menu_crumb_select(m, 1), RNF_MENU_EVENT_POPPED);
  CHECK_EQ(pageId(m), "settings.controls");
  CHECK_EQ(rnf_menu_focus(m), size_t(2));
  // "Settings" (the group's title): the Settings top page (Display, as its tile opens it).
  CHECK_EQ(rnf_menu_crumb_select(m, 0), RNF_MENU_EVENT_SWITCHED);
  CHECK_EQ(pageId(m), "settings.display");
  CHECK_EQ(rnf_menu_focus(m), size_t(0));
  CHECK_EQ(crumbs(m), "Settings > Display");
  CHECK_EQ(rnf_menu_crumb_select(m, 0), RNF_MENU_EVENT_NONE);  // already there
  // From two levels down straight to the group's top page.
  REQUIRE_EQ(rnf_menu_switch(m, 1), RNF_MENU_EVENT_SWITCHED);
  REQUIRE_EQ(rnf_menu_push(m, "controls.detail"), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(rnf_menu_crumb_select(m, 0), RNF_MENU_EVENT_POPPED);
  CHECK_EQ(pageId(m), "settings.display");
  CHECK_EQ(rnf_menu_depth(m), size_t(2));
  CHECK_EQ(rnf_menu_back(m), RNF_MENU_EVENT_POPPED);
  CHECK_EQ(pageId(m), "quick");
  CHECK_EQ(rnf_menu_crumb_select(nullptr, 0), RNF_MENU_EVENT_NONE);
  rnf_menu_free(m);
}

TEST_CASE("menu: navigation, back stack with focus, L / R, breadcrumb") {
  rnf_menu* m = rnf_menu_new(kDesktopLinux);
  CHECK_EQ(rnf_menu_depth(m), size_t(0));
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_NONE);
  REQUIRE(rnf_menu_open(m, nullptr));
  CHECK_EQ(pageId(m), "quick");
  CHECK_EQ(rnf_menu_focus(m), size_t(0));
  CHECK_EQ(crumbs(m), "");
  CHECK_EQ(rnf_menu_move(m, -1, 0, nullptr), RNF_MENU_EVENT_NONE);  // left edge
  CHECK_EQ(rnf_menu_move(m, 0, 1, nullptr), RNF_MENU_EVENT_NONE);   // one row
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_CLOSE);              // Resume
  for (int i = 0; i < 4; ++i) CHECK_EQ(rnf_menu_move(m, 1, 0, nullptr), RNF_MENU_EVENT_MOVED);
  CHECK_EQ(rnf_menu_focus(m), size_t(4));  // Settings
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(pageId(m), "settings.display");
  CHECK_EQ(crumbs(m), "Settings > Display");
  int dir = 0;
  CHECK_EQ(rnf_menu_move(m, 1, 0, &dir), RNF_MENU_EVENT_ADJUST);  // Size: Integer -> FILL
  CHECK_EQ(dir, 1);
  CHECK_EQ(rnf_menu_switch(m, 1), RNF_MENU_EVENT_SWITCHED);
  CHECK_EQ(pageId(m), "settings.controls");
  CHECK_EQ(crumbs(m), "Settings > Controls");
  CHECK_EQ(rnf_menu_switch(m, -1), RNF_MENU_EVENT_SWITCHED);
  CHECK_EQ(rnf_menu_switch(m, -1), RNF_MENU_EVENT_SWITCHED);  // wraps: System
  CHECK_EQ(pageId(m), "settings.system");
  CHECK_EQ(rnf_menu_switch(m, 1), RNF_MENU_EVENT_SWITCHED);
  // Display > CRT details: depth 3 below the Quick Menu.
  for (int i = 0; i < 5; ++i) rnf_menu_move(m, 0, 1, nullptr);
  CHECK_EQ(rnf_menu_focus(m), size_t(5));
  CHECK_EQ(rnf_menu_move(m, 0, 1, nullptr), RNF_MENU_EVENT_NONE);
  CHECK_EQ(rnf_menu_move(m, 1, 0, nullptr), RNF_MENU_EVENT_NONE);  // a page row: nothing to adjust
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(rnf_menu_depth(m), size_t(3));
  CHECK_EQ(crumbs(m), "Settings > Display > CRT Details");
  CHECK(rnf_menu_depth(m) <= size_t(RNF_MENU_MAX_DEPTH));
  CHECK_EQ(rnf_menu_switch(m, 1), RNF_MENU_EVENT_NONE);  // not a group page
  CHECK_EQ(rnf_menu_back(m), RNF_MENU_EVENT_POPPED);
  CHECK_EQ(rnf_menu_focus(m), size_t(5));  // focus restored
  CHECK_EQ(rnf_menu_back(m), RNF_MENU_EVENT_POPPED);
  CHECK_EQ(pageId(m), "quick");
  CHECK_EQ(rnf_menu_focus(m), size_t(4));  // the Settings tile
  CHECK_EQ(rnf_menu_back(m), RNF_MENU_EVENT_CLOSE);
  rnf_menu_free(m);
}

TEST_CASE("menu: practice cards 4 x 2, lists in sheets of six, custom pages") {
  rnf_menu* m = rnf_menu_new(kDesktopLinux);
  REQUIRE(rnf_menu_open(m, nullptr));
  rnf_menu_move(m, 1, 0, nullptr);
  rnf_menu_move(m, 1, 0, nullptr);
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(pageId(m), "practice");
  CHECK_EQ(rnf_menu_move(m, 0, 1, nullptr), RNF_MENU_EVENT_MOVED);
  CHECK_EQ(rnf_menu_focus(m), size_t(4));
  CHECK_EQ(rnf_menu_move(m, 1, 0, nullptr), RNF_MENU_EVENT_MOVED);
  CHECK_EQ(rnf_menu_focus(m), size_t(5));
  CHECK_EQ(rnf_menu_move(m, 0, 1, nullptr), RNF_MENU_EVENT_NONE);
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_ACTIVATE);
  CHECK_EQ(crumbs(m), "Practice");
  rnf_menu_back(m);
  // Retry > Takes: 14 takes = 3 sheets.
  rnf_menu_set_focus(m, 1);
  REQUIRE_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_PUSHED);
  rnf_menu_set_focus(m, 2);
  REQUIRE_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(pageId(m), "takes");
  CHECK_EQ(crumbs(m), "Retry > Takes");
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_NONE);  // empty list
  rnf_menu_set_count(m, rnf_menu_current(m), 14);
  CHECK_EQ(rnf_menu_sheet_count(m), size_t(3));
  for (int i = 0; i < 6; ++i) rnf_menu_move(m, 0, 1, nullptr);
  CHECK_EQ(rnf_menu_focus(m), size_t(6));
  CHECK_EQ(rnf_menu_sheet(m), size_t(1));
  CHECK_EQ(rnf_menu_switch(m, 1), RNF_MENU_EVENT_SWITCHED);
  CHECK_EQ(rnf_menu_focus(m), size_t(12));
  CHECK_EQ(rnf_menu_switch(m, 1), RNF_MENU_EVENT_SWITCHED);  // wraps
  CHECK_EQ(rnf_menu_focus(m), size_t(0));
  int dir = 0;
  CHECK_EQ(rnf_menu_move(m, -1, 0, &dir), RNF_MENU_EVENT_ADJUST);
  CHECK_EQ(dir, -1);
  rnf_menu_set_focus(m, 13);
  rnf_menu_set_count(m, rnf_menu_current(m), 3);  // takes went away: focus kept in range
  CHECK_EQ(rnf_menu_focus(m), size_t(2));
  CHECK_EQ(rnf_menu_sheet_count(m), size_t(1));
  CHECK_EQ(rnf_menu_switch(m, 1), RNF_MENU_EVENT_NONE);
  // Library: its own roots (Settings, Projects).
  REQUIRE(rnf_menu_open(m, "settings.display"));
  CHECK_EQ(crumbs(m), "Settings > Display");
  CHECK_EQ(rnf_menu_back(m), RNF_MENU_EVENT_CLOSE);
  REQUIRE(rnf_menu_open(m, "library.projects"));
  CHECK_EQ(crumbs(m), "Projects");
  CHECK_EQ(rnf_menu_push(m, "controls.controller"), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(rnf_menu_move(m, 1, 0, nullptr), RNF_MENU_EVENT_NONE);  // custom: the frontend navigates
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_ACTIVATE);
  CHECK_FALSE(rnf_menu_open(m, "nope"));
  rnf_menu_close(m);
  CHECK_EQ(rnf_menu_depth(m), size_t(0));
  rnf_menu_free(m);
}

TEST_CASE("menu: toggles flip with A, info rows do nothing, literal choices") {
  rnf_menu* m = rnf_menu_new(kDesktopLinux);
  REQUIRE(rnf_menu_open(m, "settings.display"));
  rnf_menu_set_focus(m, 1);  // CRT
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_ACTIVATE);
  REQUIRE(rnf_menu_open(m, "system.about"));
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_NONE);  // Version
  int sys = rnf_menu_page_find(m, "settings.system");
  int lang = rnf_menu_item_find(m, size_t(sys), "system.language");
  REQUIRE(lang >= 0);
  CHECK_EQ(std::string(rnf_menu_text(rnf_menu_item_choice(m, size_t(sys), size_t(lang), 2))), "English");
  CHECK_EQ(std::string(rnf_menu_text(rnf_menu_item_choice(m, size_t(sys), size_t(lang), 0))), "Automatic");
  rnf_menu_free(m);
}
