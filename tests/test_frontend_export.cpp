// Frontend core: export settings + geometry, streaming output canvas, flash level texts and the
// shared localization table (generated from Localizable.xcstrings) with its formatter.
// Ported from the macOS ExportTests (geometry) / StreamOutputTests / UILanguageTests.
#include <algorithm>
#include <cmath>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {
rnf_export_settings defaults() {
  rnf_export_settings s{};
  s.preset = {RNF_PRESET_CANVAS, 0, 1280, 960};
  s.crop_top = 8;
  s.crop_bottom = 8;
  return s;
}
rnf_export_geometry geometry(const rnf_export_settings& s) {
  rnf_export_geometry g{};
  rnf_export_geometry_compute(&s, &g);
  return g;
}
}  // namespace

TEST_CASE("export geometry") {
  rnf_export_settings s = defaults();
  s.crop_top = 0;
  s.crop_bottom = 0;
  rnf_export_geometry g = geometry(s);
  CHECK(g.canvas_width == 1280 && g.canvas_height == 960 && g.dst_width == 1024 && g.dst_height == 960 && g.dst_x == 128 &&
        g.dst_y == 0);
  std::vector<int> cols(size_t(g.dst_width));
  rnf_export_column_map(&g, cols.data());
  CHECK((std::vector<int>(cols.begin(), cols.begin() + 8) == std::vector<int>{0, 0, 0, 0, 1, 1, 1, 1}));
  s.crop_top = 8;
  s.crop_bottom = 8;
  s.pixel_aspect_87 = 1;
  g = geometry(s);
  CHECK_EQ(g.src_height, 224);
  CHECK_EQ(g.vertical_scale, 4);
  CHECK_EQ(g.dst_height, 896);
  CHECK_EQ(g.dst_width, 1170);
  std::vector<int> rows(size_t(g.dst_height));
  rnf_export_row_map(&g, rows.data());
  CHECK(rows[0] == 8 && rows[4] == 9 && rows.back() == 231);
  s.preset = {RNF_PRESET_NATIVE, 3, 0, 0};
  s.pixel_aspect_87 = 0;
  g = geometry(s);
  CHECK(g.canvas_width == 768 && g.canvas_height == 672);
  s.preset = {RNF_PRESET_CANVAS, 0, 1920, 1080};
  s.crop_top = 0;
  s.crop_bottom = 0;
  g = geometry(s);
  CHECK_EQ(g.vertical_scale, 4);
  CHECK_EQ(g.dst_height, 960);
  const int S = RNF_QUALITY_STANDARD;
  CHECK_EQ(rnf_export_video_bitrate(&g, 0, S), int64_t(12000000));  // 1920x1080: YouTube 1080p60
  CHECK_EQ(rnf_export_video_bitrate(&g, 1, S), int64_t(8400000));    // HEVC x0.7
  CHECK_EQ(rnf_export_video_bitrate(&g, 0, RNF_QUALITY_LIGHT), int64_t(6000000));
  CHECK_EQ(rnf_export_video_bitrate(&g, 0, RNF_QUALITY_HIGH), int64_t(24000000));
  rnf_export_geometry c{};
  auto at = [&](int w, int h, int hevc) {
    c.canvas_width = w;
    c.canvas_height = h;
    return rnf_export_video_bitrate(&c, hevc, S);
  };
  CHECK_EQ(at(1920, 1440, 0), int64_t(24000000));
  CHECK_EQ(at(1280, 720, 0), int64_t(7500000));
  CHECK_EQ(at(3840, 2160, 0), int64_t(60000000));
  CHECK_EQ(at(640, 480, 0), int64_t(4000000));
  const int64_t b960 = at(1280, 960, 0);  // between 720p and 1080p
  CHECK(b960 > 7500000 && b960 < 12000000);
  CHECK_EQ(at(256, 240, 0), int64_t(1500000));  // floor
  CHECK_EQ(at(256, 240, 1), int64_t(1050000));
  CHECK_EQ(rnf_export_video_bitrate(nullptr, 0, S), int64_t(1500000));
  CHECK_EQ(rnf_export_audio_bitrate(), int64_t(128000));
  // 60 s of 1080p: (12 + 0.128) Mbit/s x 60 s / 8 = 90.96 MB + a container of well under 1 MB.
  rnf_export_prediction pr{};
  const int64_t total = rnf_export_predict_size(&g, 0, S, 3606, &pr);  // 3606 frames ~= 60.0 s
  CHECK_EQ(total, pr.total_bytes);
  CHECK(std::fabs(pr.seconds - 60.0) < 0.01);
  CHECK(pr.container_bytes > 15000 && pr.container_bytes < 100000);
  CHECK(total > 90900000 && total < 91200000);
  CHECK(rnf_export_predict_size(&g, 0, RNF_QUALITY_HIGH, 3606, nullptr) > 2 * pr.video_bytes);
  CHECK(rnf_export_predict_size(&g, 0, S, 0, &pr) < 3000);  // no frames: just the fixed boxes
}

TEST_CASE("export validation and presets") {
  rnf_export_settings s = defaults();
  CHECK_EQ(rnf_export_validate(&s, nullptr), RN_OK);
  s.crop_top = 120;
  s.crop_bottom = 120;
  char* msg = nullptr;
  CHECK_EQ(rnf_export_validate(&s, &msg), RN_ERR_INVALID_ARG);
  CHECK_EQ(take(msg), "The overscan crop is too large");
  s = defaults();
  s.start_frame = 10;
  s.end_frame = 10;
  CHECK_EQ(rnf_export_validate(&s, &msg), RN_ERR_INVALID_ARG);
  CHECK_EQ(take(msg), "The export range is empty");
  REQUIRE_EQ(rnf_export_preset_count(), size_t(7));
  rnf_export_preset p;
  rnf_export_preset_get(1, &p);
  CHECK_EQ(take(rnf_export_preset_label(&p)), "Native ×2");
  rnf_export_preset_get(6, &p);
  CHECK_EQ(take(rnf_export_preset_label(&p)), "1920×1080");
}

TEST_CASE("stream output: integer multiples fill the canvas") {
  auto check = [](rnf_stream_size s, int par, int cw, int ch, double x, double y, double w, double h) {
    rnf_stream_layout l = rnf_stream_output_layout(s, par);
    CHECK(l.canvas_width == cw && l.canvas_height == ch && l.x == x && l.y == y && l.width == w && l.height == h);
  };
  check(RNF_STREAM_X1, 0, 256, 240, 0, 0, 256, 240);
  check(RNF_STREAM_X2, 0, 512, 480, 0, 0, 512, 480);
  check(RNF_STREAM_X3, 0, 768, 720, 0, 0, 768, 720);
  check(RNF_STREAM_X4, 0, 1024, 960, 0, 0, 1024, 960);
  check(RNF_STREAM_X1, 1, 293, 240, 0, 0, 293, 240);
  check(RNF_STREAM_X4, 1, 1170, 960, 0, 0, 1170, 960);
  check(RNF_STREAM_W1280, 0, 1280, 960, 128, 0, 1024, 960);
  check(RNF_STREAM_W1280, 1, 1280, 960, 55, 0, 1170, 960);
  check(RNF_STREAM_W1920, 0, 1920, 1440, 192, 0, 1536, 1440);
  check(RNF_STREAM_W1920, 1, 1920, 1440, 82, 0, 1755, 1440);
  CHECK_EQ(RNF_STREAM_DEFAULT, RNF_STREAM_X4);
  CHECK_EQ(take(rnf_stream_output_label(RNF_STREAM_X1, 0)), "Native 256×240");
  CHECK_EQ(take(rnf_stream_output_label(RNF_STREAM_X4, 0)), "4× 1024×960");
  CHECK_EQ(take(rnf_stream_output_label(RNF_STREAM_W1280, 1)), "1280×960 (4:3, with black bars)");
}

TEST_CASE("export and stream media geometry timing and source rotation") {
  rn_media_info media{320, 224, 5963, 100, 2, 4, 3, 1};
  rnf_export_settings s{};
  s.preset = {RNF_PRESET_NATIVE, 2, 0, 0};
  rnf_export_geometry g{};
  REQUIRE_EQ(rnf_export_validate_media(&s, &media, 0, 0, nullptr), RN_OK);
  rnf_export_geometry_compute_media(&s, &media, 0, 0, &g);
  CHECK_EQ(g.canvas_width, 448);
  CHECK_EQ(g.canvas_height, 640);
  CHECK_EQ(g.rotation, uint32_t(1));
  CHECK_EQ(g.src_width, 320);
  CHECK_EQ(g.src_height, 224);
  int x = -1, y = -1;
  REQUIRE(rnf_export_source_point(&g, 0, 0, &x, &y));
  CHECK_EQ(x, 0);
  CHECK_EQ(y, 223);
  REQUIRE(rnf_export_source_point(&g, 447, 639, &x, &y));
  CHECK_EQ(x, 319);
  CHECK_EQ(y, 0);
  CHECK(!rnf_export_source_point(&g, 448, 0, &x, &y));
  s.pixel_aspect_87 = 1;
  rnf_export_geometry_compute_media(&s, &media, 0, 0, &g);
  CHECK_EQ(g.canvas_width, 448);
  CHECK_EQ(g.canvas_height, 854);
  CHECK_EQ(g.dst_height, 853);
  s.crop_left = 16;
  s.crop_top = 8;
  rnf_export_geometry_compute_media(&s, &media, 304, 208, &g);
  CHECK_EQ(g.src_width, 288);
  CHECK_EQ(g.src_height, 200);
  REQUIRE(rnf_export_source_point(&g, 0, 0, &x, &y));
  CHECK_EQ(x, 16);
  CHECK_EQ(y, 207);
  s.crop_bottom = 193;
  CHECK_EQ(rnf_export_validate_media(&s, &media, 304, 208, nullptr), RN_ERR_INVALID_ARG);
  s.crop_bottom = 0;
  CHECK_EQ(rnf_export_validate_media(&s, &media, 321, 224, nullptr), RN_ERR_INVALID_ARG);
  s.crop_top = s.crop_left = 0;
  s.pixel_aspect_87 = 0;
  for (uint32_t rotation = 0; rotation < 4; ++rotation) {
    media.rotation = rotation;
    rnf_export_geometry_compute_media(&s, &media, 0, 0, &g);
    REQUIRE(rnf_export_source_point(&g, 0, 0, &x, &y));
    CHECK_EQ(x, rotation >= 2 ? 319 : 0);
    CHECK_EQ(y, rotation == 1 || rotation == 2 ? 223 : 0);
  }
  media.rotation = 1;
  auto l = rnf_stream_output_layout_media(RNF_STREAM_X2, 0, &media, 0, 0);
  CHECK_EQ(l.canvas_width, 448);
  CHECK_EQ(l.canvas_height, 640);
  CHECK_EQ(l.width, 448.0);
  CHECK_EQ(l.height, 640.0);
  l = rnf_stream_output_layout_media(RNF_STREAM_X2, 1, &media, 0, 0);
  CHECK_EQ(l.canvas_width, 448);
  CHECK_EQ(l.canvas_height, 853);
  l = rnf_stream_output_layout_media(RNF_STREAM_W1280, 0, &media, 0, 0);
  CHECK_EQ(l.width, 672.0);
  CHECK_EQ(l.height, 960.0);
  CHECK_EQ(l.x, 304.0);
  CHECK_EQ(take(rnf_stream_output_label_media(RNF_STREAM_X2, 0, &media, 0, 0)), "2× 448×640");

  const double audioRate = 800.0 * 5963 / 100;
  rnf_export_timing timing{};
  REQUIRE_EQ(rnf_export_timing_compute(&media, audioRate, &timing), RN_OK);
  CHECK_EQ(timing.timescale, uint64_t(5963));
  CHECK_EQ(timing.frame_duration, uint64_t(100));
  CHECK_EQ(timing.audio_channels, uint32_t(2));
  CHECK_EQ(timing.audio_sample_rate, audioRate);
  CHECK_EQ(timing.gop_frames, uint64_t(30));
  rnf_export_prediction p{};
  rnf_export_predict_size_media(&g, 0, RNF_QUALITY_STANDARD, 5963, &media, audioRate, &p);
  CHECK_EQ(p.seconds, 100.0);
  CHECK_EQ(p.audio_bitrate, int64_t(256000));
  CHECK_EQ(p.audio_bytes, int64_t(3200000));
  rnf_export_prediction fasterAudio{};
  rnf_export_predict_size_media(&g, 0, RNF_QUALITY_STANDARD, 5963, &media, audioRate * 2, &fasterAudio);
  CHECK(fasterAudio.container_bytes > p.container_bytes);
  CHECK_EQ(rnf_export_timing_compute(&media, 0, &timing), RN_ERR_INVALID_ARG);
  media.fps_den = 0;
  CHECK_EQ(rnf_export_timing_compute(&media, audioRate, &timing), RN_ERR_INVALID_ARG);
}

TEST_CASE("explicit NES media keeps legacy export results") {
  const rn_media_info media{RN_VIDEO_WIDTH, RN_VIDEO_HEIGHT, RN_FPS_NUM, RN_FPS_DEN, 1, 8, 7, 0};
  for (int par = 0; par < 2; ++par) {
    for (size_t preset = 0; preset < rnf_export_preset_count(); ++preset) {
      auto s = defaults();
      rnf_export_preset_get(preset, &s.preset);
      s.pixel_aspect_87 = par;
      auto legacy = geometry(s);
      rnf_export_geometry generic{};
      rnf_export_geometry_compute_media(&s, &media, 0, 0, &generic);
      CHECK_EQ(generic.canvas_width, legacy.canvas_width);
      CHECK_EQ(generic.canvas_height, legacy.canvas_height);
      CHECK_EQ(generic.dst_width, legacy.dst_width);
      CHECK_EQ(generic.dst_height, legacy.dst_height);
      CHECK_EQ(generic.dst_x, legacy.dst_x);
      CHECK_EQ(generic.dst_y, legacy.dst_y);
      // Independent frozen NES formula, rather than comparing two wrappers of one implementation.
      const int sw = 256, sh = 224;
      const double aspect = par ? 8.0 / 7.0 : 1.0;
      auto even = [](int v) { return v + (v & 1); };
      auto width = [&](int scale) { return int(std::round(double(sw * scale) * aspect)); };
      int k, dw, dh, cw, ch;
      if (s.preset.kind == RNF_PRESET_NATIVE) {
        k = std::max(1, s.preset.scale);
        dw = even(width(k)); dh = sh * k; cw = dw; ch = even(dh);
      } else {
        k = std::max(1, s.preset.height / sh);
        while (k > 1 && width(k) > s.preset.width) --k;
        dw = std::min(s.preset.width, width(k)); dh = std::min(s.preset.height, sh * k);
        cw = even(s.preset.width); ch = even(s.preset.height);
      }
      CHECK_EQ(generic.dst_width, dw);
      CHECK_EQ(generic.dst_height, dh);
      CHECK_EQ(generic.canvas_width, cw);
      CHECK_EQ(generic.canvas_height, ch);
      CHECK_EQ(generic.dst_x, (cw - dw) / 2);
      CHECK_EQ(generic.dst_y, (ch - dh) / 2);
      rnf_export_prediction a{}, b{};
      rnf_export_predict_size(&legacy, 0, RNF_QUALITY_STANDARD, 3606, &a);
      rnf_export_predict_size_media(&generic, 0, RNF_QUALITY_STANDARD, 3606, &media, RN_SAMPLE_RATE, &b);
      CHECK_EQ(a.total_bytes, b.total_bytes);
      CHECK_EQ(a.seconds, b.seconds);
    }
  }
}

TEST_CASE("flash level texts") {
  CHECK_EQ(take(rnf_flash_level_label(RN_FLASH_STANDARD)), "Standard");
  CHECK(take(rnf_flash_level_detail(RN_FLASH_LOW)).find("25%") != std::string::npos);
}

TEST_CASE("UI language rule") {
  const char* ja[] = {"ja-JP", "en-US"};
  const char* jaOnly[] = {"ja"};
  const char* fr[] = {"fr-FR", "ja-JP", "en"};
  const char* gb[] = {"en-GB"};
  const char* zh[] = {"zh-Hans-CN"};
  CHECK_EQ(std::string(rnf_ui_language_choose(ja, 2)), "ja");
  CHECK_EQ(std::string(rnf_ui_language_choose(jaOnly, 1)), "ja");
  CHECK_EQ(std::string(rnf_ui_language_choose(fr, 3)), "en");
  CHECK_EQ(std::string(rnf_ui_language_choose(gb, 1)), "en");
  CHECK_EQ(std::string(rnf_ui_language_choose(zh, 1)), "en");
  CHECK_EQ(std::string(rnf_ui_language_choose(nullptr, 0)), "en");
}

TEST_CASE("localization table and formatting") {
  CHECK(rnf_l10n_count() > 500);
  const char *key, *en, *ja, *prev = "";
  for (size_t i = 0; i < rnf_l10n_count(); ++i) {
    REQUIRE(rnf_l10n_entry(i, &key, &en, &ja));
    CHECK(std::strcmp(prev, key) < 0);  // sorted for binary search
    prev = key;
  }
  CHECK_EQ(std::string(rnf_l10n_language()), "en");
  CHECK_EQ(std::string(rnf_l10n_lookup_in("ja", "Move")), "移動");
  CHECK_EQ(std::string(rnf_l10n_lookup_in("en", "Move")), "Move");
  CHECK_EQ(std::string(rnf_l10n_lookup("not a key")), "not a key");
  const char* productTitle = rnf_l10n_lookup_in("en", "ReplayNES Version");
  CHECK_EQ(std::string(productTitle), std::string(RN_PRODUCT_NAME) + " Version");
  CHECK_EQ(std::string(rnf_l10n_lookup_in("ja", "ReplayNES Version")),
           std::string(RN_PRODUCT_NAME) + "のバージョン");
  CHECK_EQ(std::string(rnf_l10n_lookup("ReplayNES / ReplayNES")),
           std::string(RN_PRODUCT_NAME) + " / " + RN_PRODUCT_NAME);
  // Another lookup must not invalidate a previously returned string.
  CHECK_EQ(std::string(productTitle), std::string(RN_PRODUCT_NAME) + " Version");
  rnf_arg args[3];
  args[0] = rnf_arg{RNF_ARG_INT, 2, 0, 0, nullptr};
  args[1] = rnf_arg{RNF_ARG_STRING, 0, 0, 0, "D-pad ↑"};
  CHECK_EQ(take(rnf_l10n_format("Pad %lld %@", args, 2)), "Pad 2 D-pad ↑");
  rnf_l10n_set_language("ja");
  CHECK_EQ(std::string(rnf_l10n_language()), "ja");
  CHECK_EQ(take(rnf_l10n_format("Pad %lld %@", args, 2)), "パッド2 D-pad ↑");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "gc0:face.east", "A")), "パッド1 右ボタン(A)");
  CHECK_EQ(take(rnf_input_element_title("face.east", RNF_FAMILY_NINTENDO, nullptr)), "右ボタン");
  rnf_l10n_set_language("en");
  // Positional arguments and printf-style conversions.
  args[0] = rnf_arg{RNF_ARG_STRING, 0, 0, 0, "x"};
  args[1] = rnf_arg{RNF_ARG_UINT, 0, 255, 0, nullptr};
  args[2] = rnf_arg{RNF_ARG_DOUBLE, 0, 0, 2.25, nullptr};
  CHECK_EQ(take(rnf_format("%2$llu %1$@ %3$.1f %%", args, 3)), "255 x 2.2 %");
  CHECK_EQ(take(rnf_format("#%016llx", args + 1, 1)), "#00000000000000ff");
  CHECK_EQ(take(rnf_format("trailing %", args, 0)), "trailing %");
}
