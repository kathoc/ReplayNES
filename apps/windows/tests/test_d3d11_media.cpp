// D3D11 production upload/UV/atlas/readback logic, without D3D or a GPU.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/d3d11_media.h"
#include "../src/d3d11_thumb_atlas.h"
#include "game_rect.h"
#include "support/rn_test.h"
#include <algorithm>
#include <vector>
using namespace rnl;

TEST_CASE("D3D11 media upload honors both pitches and rejects invalid frames") {
  rn_media_info media{384, 256, 5963, 100, 2, 4, 3, 1};
  std::vector<uint32_t> src(336 * 224, 0xdeadbeef), dst(400 * 256, 0xcccccccc);
  for (uint32_t y = 0; y < 224; ++y) for (uint32_t x = 0; x < 320; ++x)
    src[y * 336 + x] = 0xff000000 | (y << 12) | x;
  rn_video_frame_info frame{src.data(), 320, 224, 336};
  REQUIRE(d3d11FrameFits(media, frame));
  d3d11CopyPicture(frame, dst.data(), 400 * 4);
  for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 400; ++x)
    CHECK_EQ(dst[y * 400 + x], y < 224 && x < 320 ? src[y * 336 + x] : 0xccccccccu);
  frame.stride = 319; CHECK(!d3d11FrameFits(media, frame));
  frame.stride = 400; frame.width = 385; CHECK(!d3d11FrameFits(media, frame));
  frame.width = 320; frame.height = 257; CHECK(!d3d11FrameFits(media, frame));
  frame.height = 0; CHECK(!d3d11FrameFits(media, frame));
  frame.height = 224; frame.pixels = nullptr; CHECK(!d3d11FrameFits(media, frame));
}

TEST_CASE("D3D11 NES upload and UV endpoints exactly match the old presenter") {
  rn_media_info media{256, 240, RN_FPS_NUM, RN_FPS_DEN, 1, 8, 7, 0};
  std::vector<uint32_t> src(256 * 240), old(272 * 240, 0), now(old);
  for (size_t i = 0; i < src.size(); ++i) src[i] = uint32_t(i * 16807);
  rn_video_frame_info frame{src.data(), 256, 240, 256};
  for (int y = 0; y < 240; ++y) std::memcpy(old.data() + y * 272, src.data() + y * 256, 256 * 4);
  d3d11CopyPicture(frame, now.data(), 272 * 4);
  CHECK(old == now);
  for (int crop : {0, 8}) {
    const auto uv = d3d11PictureUV(media, frame, crop);
    const float legacy[4] = {float(crop) / 256, float(crop) / 240,
                             float(256 - crop) / 256, float(240 - crop) / 240};
    for (int i = 0; i < 4; ++i) {
      // Original shader lerp at each corner, including floating-point rounding.
      CHECK_EQ(uv[2 * i], legacy[0] + (legacy[2] - legacy[0]) * float(i & 1));
      CHECK_EQ(uv[2 * i + 1], legacy[1] + (legacy[3] - legacy[1]) * float((i >> 1) & 1));
    }
  }
}

TEST_CASE("D3D11 media UVs rotate clockwise inside the visible texture extent") {
  rn_media_info media{384, 256, 5963, 100, 2, 4, 3, 0};
  rn_video_frame_info frame{nullptr, 320, 224, 336};
  // Original corner labels: TL=0, TR=1, BL=2, BR=3.
  const int corners[4][4] = {{0, 1, 2, 3}, {2, 0, 3, 1}, {3, 2, 1, 0}, {1, 3, 0, 2}};
  for (uint32_t rotation = 0; rotation < 4; ++rotation) {
    media.rotation = rotation;
    const auto uv = d3d11PictureUV(media, frame, 0);
    for (int i = 0; i < 4; ++i) {
      const int c = corners[rotation][i];
      CHECK_EQ(uv[2 * i], c & 1 ? 320.f / 384 : 0.f);
      CHECK_EQ(uv[2 * i + 1], c & 2 ? 224.f / 256 : 0.f);
    }
    const auto rect = computeGameRect(1280, 720, media, frame, false, true, false);
    CHECK_EQ(rect.w, rotation & 1 ? 378.f : 1280.f);
    CHECK_EQ(rect.h, rotation & 1 ? 720.f : 672.f);
  }
  frame.width = 288;
  const auto smaller = d3d11PictureUV(media, frame, 0);
  CHECK_EQ(smaller[0], 288.f / 384);
}

TEST_CASE("D3D11 NES thumbnail pixels and UVs retain the original 128 by 120 cells") {
  D3D11ThumbAtlas atlas;
  std::vector<uint32_t> pixels(128 * 120);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = 0xff000000 | uint32_t(i);
  for (uint64_t k = 1; k <= 64; ++k) {
    atlas.beginFrame();
    float uv[4];
    REQUIRE(atlas.get(k, pixels.data(), 128, 120, uv));
    REQUIRE(atlas.pending.size() == 1);
    CHECK_EQ(atlas.pending[0].cell, int(k - 1));
    CHECK(atlas.pending[0].pixels == pixels);
    CHECK_EQ(uv[0], float(((k - 1) % 8) * 128) / 1024);
    CHECK_EQ(uv[1], float(((k - 1) / 8) * 128) / 1024);
    CHECK_EQ(uv[2] - uv[0], 128.f / 1024);
    CHECK_EQ(uv[3] - uv[1], 120.f / 1024);
    atlas.pending.clear();
  }
  atlas.beginFrame();
  float uv[4];
  REQUIRE(atlas.get(65, pixels.data(), 128, 120, uv));
  CHECK_EQ(atlas.pending[0].cell, 0);  // oldest cell
}

TEST_CASE("D3D11 tall thumbnails reserve all rows and keep current-frame cells alive") {
  D3D11ThumbAtlas atlas;
  atlas.beginFrame();
  std::vector<uint32_t> px(128 * 244, 0xff123456);
  float uv[4];
  for (uint64_t key = 1; key <= 32; ++key) {
    REQUIRE(atlas.get(key, px.data(), 128, 244, uv));
    CHECK_EQ(uv[3] - uv[1], 244.f / 1024);
    atlas.pending.clear();  // GPU upload completed, but UI still uses every cell
  }
  CHECK(!atlas.get(33, px.data(), 128, 244, uv));
  atlas.beginFrame();
  REQUIRE(atlas.get(33, px.data(), 128, 244, uv));
  CHECK_EQ(atlas.pending[0].cell, 0);
  atlas.clear();
  std::vector<uint32_t> tall(128 * 2048);
  for (uint32_t y = 0; y < 2048; ++y) std::fill_n(tall.data() + y * 128, 128, y);
  REQUIRE(atlas.get(1, tall.data(), 128, 2048, uv));
  CHECK_EQ(atlas.pending[0].height, 1024u);
  for (size_t y = 0; y < 1024; ++y) CHECK_EQ(atlas.pending[0].pixels[y * 128], y * 2);
  CHECK(!atlas.get(1, tall.data(), 128, 244, uv));  // conflicting dimensions cannot overwrite live content
  atlas.clear();
  for (uint64_t key = 1; key <= 12; ++key) REQUIRE(atlas.get(key, px.data(), 128, 120, uv));
  CHECK(!atlas.get(13, px.data(), 128, 120, uv));
}

TEST_CASE("D3D11 screenshot reads shown dimensions and row pitch with legacy colors") {
  for (auto size : {std::pair<unsigned, unsigned>{256, 240}, {378, 720}, {1280, 720}}) {
    const auto w = size.first, h = size.second, pitch = (w + 8) * 4;
    std::vector<uint8_t> src(size_t(pitch) * h, 0xee), rgba(size_t(w) * h * 4);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
      auto* p = src.data() + size_t(y) * pitch + x * 4;
      p[0] = uint8_t(x); p[1] = uint8_t(y); p[2] = uint8_t(x ^ y); p[3] = 0;
    }
    d3d11ScreenshotRGBA(src.data(), pitch, w, h, rgba.data());
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
      const auto* p = rgba.data() + (size_t(y) * w + x) * 4;
      CHECK_EQ(p[0], uint8_t(x ^ y)); CHECK_EQ(p[1], uint8_t(y));
      CHECK_EQ(p[2], uint8_t(x)); CHECK_EQ(p[3], 255);
    }
  }
}
