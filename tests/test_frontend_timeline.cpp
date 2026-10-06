// Frontend core: timeline geometry, A/B range gestures, take lineage, visible ranges (also on a
// real session), filmstrip grid, thumbnail cache policy and the downscaler.
// Ported from the macOS TimelineTests.
#include <algorithm>
#include <atomic>
#include <set>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

rn_take_info take(uint64_t id, uint64_t parent, uint64_t branch, uint64_t length) {
  rn_take_info t{};
  t.id = id;
  t.parent_id = parent;
  t.branch_frame = branch;
  t.length = length;
  t.created_seq = id;
  return t;
}

uint64_t frameAt(double w, uint64_t len, double x) { return rnf_timeline_frame_at_x(w, len, x); }
double xFor(double w, uint64_t len, uint64_t f) { return rnf_timeline_x_for_frame(w, len, f); }

std::pair<uint64_t, uint64_t> drag(rnf_timeline_handle h, uint64_t a, uint64_t b, double x, double w, uint64_t len) {
  uint64_t na, nb;
  rnf_timeline_drag(h, a, b, x, w, len, &na, &nb);
  return {na, nb};
}

std::string hit(double x, const std::vector<rnf_timeline_range>& r, double w, uint64_t len, double tol = 5,
                int preferred = -1) {
  rnf_timeline_hit h = rnf_timeline_hit_test(x, r.data(), r.size(), w, len, tol, preferred >= 0, preferred);
  if (h.kind == RNF_HIT_NONE) return "none";
  if (h.kind == RNF_HIT_BODY) return "body" + std::to_string(h.slot);
  return "handle" + std::to_string(h.slot) + (h.handle == RNF_HANDLE_A ? "a" : "b");
}

rnf_timeline_range range(int slot, uint64_t a, int64_t b = -1) {
  return rnf_timeline_range{slot, a, b >= 0 ? 1 : 0, b >= 0 ? uint64_t(b) : 0};
}

std::vector<uint64_t> frames(size_t (*fn)(uint64_t, double, uint64_t*, size_t), uint64_t len, double f) {
  std::vector<uint64_t> v(fn(len, f, nullptr, 0));
  fn(len, f, v.data(), v.size());
  return v;
}

std::vector<rnf_thumb_tile> tiles(uint64_t len, double f, double tw) {
  std::vector<rnf_thumb_tile> v(rnf_thumb_tiles(len, f, tw, nullptr, 0));
  rnf_thumb_tiles(len, f, tw, v.data(), v.size());
  return v;
}

const double tw = 256.0 / 240.0 * 40;

// Reference-counted fake image payloads.
struct Img {
  std::atomic<int> refs{0};
};
void retainImg(void* p) { static_cast<Img*>(p)->refs++; }
void releaseImg(void* p) { static_cast<Img*>(p)->refs--; }

}  // namespace

TEST_CASE("geometry mapping") {
  CHECK_EQ(frameAt(600, 1200, 0), uint64_t(0));
  CHECK_EQ(frameAt(600, 1200, 300), uint64_t(600));
  CHECK_EQ(frameAt(600, 1200, 600), uint64_t(1200));
  CHECK_EQ(frameAt(600, 1200, -50), uint64_t(0));
  CHECK_EQ(frameAt(600, 1200, 9999), uint64_t(1200));
  CHECK(near(xFor(600, 1200, 600), 300, 1e-9));
  CHECK(near(xFor(600, 1200, 5000), 600, 1e-9));
  for (uint64_t f : {0, 1, 2, 599, 1199, 1200}) CHECK_EQ(frameAt(600, 1200, xFor(600, 1200, f)), f);
  CHECK_EQ(frameAt(600, 0, 300), uint64_t(0));
  CHECK_EQ(xFor(0, 100, 50), 0.0);
}

TEST_CASE("range gesture to frames") {
  uint64_t a = 0, b = 0;
  REQUIRE(rnf_timeline_range_from_drag(500, 1000, 100, 250, &a, &b));
  CHECK(a == 200 && b == 500);
  REQUIRE(rnf_timeline_range_from_drag(500, 1000, 250, 100, &a, &b));
  CHECK(a == 200 && b == 500);
  REQUIRE(rnf_timeline_range_from_drag(500, 1000, -20, 900, &a, &b));
  CHECK(a == 0 && b == 1000);
  CHECK_FALSE(rnf_timeline_range_from_drag(500, 1000, 100, 100.2, &a, &b));
  CHECK((drag(RNF_HANDLE_A, 200, 500, 50, 500, 1000) == std::make_pair<uint64_t, uint64_t>(100, 500)));
  CHECK((drag(RNF_HANDLE_A, 200, 500, 400, 500, 1000) == std::make_pair<uint64_t, uint64_t>(499, 500)));
  CHECK((drag(RNF_HANDLE_B, 200, 500, 450, 500, 1000) == std::make_pair<uint64_t, uint64_t>(200, 900)));
  CHECK((drag(RNF_HANDLE_B, 200, 500, 10, 500, 1000) == std::make_pair<uint64_t, uint64_t>(200, 201)));
  CHECK((drag(RNF_HANDLE_B, 200, 500, 800, 500, 1000) == std::make_pair<uint64_t, uint64_t>(200, 1000)));
}

TEST_CASE("hit test") {
  std::vector<rnf_timeline_range> r = {range(0, 100, 300), range(1, 280, 600), range(2, 800)};
  CHECK_EQ(hit(102, r, 1000, 1000), "handle0a");
  CHECK_EQ(hit(150, r, 1000, 1000), "body0");
  CHECK_EQ(hit(298, r, 1000, 1000), "handle0b");
  CHECK_EQ(hit(283, r, 1000, 1000), "handle1a");
  CHECK_EQ(hit(290, r, 1000, 1000, 10), "handle1a");
  CHECK_EQ(hit(290, r, 1000, 1000, 10, 0), "handle0b");
  CHECK_EQ(hit(290.5, r, 1000, 1000, 1), "body1");
  CHECK_EQ(hit(290.5, r, 1000, 1000, 1, 0), "body0");
  CHECK_EQ(hit(801, r, 1000, 1000), "handle2a");
  CHECK_EQ(hit(850, r, 1000, 1000), "none");
  CHECK_EQ(hit(700, r, 1000, 1000), "none");
}

TEST_CASE("mark at playhead") {
  rnf_timeline_range r = range(0, 100, 300), aOnly = range(0, 5), a100 = range(0, 100);
  uint64_t a = 0, b = 0;
  char* msg = nullptr;
  CHECK_EQ(rnf_timeline_mark_a(50, &r, &a, &b, &msg), RNF_MARK_SET_RANGE);
  CHECK(a == 50 && b == 300);
  CHECK_EQ(rnf_timeline_mark_a(299, &r, &a, &b, &msg), RNF_MARK_SET_RANGE);
  CHECK(a == 299 && b == 300);
  CHECK_EQ(rnf_timeline_mark_a(300, &r, &a, &b, &msg), RNF_MARK_SET_A_ONLY);
  CHECK_EQ(rnf_timeline_mark_a(10, nullptr, &a, &b, &msg), RNF_MARK_SET_A_ONLY);
  CHECK_EQ(rnf_timeline_mark_a(10, &aOnly, &a, &b, &msg), RNF_MARK_SET_A_ONLY);
  CHECK_EQ(rnf_timeline_mark_b(400, &r, &a, &b, &msg), RNF_MARK_SET_RANGE);
  CHECK(a == 100 && b == 400);
  CHECK_EQ(rnf_timeline_mark_b(150, &a100, &a, &b, &msg), RNF_MARK_SET_RANGE);
  CHECK(a == 100 && b == 150);
  CHECK_EQ(rnf_timeline_mark_b(100, &r, &a, &b, &msg), RNF_MARK_INVALID);
  CHECK_EQ(take(msg), "Set B at a position after A");
  CHECK_EQ(rnf_timeline_mark_b(100, nullptr, &a, &b, &msg), RNF_MARK_INVALID);
  CHECK_EQ(take(msg), "Set A of this section first");
}

TEST_CASE("shared prefix") {
  std::vector<rn_take_info> t = {take(1, 0, 0, 1000), take(2, 1, 400, 900), take(3, 2, 600, 700), take(4, 1, 700, 750)};
  auto sp = [&](uint64_t x, uint64_t y) { return rnf_take_shared_prefix(t.data(), t.size(), x, y); };
  CHECK_EQ(sp(1, 1), uint64_t(1000));
  CHECK_EQ(sp(1, 2), uint64_t(400));
  CHECK_EQ(sp(2, 1), uint64_t(400));
  CHECK_EQ(sp(2, 3), uint64_t(600));
  CHECK_EQ(sp(3, 4), uint64_t(400));
  CHECK_EQ(sp(4, 1), uint64_t(700));
  CHECK_EQ(sp(2, 4), uint64_t(400));
  CHECK_EQ(sp(0, 1), uint64_t(0));
  CHECK_EQ(sp(1, 99), uint64_t(0));
}

TEST_CASE("visible ranges") {
  std::vector<rn_take_info> t = {take(1, 0, 0, 1000), take(2, 1, 400, 900)};
  std::vector<rnf_practice_slot> s(5);
  for (int i = 0; i < 5; ++i) s[size_t(i)].index = i;
  s[0].has_a = s[0].has_b = s[0].has_take_frame = 1; s[0].take_frame = 100; s[0].length = 200; s[0].take_id = 1;
  s[1].has_a = s[1].has_b = s[1].has_take_frame = 1; s[1].take_frame = 350; s[1].length = 100; s[1].take_id = 1;
  s[2].has_a = 1;
  s[3].has_a = s[3].has_take_frame = 1; s[3].take_frame = 500; s[3].take_id = 2;
  auto vis = [&](uint64_t active, uint64_t len) {
    std::vector<rnf_timeline_range> out(s.size());
    out.resize(rnf_timeline_visible_ranges(s.data(), s.size(), t.data(), t.size(), active, len, out.data(), out.size()));
    std::vector<std::string> d;
    for (auto& r : out) d.push_back(std::to_string(r.slot) + ":" + std::to_string(r.a) + "-" + (r.has_b ? std::to_string(r.b) : "nil"));
    return d;
  };
  CHECK((vis(2, 900) == std::vector<std::string>{"0:100-300", "3:500-nil"}));
  CHECK((vis(1, 1000) == std::vector<std::string>{"0:100-300", "1:350-450"}));
}

TEST_CASE("visible ranges on an engine session") {
  std::string dir = rntest::tempDir("frontend-timeline");
  std::string rom = rntest::writeTestRom(dir);
  rn_session_options o;
  rn_session_options_init(&o);
  rn_session* s = nullptr;
  REQUIRE_EQ(rn_session_new(rom.c_str(), nullptr, &o, &s), RN_OK);
  for (int i = 0; i < 300; ++i) rn_step(s, i % 7 == 0 ? RN_BTN_A : 0, 0, 0, nullptr);
  REQUIRE_EQ(rn_practice_set_range(s, 2, 60, 180), RN_OK);
  auto visible = [&]() {
    std::vector<rnf_practice_slot> slots;
    for (uint32_t i = 0; i < RN_PRACTICE_SLOTS; ++i) {
      rn_practice_slot_info p{};
      rnf_practice_slot q{};
      q.index = int(i);
      if (rn_practice_slot_get(s, i, &p) == RN_OK && p.has_a) {
        q.has_a = 1; q.has_b = p.has_b; q.has_take_frame = p.has_take_frame;
        q.take_frame = p.take_frame; q.length = p.length_frames; q.take_id = p.take_id;
      }
      slots.push_back(q);
    }
    std::vector<rn_take_info> takes(rn_take_count(s));
    for (size_t i = 0; i < takes.size(); ++i) rn_take_get(s, i, &takes[i]);
    std::vector<rnf_timeline_range> out(slots.size());
    out.resize(rnf_timeline_visible_ranges(slots.data(), slots.size(), takes.data(), takes.size(), rn_active_take(s),
                                           rn_take_length(s), out.data(), out.size()));
    return out;
  };
  auto v = visible();
  REQUIRE_EQ(v.size(), size_t(1));
  CHECK(v[0].slot == 2 && v[0].a == 60 && v[0].has_b && v[0].b == 180);
  // A branch before B hides the range on the new take.
  rn_seek(s, 100);
  rn_step(s, RN_BTN_B, 0, 0, nullptr);
  CHECK(visible().empty());
  rn_undo_take_switch(s);
  CHECK_EQ(visible().size(), size_t(1));
  rn_session_close(s);
}

// ------------------------------------------------------------------ filmstrip grid

TEST_CASE("thumbnail steps") {
  CHECK_EQ(RNF_THUMB_BASE_STEP, 300.0);
  CHECK_EQ(rnf_thumb_step(RNF_THUMB_MIN_EXPONENT), 300.0);
  CHECK(rnf_thumb_is_step(300));
  CHECK(rnf_thumb_is_step(2400));
  CHECK_FALSE(rnf_thumb_is_step(150));
  CHECK_FALSE(rnf_thumb_is_step(450));
  CHECK_FALSE(rnf_thumb_is_step(0));
  auto fine = frames(rnf_thumb_frames, 100000, 600), coarse = frames(rnf_thumb_frames, 100000, 1200);
  std::set<uint64_t> fs(fine.begin(), fine.end());
  for (auto f : coarse) CHECK(fs.count(f));
  CHECK(frames(rnf_thumb_frames, 299, 300).empty());
  CHECK((frames(rnf_thumb_frames, 1000, 300) == std::vector<uint64_t>{300, 600, 900}));
  CHECK(rnf_thumb_is_grid_frame(900, 300));
  CHECK_FALSE(rnf_thumb_is_grid_frame(901, 300));
  CHECK_FALSE(rnf_thumb_is_grid_frame(0, 300));
  CHECK_EQ(rnf_thumb_start_picture(300), uint64_t(60));
  CHECK(frames(rnf_thumb_targets, 59, 300).empty());
  CHECK((frames(rnf_thumb_targets, 60, 300) == std::vector<uint64_t>{60}));
  CHECK((frames(rnf_thumb_targets, 1000, 300) == std::vector<uint64_t>{60, 300, 600, 900}));
  CHECK((frames(rnf_thumb_targets, 2400, 1200) == std::vector<uint64_t>{60, 1200, 2400}));
  CHECK_EQ(rnf_thumb_fallback_window(300), uint64_t(300));
}

TEST_CASE("scale is the smallest step that fits the take") {
  for (double w : {300.0, 575.0, 800.0, 1400.0}) {
    for (uint64_t len = 0; len <= 400000; len += 97) {
      double f = rnf_thumb_tile_step(w, tw, len);
      CHECK(f >= 300);
      CHECK(rnf_thumb_is_step(f));
      CHECK(rnf_thumb_extent(len, f, tw) <= w + 1e-9);
      if (f > 300) CHECK(rnf_thumb_extent(len, f / 2, tw) > w);
      auto t = tiles(len, f, tw);
      CHECK_EQ(t.size(), size_t(std::ceil(double(len) / f)));
      std::set<uint64_t> pics;
      for (auto& x : t) pics.insert(x.picture);
      CHECK_EQ(pics.size(), t.size());
    }
  }
  CHECK_EQ(rnf_thumb_tile_step(575, tw, 120), 300.0);
  CHECK_EQ(rnf_thumb_tile_step(575, tw, 4000), 300.0);
  CHECK_EQ(rnf_thumb_tile_step(575, tw, 4100), 600.0);
  CHECK_EQ(rnf_thumb_tile_step(575, tw, 36000), 4800.0);
}

TEST_CASE("tiles have their natural width, revealed up to the extent") {
  auto t = tiles(420, 300, tw);
  REQUIRE_EQ(t.size(), size_t(2));
  CHECK(t[0].frame == 0 && t[1].frame == 300);
  CHECK(t[0].picture == 60 && t[1].picture == 300);
  CHECK_EQ(t[0].x, 0.0);
  CHECK(near(t[0].visible, tw, 1e-9));
  CHECK(near(t[1].x, tw, 1e-9));
  CHECK(near(t[1].visible, double(420 - 300) * tw / 300, 1e-9));
  for (uint64_t len = 1; len <= 5000; ++len) {
    double f = rnf_thumb_tile_step(575, tw, len);
    double end = rnf_thumb_extent(len, f, tw);
    auto x = tiles(len, f, tw);
    for (size_t i = 0; i + 1 < x.size(); ++i) CHECK(near(x[i].visible, tw, 1e-9));
    CHECK(near(x.back().x + x.back().visible, end, 1e-9));
    CHECK(x.back().visible > 0);
    CHECK(x.back().visible <= tw + 1e-9);
  }
  CHECK_EQ(tiles(300, 300, tw).size(), size_t(1));
  CHECK_EQ(tiles(301, 300, tw).size(), size_t(2));
  CHECK(tiles(0, 300, tw).empty());
}

TEST_CASE("frame mapping at the fixed scale") {
  const uint64_t len = 1000;
  const double f = 300, w = rnf_thumb_extent(len, f, tw);
  CHECK(near(xFor(w, len, 300), tw, 1e-9));
  CHECK(near(xFor(w, len, 600), 2 * tw, 1e-9));
  CHECK_EQ(frameAt(w, len, tw), uint64_t(300));
  CHECK_EQ(frameAt(w, len, 10000), len);
  CHECK_EQ(frameAt(w, len, -5), uint64_t(0));
  CHECK_EQ(xFor(w, len, 5000), w);
}

TEST_CASE("playhead snaps to whole pixels") {
  CHECK_EQ(rnf_thumb_snap_to_pixel(10.74, 2), 10.5);
  CHECK_EQ(rnf_thumb_snap_to_pixel(10.49, 2), 10.0);
  CHECK_EQ(rnf_thumb_snap_to_pixel(10.5, 2), 10.5);
  CHECK_EQ(rnf_thumb_snap_to_pixel(3.99, 1), 3.0);
  const double px = tw / 300;
  double last = 0;
  for (uint64_t len = 0; len <= 3000; ++len) {
    double x = rnf_thumb_snap_to_pixel(double(len) * px, 2);
    CHECK((x == last || std::fabs(x - last - 0.5) < 1e-9));
    last = x;
  }
}

// ------------------------------------------------------------------ cache

TEST_CASE("thumbnail cache invalidation") {
  std::vector<Img> imgs(64);
  size_t next = 0;
  auto img = [&]() { return static_cast<void*>(&imgs[next++]); };
  rnf_thumb_cache* c = rnf_thumb_cache_new(retainImg, releaseImg);
  int changes = 0;
  rnf_thumb_cache_set_on_change(c, [](void* ctx) { ++*static_cast<int*>(ctx); }, &changes);
  rnf_thumb_cache_reset(c, 1);
  rnf_thumb_cache_set_step(c, 10);
  CHECK(rnf_thumb_cache_wants(c, 10, 1));
  CHECK(rnf_thumb_cache_wants(c, rnf_thumb_start_picture(10), 1));
  CHECK_FALSE(rnf_thumb_cache_wants(c, 15, 1));
  CHECK_FALSE(rnf_thumb_cache_wants(c, 0, 1));
  CHECK_FALSE(rnf_thumb_cache_wants(c, 10, 2));
  for (uint64_t f = 10; f <= 100; f += 10) CHECK(rnf_thumb_cache_insert(c, f, 1, 0, 0, img()));
  CHECK_FALSE(rnf_thumb_cache_wants(c, 10, 1));
  CHECK_EQ(rnf_thumb_cache_count(c), size_t(10));
  CHECK_FALSE(rnf_thumb_cache_insert(c, 110, 2, 0, 0, img()));
  CHECK_EQ(imgs[10].refs.load(), 0);  // rejected: not retained

  uint64_t gen = rnf_thumb_cache_generation(c);
  rnf_thumb_cache_rebase(c, 2, 40);
  CHECK_EQ(rnf_thumb_cache_take(c), uint64_t(2));
  CHECK(rnf_thumb_cache_generation(c) != gen);
  uint64_t want[] = {10, 20, 30, 40, 50, 60}, out[6];
  size_t n = rnf_thumb_cache_missing(c, want, 6, out, 6);
  CHECK(n == 2 && out[0] == 50 && out[1] == 60);
  CHECK_EQ(imgs[5].refs.load(), 0);  // frame 60 dropped: released
  CHECK_EQ(imgs[3].refs.load(), 1);  // frame 40 kept
  CHECK_FALSE(rnf_thumb_cache_insert(c, 50, 2, 1, gen, img()));
  CHECK(rnf_thumb_cache_insert(c, 50, 2, 1, rnf_thumb_cache_generation(c), img()));
  rnf_thumb_cache_rebase(c, 2, 0);
  CHECK_EQ(rnf_thumb_cache_count(c), size_t(5));

  void* p = rnf_thumb_cache_image_at(c, 30);
  CHECK(p == &imgs[2]);
  CHECK_EQ(imgs[2].refs.load(), 2);  // returned retained
  releaseImg(p);
  CHECK(rnf_thumb_cache_image_at(c, 33) == nullptr);
  p = rnf_thumb_cache_image_before(c, 45, 5);
  CHECK(p == &imgs[3]);
  if (p) releaseImg(p);
  CHECK(rnf_thumb_cache_image_before(c, 45, 4) == nullptr);
  CHECK(rnf_thumb_cache_image_before(c, 10, 100) == nullptr);
  CHECK(rnf_thumb_cache_image_before(c, 500, 100) == nullptr);
  uint64_t v = rnf_thumb_cache_version(c);
  uint64_t bf[] = {60, 70};
  void* bp[] = {img(), img()};
  CHECK(rnf_thumb_cache_insert_batch(c, bf, bp, 2, 2, rnf_thumb_cache_generation(c)));
  CHECK_EQ(rnf_thumb_cache_version(c), v + 1);
  uint64_t bf2[] = {80};
  void* bp2[] = {img()};
  CHECK_FALSE(rnf_thumb_cache_insert_batch(c, bf2, bp2, 1, 2, rnf_thumb_cache_generation(c) - 1));
  CHECK_EQ(rnf_thumb_cache_count(c), size_t(7));

  rnf_thumb_cache_reset(c, 7);
  CHECK_EQ(rnf_thumb_cache_count(c), size_t(0));
  CHECK(rnf_thumb_cache_image_at(c, 30) == nullptr);
  for (auto& i : imgs) CHECK_EQ(i.refs.load(), 0);

  rnf_thumb_cache_set_capacity(c, 5);
  for (uint64_t f = 4; f <= 40; f += 4) rnf_thumb_cache_insert(c, f, 7, 0, 0, img());
  rnf_thumb_cache_set_step(c, 8);
  uint64_t grid[] = {8, 16, 24, 32, 40};
  CHECK_EQ(rnf_thumb_cache_missing(c, grid, 5, nullptr, 0), size_t(0));
  CHECK_EQ(rnf_thumb_cache_count(c), size_t(6));  // grid + tile 0's picture (frame 4 at this step)
  CHECK(changes > 10);
  rnf_thumb_cache_free(c);
  for (auto& i : imgs) CHECK_EQ(i.refs.load(), 0);
}

TEST_CASE("downscaler") {
  std::vector<uint32_t> px(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT);
  for (int y = 0; y < RN_VIDEO_HEIGHT; ++y)
    for (int x = 0; x < RN_VIDEO_WIDTH; ++x)
      px[size_t(y * RN_VIDEO_WIDTH + x)] = x < 128 ? 0xFFFF0000u : ((x + y) % 2 == 0 ? 0xFFFFFFFFu : 0xFF000000u);
  std::vector<uint32_t> out(RNF_THUMB_WIDTH * RNF_THUMB_HEIGHT);
  rnf_thumb_downscale(px.data(), out.data());
  CHECK_EQ(out[0], 0xFFFF0000u);
  CHECK_EQ(out[119 * 128 + 63], 0xFFFF0000u);
  CHECK_EQ(out[10 * 128 + 80], 0xFF7F7F7Fu);
}
