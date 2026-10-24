// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>
#include <vector>

#include "export/export_frame.h"
#include "support/rn_test.h"

TEST_CASE("desktop export preserves NES pixels with the media scaler") {
  std::vector<uint32_t> pixels(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint32_t(i * 173) & 0xFFFFFF;
  rn_video_frame_info frame{pixels.data(), RN_VIDEO_WIDTH, RN_VIDEO_HEIGHT, RN_VIDEO_WIDTH};
  rnf_export_settings settings{};
  settings.preset.kind = RNF_PRESET_CANVAS;
  settings.preset.width = 640;
  settings.preset.height = 480;
  settings.crop_top = settings.crop_bottom = 8;
  for (int aspect : {0, 1}) {
    settings.pixel_aspect_87 = aspect;
    rnf_export_geometry g{};
    rnf_export_geometry_compute(&settings, &g);
    std::vector<int> cols(size_t(g.dst_width)), rows(size_t(g.dst_height));
    rnf_export_column_map(&g, cols.data());
    rnf_export_row_map(&g, rows.data());
    std::vector<uint8_t> oldPixels(size_t(g.canvas_width) * g.canvas_height * 4), mediaPixels(oldPixels.size());
    rnl::exportframe::scaleNearest(pixels.data(), oldPixels.data(), size_t(g.canvas_width) * 4, g, cols, rows);
    rnl::exportframe::scaleNearestFrame(frame, mediaPixels.data(), size_t(g.canvas_width) * 4, g);
    CHECK(oldPixels == mediaPixels);
  }
}

TEST_CASE("desktop export rotates strided non-NES pixels in all four directions") {
  rn_media_info media{320, 224, 5963, 100, 2, 1, 1, 0};
  const uint32_t stride = 336;
  std::vector<uint32_t> source(size_t(stride) * media.max_height, 0xDEADBEEF);
  for (uint32_t y = 0; y < media.max_height; ++y)
    for (uint32_t x = 0; x < media.max_width; ++x) source[size_t(y) * stride + x] = 0xFF000000 | (y << 16) | x;
  rn_video_frame_info frame{source.data(), 320, 224, stride};
  for (uint32_t rotation = 0; rotation < 4; ++rotation) {
    media.rotation = rotation;
    rnf_export_settings settings{};
    settings.preset.kind = RNF_PRESET_CANVAS;
    settings.preset.width = rotation % 2 ? 224 : 320;
    settings.preset.height = rotation % 2 ? 320 : 224;
    rnf_export_geometry g{};
    rnf_export_geometry_compute_media(&settings, &media, frame.width, frame.height, &g);
    CHECK(g.dst_width == settings.preset.width);
    CHECK(g.dst_height == settings.preset.height);
    std::vector<uint32_t> output(size_t(g.canvas_width) * g.canvas_height);
    rnl::exportframe::scaleNearestFrame(frame, reinterpret_cast<uint8_t*>(output.data()), size_t(g.canvas_width) * 4, g);
    for (int y = 0; y < g.canvas_height; ++y)
      for (int x = 0; x < g.canvas_width; ++x) {
        const int sx = rotation == 0 ? x : rotation == 1 ? y : rotation == 2 ? 319 - x : 319 - y;
        const int sy = rotation == 0 ? y : rotation == 1 ? 223 - x : rotation == 2 ? 223 - y : x;
        CHECK(output[size_t(y) * g.canvas_width + x] == source[size_t(sy) * stride + sx]);
      }
  }
}
