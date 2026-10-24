// Media geometry and thumbnail regressions, independent of SDL and Vulkan.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <filesystem>
#include <vector>

#include "game_rect.h"
#include "replaynes/product.h"
#include "library_thumbs.h"
#include "png_writer.h"
#include "support/rn_test.h"
#include "thumbnails.h"

using namespace rnl;

TEST_CASE("media game rectangle follows rotation and pixel aspect") {
  rn_media_info m{320, 224, 5963, 100, 2, 4, 3, 0};
  rn_video_frame_info f{nullptr, 320, 224, 336};
  for (uint32_t rotation = 0; rotation < 4; ++rotation) {
    m.rotation = rotation;
    auto r = computeGameRect(1280, 720, m, f, false, true, false);
    REQUIRE(r.visible);
    CHECK_EQ(r.crop, 0);
    if (rotation & 1) {
      CHECK_EQ(r.w, 378);
      CHECK_EQ(r.h, 720);
      CHECK_EQ(r.x, 451);
      CHECK_EQ(r.y, 0);
    } else {
      CHECK_EQ(r.w, 1280);
      CHECK_EQ(r.h, 672);
      CHECK_EQ(r.x, 0);
      CHECK_EQ(r.y, 24);
    }
  }
  m.rotation = 1;
  auto square = computeGameRect(1280, 720, m, f, false, false, false);
  CHECK_EQ(square.w, 504);
  CHECK_EQ(square.h, 720);
  auto integer = computeGameRect(1280, 720, m, f, true, true, false);
  CHECK_EQ(integer.w, 224);
  CHECK_EQ(integer.h, 427);
  f.width = 160;
  f.height = 112;
  auto smaller = computeGameRect(1280, 720, m, f, false, true, false);
  CHECK_EQ(smaller.w, 378);
  CHECK_EQ(smaller.h, 720);
}

TEST_CASE("media NES game rectangle equals legacy placement") {
  rn_media_info m{256, 240, RN_FPS_NUM, RN_FPS_DEN, 1, 8, 7, 0};
  rn_video_frame_info f{nullptr, 256, 240, 256};
  for (int width : {320, 1280, 1920}) for (int height : {240, 720, 1080})
    for (bool integer : {false, true}) for (bool par : {false, true}) for (bool crop : {false, true}) {
      auto old = computeGameRect(width, height, integer, par, crop);
      auto now = computeGameRect(width, height, m, f, integer, par, crop);
      CHECK_EQ(now.x, old.x); CHECK_EQ(now.y, old.y);
      CHECK_EQ(now.w, old.w); CHECK_EQ(now.h, old.h);
      CHECK_EQ(now.crop, old.crop); CHECK_EQ(now.crtCrop, old.crtCrop);
      CHECK_EQ(now.crt.x, old.crt.x); CHECK_EQ(now.crt.y, old.crt.y);
      CHECK_EQ(now.crt.w, old.crt.w); CHECK_EQ(now.crt.h, old.crt.h);
    }
}

TEST_CASE("media thumbnails and filmstrip follow rotated aspect and padded stride") {
  rn_media_info m{320, 224, 5963, 100, 2, 4, 3, 1};
  std::vector<uint32_t> pixels(336 * 224, 0xffff00ff);
  for (uint32_t y = 0; y < 224; ++y) for (uint32_t x = 0; x < 320; ++x)
    pixels[size_t(y) * 336 + x] = y < 112 ? (x < 160 ? 0xffff0000 : 0xff00ff00)
                                               : (x < 160 ? 0xff0000ff : 0xffffff00);
  rn_video_frame_info f{pixels.data(), 320, 224, 336};
  uint32_t w = 0, h = 0;
  REQUIRE(thumbnailSize(f, m, &w, &h));
  CHECK_EQ(w, 128u); CHECK_EQ(h, 244u);
  // Filmstrip width is the strip height multiplied by this display aspect.
  CHECK(std::abs(120.0 * w / h - 62.9508196721) < 1e-8);
  ThumbRef img(ThumbImage::make(f, m));
  REQUIRE(img);
  CHECK_EQ(img.get()->width, w); CHECK_EQ(img.get()->height, h);
  const auto& p = img.get()->px;
  CHECK_EQ(p.front(), 0xff0000ffu);
  CHECK_EQ(p[w - 1], 0xffff0000u);
  CHECK_EQ(p[size_t(h - 1) * w], 0xffffff00u);
  CHECK_EQ(p.back(), 0xff00ff00u);
  std::vector<uint8_t> rgba(p.size() * 4);
  for (size_t i = 0; i < p.size(); ++i) {
    rgba[4*i] = uint8_t(p[i] >> 16); rgba[4*i+1] = uint8_t(p[i] >> 8);
    rgba[4*i+2] = uint8_t(p[i]); rgba[4*i+3] = 255;
  }
  CHECK(writePNG(std::string(RNL_TEST_TMP) + "/media-rotated-thumbnail.png", int(w), int(h), rgba.data()));
  const std::string cache = std::string(RNL_TEST_TMP) + "/media-thumbnail-cache";
  {
    LibraryThumbs library(cache);
    library.save("rotated-project", "", f, m);
  }
  {
    LibraryThumbs library(cache);
    auto restored = library.forProject("rotated-project");
    REQUIRE(restored);
    CHECK_EQ(restored.get()->width, w); CHECK_EQ(restored.get()->height, h);
    CHECK(restored.get()->px == p);
  }
  std::filesystem::remove_all(cache);
}

TEST_CASE("external NES-sized media keeps its declared pixel aspect") {
  rn_media_info m{256, 240, 60, 1, 2, 8, 7, 0};
  rn_video_frame_info f{nullptr, 256, 240, 256};
  uint32_t width = 0, height = 0;
  REQUIRE(thumbnailSize(f, m, &width, &height));
  CHECK_EQ(width, 128u);
  CHECK_EQ(height, 105u);
}

TEST_CASE("media NES thumbnail pixels exactly match legacy downscaler") {
  rn_media_info m{256, 240, RN_FPS_NUM, RN_FPS_DEN, 1, 8, 7, 0};
  std::vector<uint32_t> pixels(256 * 240);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = 0xff000000u | (uint32_t(i * 34723) & 0xffffffu);
  rn_video_frame_info f{pixels.data(), 256, 240, 256};
  std::vector<uint32_t> expected(128 * 120);
  rnf_thumb_downscale(pixels.data(), expected.data());
  ThumbRef img(ThumbImage::make(f, m, true));
  REQUIRE(img);
  CHECK_EQ(img.get()->width, 128u); CHECK_EQ(img.get()->height, 120u);
  CHECK(img.get()->px == expected);
}

TEST_CASE("product overscan policy: fixed picture and CRT geometry") {
  const auto uncropped = computeGameRect(1280, 720, false, false, false);
  CHECK_EQ(uncropped.x, 256); CHECK_EQ(uncropped.y, 0);
  CHECK_EQ(uncropped.w, 768); CHECK_EQ(uncropped.h, 720);
  CHECK_EQ(uncropped.crop, 0); CHECK_EQ(uncropped.crtCrop, 0);
  CHECK_EQ(uncropped.crt.x, 160); CHECK_EQ(uncropped.crt.w, 960);
  const auto requested = computeGameRect(1280, 720, false, false, true);
#if RN_PRODUCT_OVERSCAN_CROP
  CHECK_EQ(requested.x, 255); CHECK_EQ(requested.y, 0);
  CHECK_EQ(requested.w, 771); CHECK_EQ(requested.h, 720);
  CHECK_EQ(requested.crop, 8); CHECK_EQ(requested.crtCrop, 8.0 / 240);
  CHECK_EQ(requested.crt.x, 126); CHECK_EQ(requested.crt.w, 1029);
#else
  CHECK_EQ(requested.x, uncropped.x); CHECK_EQ(requested.y, uncropped.y);
  CHECK_EQ(requested.w, uncropped.w); CHECK_EQ(requested.h, uncropped.h);
  CHECK_EQ(requested.crop, 0); CHECK_EQ(requested.crtCrop, 0);
  CHECK_EQ(requested.crt.x, uncropped.crt.x); CHECK_EQ(requested.crt.w, uncropped.crt.w);
#endif
  rn_media_info media{320, 224, 60, 1, 2, 1, 1, 0};
  rn_video_frame_info frame{nullptr, 320, 224, 336};
  for (uint32_t rotation = 0; rotation < 4; ++rotation) {
    media.rotation = rotation;
    const auto rect = computeGameRect(1280, 720, media, frame, false, false, true);
    CHECK_EQ(rect.crop, RN_PRODUCT_OVERSCAN_CROP ? 8 : 0);
#if !RN_PRODUCT_OVERSCAN_CROP
    const auto full = computeGameRect(1280, 720, media, frame, false, false, false);
    CHECK_EQ(rect.x, full.x); CHECK_EQ(rect.y, full.y);
    CHECK_EQ(rect.w, full.w); CHECK_EQ(rect.h, full.h);
#endif
  }
}
