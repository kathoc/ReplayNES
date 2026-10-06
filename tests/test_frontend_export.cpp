// Frontend core: export settings + geometry, streaming output canvas, flash level texts and the
// shared localization table (generated from Localizable.xcstrings) with its formatter.
// Ported from the macOS ExportTests (geometry) / StreamOutputTests / UILanguageTests.
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
  CHECK_EQ(rnf_export_video_bitrate(&g, 0.25), int64_t(1920 * 1080 * 60 * 0.25));
  rnf_export_geometry tiny{};
  tiny.canvas_width = 16;
  tiny.canvas_height = 16;
  CHECK_EQ(rnf_export_video_bitrate(&tiny, 0.25), int64_t(2000000));
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
  rnf_arg args[3];
  args[0] = rnf_arg{RNF_ARG_INT, 2, 0, 0, nullptr};
  args[1] = rnf_arg{RNF_ARG_STRING, 0, 0, 0, "D-pad ↑"};
  CHECK_EQ(take(rnf_l10n_format("Pad %lld %@", args, 2)), "Pad 2 D-pad ↑");
  rnf_l10n_set_language("ja");
  CHECK_EQ(std::string(rnf_l10n_language()), "ja");
  CHECK_EQ(take(rnf_l10n_format("Pad %lld %@", args, 2)), "パッド2 D-pad ↑");
  CHECK_EQ(take(rnf_input_display_name(RNF_KEYBOARD_MACOS, "gc0:face.east", "A")), "パッド1 右ボタン(A)");
  CHECK_EQ(take(rnf_input_element_title("face.east", RNF_FAMILY_NINTENDO, nullptr)), "右ボタン（A）");
  rnf_l10n_set_language("en");
  // Positional arguments and printf-style conversions.
  args[0] = rnf_arg{RNF_ARG_STRING, 0, 0, 0, "x"};
  args[1] = rnf_arg{RNF_ARG_UINT, 0, 255, 0, nullptr};
  args[2] = rnf_arg{RNF_ARG_DOUBLE, 0, 0, 2.25, nullptr};
  CHECK_EQ(take(rnf_format("%2$llu %1$@ %3$.1f %%", args, 3)), "255 x 2.2 %");
  CHECK_EQ(take(rnf_format("#%016llx", args + 1, 1)), "#00000000000000ff");
  CHECK_EQ(take(rnf_format("trailing %", args, 0)), "trailing %");
}
