// Media geometry and thumbnail regressions, independent of SDL and Vulkan.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include "game_rect.h"
#include "replaynes/product.h"
#include "library_thumbs.h"
#include "png_writer.h"
#include "qoi.h"
#include "render/backdrop.h"
#include "settings.h"
#include "render/surface_plan.h"
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

TEST_CASE("arcade pixel grids that are not their screen's shape are shown 4:3") {
  // FinalBurn Neo's drivers: Strider 384 x 224 and Kid Niki 384 x 256, both on 4:3 screens; the pixel
  // aspect as FbneoCore derives it (4 * h : 3 * w). Square pixels stretched them to 1.71 and 1.50.
  struct { uint32_t w, h, num, den; } games[] = {{384, 224, 7, 9}, {384, 256, 8, 9}, {320, 224, 14, 15}};
  for (const auto& g : games) {
    rn_media_info m{g.w, g.h, 5963, 100, 2, g.num, g.den, 0};
    rn_video_frame_info f{nullptr, g.w, g.h, g.w};
    auto r = computeGameRect(1920, 1080, m, f, false, true, false);
    CHECK_EQ(r.w, 1440);
    CHECK_EQ(r.h, 1080);
    m.rotation = 1;  // a vertical game: 3:4
    auto v = computeGameRect(1920, 1080, m, f, false, true, false);
    CHECK_EQ(v.w, 810);
    CHECK_EQ(v.h, 1080);
  }
}

TEST_CASE("QOI decoding, every chunk kind; cut short or not QOI is refused") {
  // 3 x 2: RGBA, RUN (+1), DIFF, LUMA, INDEX (of the first colour), then the end marker.
  const std::vector<uint8_t> qoi = {'q', 'o', 'i', 'f', 0, 0, 0, 3, 0, 0, 0, 2, 4, 0,
                                    0xFF, 10, 20, 30, 40,  // QOI_OP_RGBA
                                    0xC1,                  // QOI_OP_RUN: 2 pixels
                                    0x76,                  // QOI_OP_DIFF: r +1, g -1, b 0
                                    0xA5, 0xA5,            // QOI_OP_LUMA: g +5, r - g +2, b - g -3
                                    0x0C,                  // QOI_OP_INDEX: (10*3 + 20*5 + 30*7 + 40*11) % 64
                                    0, 0, 0, 0, 0, 0, 0, 1};
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> px;
  REQUIRE(decodeQoi(qoi.data(), qoi.size(), &w, &h, &px));
  CHECK_EQ(w, 3u);
  CHECK_EQ(h, 2u);
  const std::vector<uint8_t> want = {10, 20, 30, 40, 10, 20, 30, 40, 10, 20, 30, 40,
                                     11, 19, 30, 40, 18, 24, 32, 40, 10, 20, 30, 40};
  CHECK(px == want);
  CHECK_FALSE(decodeQoi(qoi.data(), qoi.size() - 10, &w, &h, &px));  // cut inside the chunks
  std::vector<uint8_t> png = qoi;
  png[0] = 0x89;
  CHECK_FALSE(decodeQoi(png.data(), png.size(), &w, &h, &px));
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

TEST_CASE("backdrop: only where the picture leaves the screen empty, covering it, cut to the screen") {
  // A 4:3 picture pillarboxed on 1280 x 800: shown, covering the width, centered.
  BackdropPlan p = planBackdrop(107, 0, 1067, 800, 1280, 800, 320, 240);
  REQUIRE(p.show);
  CHECK(std::fabs(p.w - 1280) < 0.01f);
  CHECK(std::fabs(p.h - 1280 * 800.f / 1067) < 0.5f);
  CHECK(std::fabs(p.y - (800 - p.h) / 2) < 0.01f);
  CHECK(std::fabs(p.lod - std::log2(320 / backdropTexels(2))) < 1e-4f);  // the default: low
  CHECK_EQ(p.spread, 1.f);
  // Stronger levels read coarser levels; none draws nothing; sharp reads level 0 without the tent.
  const BackdropPlan sharp = planBackdrop(107, 0, 1067, 800, 1280, 800, 320, 240, 1);
  REQUIRE(sharp.show);
  CHECK_EQ(sharp.lod, 0.f);
  CHECK_EQ(sharp.spread, 0.f);
  CHECK(planBackdrop(107, 0, 1067, 800, 1280, 800, 320, 240, 4).lod > planBackdrop(107, 0, 1067, 800, 1280, 800, 320, 240, 3).lod);
  CHECK(planBackdrop(107, 0, 1067, 800, 1280, 800, 320, 240, 3).lod > p.lod);
  CHECK(p.lod > sharp.lod);
  // User decision 2026-11-03: high is finer than the former low (24 texels across).
  CHECK(backdropTexels(4) > 24.f);
  CHECK_FALSE(planBackdrop(107, 0, 1067, 800, 1280, 800, 320, 240, 0).show);
  CHECK_EQ(p.dim, kBackdropDim);
  CHECK(p.dim < 1.f);  // dimmed
  // The picture fills the screen (FILL): nothing to cover.
  CHECK_FALSE(planBackdrop(0, 0, 1280, 800, 1280, 800, 320, 240).show);
  CHECK_FALSE(planBackdrop(0.5f, 0, 1279.5f, 800, 1280, 800, 320, 240).show);
  // Cut to the screen: the rectangle becomes the screen, the source the matching middle part.
  for (int rot = 0; rot < 4; ++rot) {
    BackdropPlan c = p;
    float uv[4] = {0, 0, 1, 1};
    cropBackdrop(c, 1280, 800, rot, uv);
    CHECK(std::fabs(c.x) < 0.01f);
    CHECK(std::fabs(c.y) < 0.01f);
    CHECK(std::fabs(c.w - 1280) < 0.01f);
    CHECK(std::fabs(c.h - 800) < 0.01f);
    const float cut = (p.h - 800) / 2 / p.h;  // of the height, at the top and the bottom
    // Rotation 0 / 2 cut the source's v, 1 / 3 its u (the picture's height is the source's width).
    const float lo = (rot & 1) ? uv[0] : uv[1], hi = (rot & 1) ? uv[2] : uv[3];
    const float keep0 = (rot & 1) ? uv[1] : uv[0], keep1 = (rot & 1) ? uv[3] : uv[2];
    CHECK(std::fabs(lo - cut) < 1e-4f);
    CHECK(std::fabs(hi - (1 - cut)) < 1e-4f);
    CHECK(std::fabs(keep0) < 1e-6f);
    CHECK(std::fabs(keep1 - 1) < 1e-6f);
  }
  // A rectangle cut off-center (rotation 1): the source side follows the rotation.
  BackdropPlan off;
  off.show = true;
  off.x = -100, off.y = 0, off.w = 1000, off.h = 800;  // its left 10 % is off screen
  float uv[4] = {0, 0, 1, 1};
  cropBackdrop(off, 1280, 800, 1, uv);
  CHECK(std::fabs(off.x) < 0.01f);
  CHECK(std::fabs(off.w - 900) < 0.01f);
  // q = (p.y, 1 - p.x): p.x in [0.1, 1] -> v in [0, 0.9]; u untouched.
  CHECK(std::fabs(uv[1]) < 1e-5f);
  CHECK(std::fabs(uv[3] - 0.9f) < 1e-5f);
  CHECK(std::fabs(uv[0]) < 1e-6f);
  CHECK(std::fabs(uv[2] - 1) < 1e-6f);
}

TEST_CASE("surface plan: every device's orientation and size give an identity swapchain the size of the window") {
  auto caps = [](uint32_t w, uint32_t h, uint32_t current, uint32_t supported = kSurfaceIdentity | kSurfaceRotate90 |
                                                                                kSurfaceRotate180 | kSurfaceRotate270) {
    SurfaceCaps c;
    c.width = w, c.height = h, c.currentTransform = current, c.supportedTransforms = supported;
    c.maxWidth = std::max(w, h) == kSurfaceUndefinedExtent ? 16384 : (w == kSurfaceUndefinedExtent ? 16384 : w);
    c.maxHeight = w == kSurfaceUndefinedExtent ? 16384 : h;
    return c;
  };
  // The desktop (X11): the extent undefined, the window's size.
  SurfacePlan p = planSurface(caps(kSurfaceUndefinedExtent, kSurfaceUndefinedExtent, kSurfaceIdentity), 1280, 800);
  CHECK_EQ(p.width, 1280u);
  CHECK_EQ(p.height, 800u);
  CHECK_EQ(p.transform, kSurfaceIdentity);
  CHECK_EQ(p.rotation, 0);
  // Retroid Pocket 5 (Adreno): a portrait panel turned 90 degrees for landscape, the extent reported in
  // the window's orientation (1920 x 1080) with the 90 degree transform (seen on the device).
  p = planSurface(caps(1920, 1080, kSurfaceRotate90), 1920, 1080);
  CHECK_EQ(p.width, 1920u);
  CHECK_EQ(p.height, 1080u);
  CHECK_EQ(p.transform, kSurfaceIdentity);
  // Another driver reports the panel's 1080 x 1920 for the same window: the same plan.
  p = planSurface(caps(1080, 1920, kSurfaceRotate90), 1920, 1080);
  CHECK_EQ(p.width, 1920u);
  CHECK_EQ(p.height, 1080u);
  CHECK_EQ(p.transform, kSurfaceIdentity);  // the compositor rotates, not the frames
  CHECK_EQ(p.rotation, 90);
  // Turned the other way (270), and a landscape panel as it is (identity).
  p = planSurface(caps(1080, 1920, kSurfaceRotate270), 1920, 1080);
  CHECK_EQ(p.width, 1920u);
  CHECK_EQ(p.height, 1080u);
  p = planSurface(caps(2400, 1080, kSurfaceIdentity), 2400, 1080);
  CHECK_EQ(p.width, 2400u);
  CHECK_EQ(p.height, 1080u);
  // Upside down (180): no swap.
  p = planSurface(caps(1080, 2400, kSurfaceRotate180), 1080, 2400);
  CHECK_EQ(p.width, 1080u);
  CHECK_EQ(p.height, 2400u);
  CHECK_EQ(p.transform, kSurfaceIdentity);
  // A window smaller than the display (bars, a cutout): the window's size.
  p = planSurface(caps(1920, 1080, kSurfaceRotate90), 1920, 972);
  CHECK_EQ(p.height, 972u);
  // A surface without identity: its own transform and extent (pre-rotated frames are its business).
  p = planSurface(caps(1080, 1920, kSurfaceRotate90, kSurfaceRotate90), 1920, 1080);
  CHECK_EQ(p.transform, kSurfaceRotate90);
  CHECK_EQ(p.width, 1080u);
  CHECK_EQ(p.height, 1920u);
  // Minimised.
  CHECK_EQ(planSurface(caps(0, 0, kSurfaceIdentity), 0, 0).width, 0u);
  CHECK_EQ(planSurface(caps(kSurfaceUndefinedExtent, kSurfaceUndefinedExtent, kSurfaceIdentity), 0, 0).width, 0u);
}

TEST_CASE("backdrop setting: the older levels read as none or the default, the newer key wins, saved as backdrop2") {
  CHECK_EQ(Settings::parse("").backdrop, 2);
  CHECK_EQ(Settings::parse("backdrop=0\n").backdrop, 0);
  for (const char* old : {"backdrop=1\n", "backdrop=2\n", "backdrop=3\n"}) CHECK_EQ(Settings::parse(old).backdrop, 2);
  CHECK_EQ(Settings::parse("backdrop2=1\n").backdrop, 1);
  CHECK_EQ(Settings::parse("backdrop2=4\nbackdrop=0\n").backdrop, 4);
  CHECK_EQ(Settings::parse("backdrop=0\nbackdrop2=3\n").backdrop, 3);
  CHECK_EQ(Settings::parse("backdrop2=9\n").backdrop, 4);
  Settings s;
  s.backdrop = 1;
  const std::string text = s.serialize();
  CHECK(text.find("backdrop2=1\n") != std::string::npos);
  CHECK(text.find("\nbackdrop=") == std::string::npos);
}
