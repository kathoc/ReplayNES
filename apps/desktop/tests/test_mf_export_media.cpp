// Media Foundation's CPU preparation also runs on Linux, without COM or a GPU.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <vector>
#include "../../windows/src/mf_export_media.h"
#include "export/export_frame.h"
#include "support/rn_test.h"

TEST_CASE("MF media timestamps preserve NES rounding and arbitrary rational frame rates") {
  for (uint64_t n = 0; n < 20000; ++n) {
    CHECK_EQ(rnl::mfmedia::time100ns(n, RN_FPS_DEN, RN_FPS_NUM), rnl::exportframe::frameTime100ns(n));
    CHECK_EQ(rnl::mfmedia::time100ns(n, 1, RN_SAMPLE_RATE), rnl::exportframe::sampleTime100ns(n));
  }
  CHECK_EQ(rnl::mfmedia::time100ns(5963, 100, 5963), int64_t(1000000000));
  CHECK_EQ(rnl::mfmedia::time100ns(30000, 1001, 30000), int64_t(10010000000));
  CHECK_EQ(rnl::mfmedia::time100ns(192000, 1, 192000), int64_t(10000000));
  CHECK_EQ(rnl::mfmedia::time100ns(UINT64_MAX, 2, 1), int64_t(-1));
  CHECK_EQ(rnl::mfmedia::time100ns(1, 1, 0), int64_t(-1));
}

TEST_CASE("MF NES audio is byte exact across nonzero-start frame boundaries") {
  rnl::mfmedia::Audio converter;
  CHECK(converter.init(RN_SAMPLE_RATE, 1));
  CHECK_EQ(converter.rate(), uint32_t(RN_SAMPLE_RATE));
  std::vector<int16_t> result;
  const uint64_t start = 313;
  const uint64_t base = rn_audio_samples_before(start);
  for (uint64_t f = start; f < start + 300; ++f) {
    const uint64_t at = rn_audio_samples_before(f) - base;
    const size_t count = size_t(rn_audio_samples_before(f + 1) - rn_audio_samples_before(f));
    std::vector<int16_t> input(count);
    for (size_t i = 0; i < count; ++i) input[i] = int16_t((at + i) * 173);
    CHECK_EQ(converter.frames(), at);
    CHECK(converter.process(input.data(), count, result));
    CHECK(result == input);
    CHECK_EQ(converter.frames(), at + count);
  }
  converter.finish(result);
  CHECK(result.empty());
}

TEST_CASE("MF audio supports stereo and arbitrary integral or fractional rates") {
  for (double rate : {8000.0, 32000.0, 44100.0, 47999.23, 48000.0, 96000.0, 192000.0}) {
    rnl::mfmedia::Audio converter;
    CHECK(converter.init(rate, 2));
    std::vector<int16_t> output, joined;
    size_t total = 0;
    for (size_t count : {size_t(1), size_t(37), size_t(797), size_t(2001)}) {
      std::vector<int16_t> input(count * 2);
      for (size_t i = 0; i < count; ++i) { input[i * 2] = 10000; input[i * 2 + 1] = -20000; }
      CHECK(converter.process(input.data(), count, output));
      joined.insert(joined.end(), output.begin(), output.end());
      total += count;
    }
    converter.finish(output);
    joined.insert(joined.end(), output.begin(), output.end());
    const uint64_t expected = uint64_t(std::llround(double(total) * converter.rate() / rate));
    CHECK_EQ(converter.frames(), expected);
    CHECK_EQ(joined.size(), size_t(expected) * 2);
    // Each channel retains its own value (only the delayed tail may be silence).
    for (size_t i = 0; i < joined.size(); i += 2) {
      CHECK(joined[i] == 10000 || joined[i] == 0);
      CHECK(joined[i + 1] == -20000 || joined[i + 1] == 0);
    }
    CHECK_EQ(joined.front(), int16_t(10000));
    CHECK_EQ(joined[1], int16_t(-20000));
  }
}

TEST_CASE("MF input NV12 preserves NES pixels for media geometry") {
  const rn_media_info media{256, 240, RN_FPS_NUM, RN_FPS_DEN, 1, 8, 7, 0};
  std::vector<uint32_t> source(256 * 240);
  for (size_t i = 0; i < source.size(); ++i) source[i] = uint32_t(i * 9173) & 0xFFFFFF;
  const rn_video_frame_info frame{source.data(), 256, 240, 256};
  for (int par : {0, 1}) for (int crop : {0, 8}) {
    rnf_export_settings settings{};
    settings.preset = {RNF_PRESET_CANVAS, 0, 640, 480};
    settings.pixel_aspect_87 = par;
    settings.crop_top = settings.crop_bottom = crop;
    rnf_export_geometry oldGeometry{}, mediaGeometry{};
    rnf_export_geometry_compute(&settings, &oldGeometry);
    rnf_export_geometry_compute_media(&settings, &media, 256, 240, &mediaGeometry);
    std::vector<int> columns(size_t(oldGeometry.dst_width)), rows(size_t(oldGeometry.dst_height));
    rnf_export_column_map(&oldGeometry, columns.data());
    rnf_export_row_map(&oldGeometry, rows.data());
    std::vector<uint8_t> oldCanvas(640 * 480 * 4), newCanvas(oldCanvas.size());
    rnl::exportframe::scaleNearest(source.data(), oldCanvas.data(), 640 * 4, oldGeometry, columns, rows);
    rnl::exportframe::scaleNearestFrame(frame, newCanvas.data(), 640 * 4, mediaGeometry);
    CHECK(oldCanvas == newCanvas);
    std::vector<uint8_t> oldNv12(640 * 480 * 3 / 2), newNv12(oldNv12.size());
    rnl::exportframe::convertToYuv(oldCanvas.data(), 640 * 4, 640, 480, oldNv12.data(), 640,
                                  oldNv12.data() + 640 * 480, 640, nullptr, 0);
    rnl::exportframe::convertToYuv(newCanvas.data(), 640 * 4, 640, 480, newNv12.data(), 640,
                                  newNv12.data() + 640 * 480, 640, nullptr, 0);
    CHECK(oldNv12 == newNv12);
  }
}

TEST_CASE("MF changed frame dimensions retain the initial canvas with rotation PAR and stride") {
  const rn_media_info media{64, 48, 5963, 100, 2, 2, 1, 1};
  rnf_export_settings settings{};
  settings.preset = {RNF_PRESET_NATIVE, 1, 0, 0};
  settings.pixel_aspect_87 = 1;
  rnf_export_geometry geometry{};
  rnf_export_geometry_compute_media(&settings, &media, 64, 48, &geometry);
  CHECK_EQ(geometry.canvas_width, 48);
  CHECK_EQ(geometry.canvas_height, 128);
  const int width = geometry.canvas_width, height = geometry.canvas_height;
  CHECK(rnl::mfmedia::resizeGeometry(settings, media, 32, 16, width, height, &geometry));
  CHECK_EQ(geometry.canvas_width, width);
  CHECK_EQ(geometry.canvas_height, height);
  CHECK_EQ(geometry.dst_width, 32);
  CHECK_EQ(geometry.dst_height, 128);
  std::vector<uint32_t> pixels(37 * 16, 0xDEADBEEF);
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 32; ++x) pixels[y * 37 + x] = uint32_t(y * 32 + x + 1);
  const rn_video_frame_info frame{pixels.data(), 32, 16, 37};
  std::vector<uint32_t> canvas(size_t(width) * height);
  rnl::exportframe::scaleNearestFrame(frame, reinterpret_cast<uint8_t*>(canvas.data()), size_t(width) * 4, geometry);
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const uint32_t expected = x < 8 || x >= 40 ? 0 : uint32_t((15 - (x - 8) / 2) * 32 + y / 4 + 1);
    CHECK_EQ(canvas[size_t(y) * width + x], 0xFF000000u | expected);
  }
  settings.crop_left = 32;
  CHECK(!rnl::mfmedia::resizeGeometry(settings, media, 32, 16, width, height, &geometry));
}
