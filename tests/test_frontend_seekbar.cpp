// Frontend core: the paused seek bar's controller model (docs/design/UI_REDESIGN.md): UI confirm /
// cancel buttons, the one hold speed curve (rewind, fast-forward, a held marker), the A/B markers,
// the controller diagram's outline and the button action picker.
#include <string>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

std::vector<uint64_t> frames(const rnf_markers* m) {
  std::vector<uint64_t> v;
  for (size_t i = 0; i < rnf_markers_count(m); ++i) v.push_back(rnf_markers_frame(m, i));
  return v;
}

rnf_timeline_range range(uint64_t a, int hasB, uint64_t b) { return rnf_timeline_range{0, a, hasB, b}; }

}  // namespace

TEST_CASE("UI confirm / cancel: east confirms, south cancels by default; the setting swaps them") {
  CHECK_EQ(std::string(rnf_ui_confirm_element(0)), "face.east");
  CHECK_EQ(std::string(rnf_ui_cancel_element(0)), "face.south");
  CHECK_EQ(std::string(rnf_ui_confirm_element(1)), "face.south");
  CHECK_EQ(std::string(rnf_ui_cancel_element(1)), "face.east");
}

TEST_CASE("hold speed: one curve, accelerating, for rewind / fast-forward / held markers") {
  CHECK_EQ(rnf_hold_speed(1), 2);
  CHECK_EQ(rnf_hold_speed(RNF_HOLD_SPEED_FAST_AFTER), 2);
  CHECK_EQ(rnf_hold_speed(RNF_HOLD_SPEED_FAST_AFTER + 1), 3);
  CHECK_EQ(rnf_hold_speed(RNF_HOLD_SPEED_FASTEST_AFTER), 3);
  CHECK_EQ(rnf_hold_speed(RNF_HOLD_SPEED_FASTEST_AFTER + 1), 4);
  CHECK_EQ(rnf_hold_speed(100000), 4);
  int prev = 0, total = 0;
  for (int t = 1; t <= 600; ++t) {
    CHECK(rnf_hold_speed(t) >= prev);  // never slows down while held
    prev = rnf_hold_speed(t);
    total += prev;
  }
  CHECK_EQ(total, 30 * 2 + 60 * 3 + 510 * 4);
}

TEST_CASE("markers: drop two at the playhead; the left is A, the right B; limits") {
  rnf_markers* m = rnf_markers_new();
  rnf_markers_sync(m, nullptr, 1000);
  CHECK_EQ(rnf_markers_count(m), size_t(0));
  CHECK_EQ(rnf_markers_focus(m), -1);
  // Nothing to focus yet: up / down / cancel are not the markers'.
  CHECK_EQ(rnf_markers_move(m, 0, -1, 10).outcome, RNF_MARKERS_IGNORED);
  CHECK_EQ(rnf_markers_move(m, 1, 0, 10).outcome, RNF_MARKERS_IGNORED);
  CHECK_EQ(rnf_markers_cancel(m).outcome, RNF_MARKERS_IGNORED);
  rnf_markers_result r = rnf_markers_confirm(m, 500);
  CHECK_EQ(r.outcome, RNF_MARKERS_CHANGED);
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_A_ONLY);
  CHECK_EQ(r.a, uint64_t(500));
  CHECK_EQ(r.seek, 0);
  CHECK_EQ(rnf_markers_confirm(m, 500).outcome, RNF_MARKERS_OCCUPIED);
  r = rnf_markers_confirm(m, 200);  // left of the first: it becomes A
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_RANGE);
  CHECK_EQ(r.a, uint64_t(200));
  CHECK_EQ(r.b, uint64_t(500));
  CHECK(frames(m) == (std::vector<uint64_t>{200, 500}));
  r = rnf_markers_confirm(m, 800);
  CHECK_EQ(r.outcome, RNF_MARKERS_FULL);
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_NONE);
  CHECK_EQ(rnf_markers_focus(m), -1);
  rnf_markers_free(m);
}

TEST_CASE("markers: a view of the selected slot (sync), kept while editing") {
  rnf_markers* m = rnf_markers_new();
  rnf_timeline_range s = range(300, 1, 120);  // unordered input is sorted
  rnf_markers_sync(m, &s, 1000);
  CHECK(frames(m) == (std::vector<uint64_t>{120, 300}));
  s = range(40, 0, 0);
  rnf_markers_sync(m, &s, 1000);
  CHECK(frames(m) == (std::vector<uint64_t>{40}));
  // Focus a marker, then the slot goes away (cleared elsewhere): focus back to the bar.
  CHECK_EQ(rnf_markers_move(m, 0, -1, 0).outcome, RNF_MARKERS_CHANGED);
  CHECK_EQ(rnf_markers_focus(m), 0);
  rnf_markers_sync(m, nullptr, 1000);
  CHECK_EQ(rnf_markers_focus(m), -1);
  // While editing, the slot's state does not overwrite the edit.
  s = range(100, 1, 200);
  rnf_markers_sync(m, &s, 1000);
  rnf_markers_move(m, 0, -1, 190);  // nearest to the playhead: B
  CHECK_EQ(rnf_markers_focus(m), 1);
  rnf_markers_confirm(m, 190);
  REQUIRE(rnf_markers_editing(m));
  rnf_markers_nudge(m, 5);
  s = range(100, 1, 200);
  rnf_markers_sync(m, &s, 1000);
  CHECK(frames(m) == (std::vector<uint64_t>{100, 205}));
  rnf_markers_free(m);
}

TEST_CASE("markers: focus moves (up from the bar, left / right, down / cancel back)") {
  rnf_markers* m = rnf_markers_new();
  rnf_timeline_range s = range(100, 1, 400);
  rnf_markers_sync(m, &s, 1000);
  CHECK_EQ(rnf_markers_move(m, 0, 1, 0).outcome, RNF_MARKERS_IGNORED);  // down on the bar
  CHECK_EQ(rnf_markers_move(m, 0, -1, 260).outcome, RNF_MARKERS_CHANGED);
  CHECK_EQ(rnf_markers_focus(m), 1);  // 400 is nearer to 260 than 100
  CHECK_EQ(rnf_markers_move(m, -1, 0, 260).outcome, RNF_MARKERS_CHANGED);
  CHECK_EQ(rnf_markers_focus(m), 0);
  CHECK_EQ(rnf_markers_move(m, -1, 0, 260).outcome, RNF_MARKERS_CHANGED);  // consumed at the edge
  CHECK_EQ(rnf_markers_focus(m), 0);
  CHECK_EQ(rnf_markers_move(m, 0, -1, 260).outcome, RNF_MARKERS_CHANGED);  // up: nothing above
  CHECK_EQ(rnf_markers_move(m, 1, 0, 260).outcome, RNF_MARKERS_CHANGED);
  CHECK_EQ(rnf_markers_focus(m), 1);
  CHECK_EQ(rnf_markers_move(m, 0, 1, 260).outcome, RNF_MARKERS_CHANGED);
  CHECK_EQ(rnf_markers_focus(m), -1);
  rnf_markers_move(m, 0, -1, 0);
  CHECK_EQ(rnf_markers_focus(m), 0);
  CHECK_EQ(rnf_markers_cancel(m).outcome, RNF_MARKERS_CHANGED);
  CHECK_EQ(rnf_markers_focus(m), -1);
  rnf_markers_free(m);
}

TEST_CASE("markers: edit = seek preview, D-pad +-1, nudge, skip over the other, swap roles, commit") {
  rnf_markers* m = rnf_markers_new();
  rnf_timeline_range s = range(100, 1, 110);
  rnf_markers_sync(m, &s, 1000);
  rnf_markers_move(m, 0, -1, 0);  // A
  rnf_markers_result r = rnf_markers_confirm(m, 50);
  CHECK(rnf_markers_editing(m));
  CHECK_EQ(r.seek, 1);
  CHECK_EQ(r.seek_frame, uint64_t(100));
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_NONE);
  r = rnf_markers_move(m, 1, 0, 0);
  CHECK_EQ(r.seek_frame, uint64_t(101));
  CHECK_EQ(rnf_markers_move(m, 0, -1, 0).outcome, RNF_MARKERS_CHANGED);  // up / down: nothing
  CHECK(rnf_markers_editing(m));
  r = rnf_markers_nudge(m, 9);  // 110 is B: lands past it, the two swap roles
  CHECK_EQ(r.seek_frame, uint64_t(111));
  CHECK(frames(m) == (std::vector<uint64_t>{110, 111}));
  CHECK_EQ(rnf_markers_focus(m), 1);  // the edited marker is now the right one (B)
  r = rnf_markers_nudge(m, -1);  // onto 110 again: jumps over it back to the left
  CHECK_EQ(r.seek_frame, uint64_t(109));
  CHECK_EQ(rnf_markers_focus(m), 0);
  r = rnf_markers_nudge(m, -10000);  // clamped at 0
  CHECK_EQ(r.seek_frame, uint64_t(0));
  r = rnf_markers_nudge(m, -4);
  CHECK_EQ(r.seek, 0);  // did not move
  r = rnf_markers_confirm(m, 0);  // commit: the range, focus back to the bar, no seek back
  CHECK_FALSE(rnf_markers_editing(m));
  CHECK_EQ(rnf_markers_focus(m), -1);
  CHECK_EQ(r.seek, 0);
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_RANGE);
  CHECK_EQ(r.a, uint64_t(0));
  CHECK_EQ(r.b, uint64_t(110));
  // The end of the take bounds it; a marker at the end cannot be passed.
  s = range(990, 1, 1000);
  rnf_markers_sync(m, &s, 1000);
  rnf_markers_move(m, 0, -1, 0);
  rnf_markers_confirm(m, 0);
  r = rnf_markers_nudge(m, 10);  // onto 1000 (B, the end): stays below it
  CHECK_EQ(r.seek_frame, uint64_t(999));
  r = rnf_markers_nudge(m, 1);
  CHECK_EQ(r.seek, 0);
  CHECK(frames(m) == (std::vector<uint64_t>{999, 1000}));
  // Committing without a change writes nothing.
  rnf_markers_cancel(m);
  rnf_markers_confirm(m, 0);
  r = rnf_markers_confirm(m, 0);
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_NONE);
  rnf_markers_free(m);
}

TEST_CASE("markers: cancel while editing reverts the marker and the playhead; leave does too") {
  rnf_markers* m = rnf_markers_new();
  rnf_timeline_range s = range(100, 0, 0);
  rnf_markers_sync(m, &s, 1000);
  rnf_markers_move(m, 0, -1, 30);
  rnf_markers_confirm(m, 30);
  rnf_markers_nudge(m, 50);
  CHECK(frames(m) == (std::vector<uint64_t>{150}));
  rnf_markers_result r = rnf_markers_cancel(m);
  CHECK_EQ(r.seek, 1);
  CHECK_EQ(r.seek_frame, uint64_t(30));  // where the playhead was
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_NONE);
  CHECK(frames(m) == (std::vector<uint64_t>{100}));
  CHECK_EQ(rnf_markers_focus(m), 0);  // still on the marker
  // One marker: commit writes "A only" at its new frame.
  rnf_markers_confirm(m, 30);
  rnf_markers_nudge(m, -7);
  r = rnf_markers_confirm(m, 30);
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_A_ONLY);
  CHECK_EQ(r.a, uint64_t(93));
  // Leaving the seek bar (resume / menu) while editing: reverted, focus on the bar.
  rnf_markers_move(m, 0, -1, 0);
  rnf_markers_confirm(m, 5);
  rnf_markers_nudge(m, 3);
  r = rnf_markers_leave(m);
  CHECK_EQ(r.seek, 1);
  CHECK_EQ(r.seek_frame, uint64_t(5));
  CHECK_FALSE(rnf_markers_editing(m));
  CHECK_EQ(rnf_markers_focus(m), -1);
  CHECK(frames(m) == (std::vector<uint64_t>{93}));
  CHECK_EQ(rnf_markers_leave(m).outcome, RNF_MARKERS_IGNORED);
  rnf_markers_free(m);
}

TEST_CASE("markers: X deletes: one of two leaves A only at the other, the last clears the slot") {
  rnf_markers* m = rnf_markers_new();
  rnf_timeline_range s = range(100, 1, 400);
  rnf_markers_sync(m, &s, 1000);
  CHECK_EQ(rnf_markers_delete(m).outcome, RNF_MARKERS_IGNORED);  // focus on the bar
  rnf_markers_move(m, 0, -1, 0);  // A (100)
  rnf_markers_result r = rnf_markers_delete(m);
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_A_ONLY);
  CHECK_EQ(r.a, uint64_t(400));  // B's frame is now the slot's A
  CHECK(frames(m) == (std::vector<uint64_t>{400}));
  CHECK_EQ(rnf_markers_focus(m), 0);
  rnf_markers_confirm(m, 0);  // editing: X does nothing
  CHECK_EQ(rnf_markers_delete(m).outcome, RNF_MARKERS_IGNORED);
  rnf_markers_cancel(m);
  r = rnf_markers_delete(m);
  CHECK_EQ(r.write, RNF_MARKERS_WRITE_CLEAR);
  CHECK_EQ(rnf_markers_count(m), size_t(0));
  CHECK_EQ(rnf_markers_focus(m), -1);
  rnf_markers_free(m);
}

TEST_CASE("diagram outline: a body without grips; the Steam Deck is wide with a screen") {
  for (rnf_controller_family f : {RNF_FAMILY_NINTENDO, RNF_FAMILY_XBOX, RNF_FAMILY_PLAYSTATION, RNF_FAMILY_GENERIC,
                                  RNF_FAMILY_STEAM_DECK}) {
    size_t n = rnf_diagram_decor_count(f);
    REQUIRE(n >= 1);
    rnf_diagram_decor body{};
    REQUIRE(rnf_diagram_decor_get(f, 0, &body));
    CHECK_EQ(body.kind, RNF_DECOR_BODY);
    CHECK(body.x >= 0);
    CHECK(body.x + body.width <= RNF_DIAGRAM_CANVAS_WIDTH);
    CHECK(body.y + body.height <= RNF_DIAGRAM_CANVAS_HEIGHT);
    CHECK(body.radius * 2 <= body.height);
    int screens = 0;
    for (size_t i = 1; i < n; ++i) {
      rnf_diagram_decor d{};
      REQUIRE(rnf_diagram_decor_get(f, i, &d));
      CHECK(d.kind != RNF_DECOR_BODY);  // one body: no grips
      CHECK(d.x >= body.x);
      CHECK(d.x + d.width <= body.x + body.width);
      screens += d.kind == RNF_DECOR_SCREEN;
    }
    CHECK_EQ(screens, f == RNF_FAMILY_STEAM_DECK ? 1 : 0);
    CHECK_FALSE(rnf_diagram_decor_get(f, n, &body));
    // Every element is on the canvas; face buttons, D-pad and sticks are on the body and off the screen.
    rnf_diagram_decor screen{};
    if (f == RNF_FAMILY_STEAM_DECK) rnf_diagram_decor_get(f, 1, &screen);
    for (size_t i = 0; i < rnf_diagram_element_count(f); ++i) {
      rnf_diagram_element e{};
      REQUIRE(rnf_diagram_element_get(f, i, &e));
      CHECK(e.cx - e.width / 2 >= 0);
      CHECK(e.cx + e.width / 2 <= RNF_DIAGRAM_CANVAS_WIDTH);
      if (e.kind == RNF_DIAGRAM_SHOULDER || e.kind == RNF_DIAGRAM_TRIGGER) continue;
      CHECK(e.cx >= body.x);
      CHECK(e.cx <= body.x + body.width);
      CHECK(e.cy >= body.y);
      CHECK(e.cy <= body.y + body.height);
      if (screen.width > 0) {
        bool inside = e.cx > screen.x && e.cx < screen.x + screen.width && e.cy > screen.y && e.cy < screen.y + screen.height;
        CHECK_FALSE(inside);
      }
    }
  }
  // Steam Deck: D-pad / face buttons outside the sticks (outer top), sticks above the trackpads.
  rnf_diagram_info i{};
  rnf_diagram_info_get(RNF_FAMILY_STEAM_DECK, &i);
  CHECK(i.dpad_x < i.left_stick_x);
  CHECK(i.face_x > i.right_stick_x);
  CHECK(i.dpad_y < i.left_stick_y);
}

TEST_CASE("assign picker: None first, the slot's player, the other player, hotkeys") {
  size_t n = rnf_input_assign_choices(0, nullptr, 0);
  CHECK_EQ(n, rnf_input_action_count() + 1);
  std::vector<const char*> v(n);
  CHECK_EQ(rnf_input_assign_choices(0, v.data(), n), n);
  CHECK_EQ(std::string(v[0]), "");
  CHECK_EQ(std::string(v[1]), "p1.up");
  CHECK_EQ(std::string(v[11]), "p2.up");
  CHECK_EQ(std::string(v[21]), "hk.rewind");
  rnf_input_assign_choices(1, v.data(), n);
  CHECK_EQ(std::string(v[1]), "p2.up");
  CHECK_EQ(std::string(v[11]), "p1.up");
  // Pages of six (the menu's LIST sheets): no scrolling.
  CHECK_EQ((n + RNF_MENU_MAX_ITEMS - 1) / RNF_MENU_MAX_ITEMS, size_t(6));
}

TEST_CASE("menu: Controls has the Confirm Button choice; the action picker is a LIST page under the diagram") {
  rnf_menu* m = rnf_menu_new(RNF_MENU_FEATURE_OSK);
  int controls = rnf_menu_page_find(m, "settings.controls");
  REQUIRE(controls >= 0);
  int confirm = rnf_menu_item_find(m, size_t(controls), "controls.confirm");
  REQUIRE(confirm >= 0);
  rnf_menu_item_info it{};
  REQUIRE(rnf_menu_item_get(m, size_t(controls), size_t(confirm), &it));
  CHECK_EQ(it.kind, RNF_MENU_ITEM_CHOICE);
  CHECK_EQ(it.choice_count, size_t(2));
  CHECK_EQ(rnf_menu_item_find(m, size_t(controls), "controls.turbo"), -1);  // moved to More
  CHECK(rnf_menu_item_find(m, size_t(rnf_menu_page_find(m, "controls.detail")), "controls.turbo") >= 0);
  int assign = rnf_menu_page_find(m, "controls.assign");
  REQUIRE(assign >= 0);
  rnf_menu_page_info pi{};
  rnf_menu_page_get(m, size_t(assign), &pi);
  CHECK_EQ(pi.kind, RNF_MENU_PAGE_LIST);
  // Quick > Settings > Controls > Controller > picker fits the depth limit; L / R switch sheets.
  REQUIRE(rnf_menu_open(m, "quick"));
  CHECK_EQ(rnf_menu_push(m, "settings.controls"), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(rnf_menu_push(m, "controls.controller"), RNF_MENU_EVENT_PUSHED);
  CHECK_EQ(rnf_menu_push(m, "controls.assign"), RNF_MENU_EVENT_PUSHED);
  rnf_menu_set_count(m, size_t(assign), rnf_input_assign_choices(0, nullptr, 0));
  CHECK_EQ(rnf_menu_sheet_count(m), size_t(6));
  CHECK_EQ(rnf_menu_switch(m, 1), RNF_MENU_EVENT_SWITCHED);
  CHECK_EQ(rnf_menu_focus(m), size_t(6));
  CHECK_EQ(rnf_menu_confirm(m), RNF_MENU_EVENT_ACTIVATE);
  CHECK_EQ(rnf_menu_back(m), RNF_MENU_EVENT_POPPED);
  rnf_menu_free(m);
}
