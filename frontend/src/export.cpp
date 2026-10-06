// Export settings validation + geometry (nearest-neighbour mapping of the cropped 256x240 source
// into the output canvas), the streaming output canvas (Syphon / PipeWire / NDI-style outputs) and
// the flash reduction level texts. Encoders stay in the frontends (AVFoundation, FFmpeg).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>

#include "common.hpp"

using namespace rnf;

namespace {

const rnf_export_preset kPresets[] = {
    {RNF_PRESET_NATIVE, 1, 0, 0}, {RNF_PRESET_NATIVE, 2, 0, 0}, {RNF_PRESET_NATIVE, 3, 0, 0},
    {RNF_PRESET_NATIVE, 4, 0, 0}, {RNF_PRESET_CANVAS, 0, 1280, 960}, {RNF_PRESET_CANVAS, 0, 1920, 1440},
    {RNF_PRESET_CANVAS, 0, 1920, 1080},
};

int streamMultiple(rnf_stream_size s) {
  switch (s) {
    case RNF_STREAM_X1: return 1;
    case RNF_STREAM_X2: return 2;
    case RNF_STREAM_X3: return 3;
    case RNF_STREAM_X4: return 4;
    default: return 0;
  }
}

}  // namespace

extern "C" {

size_t rnf_export_preset_count(void) { return sizeof(kPresets) / sizeof(kPresets[0]); }
int rnf_export_preset_get(size_t i, rnf_export_preset* out) {
  if (i >= rnf_export_preset_count() || !out) return 0;
  *out = kPresets[i];
  return 1;
}
char* rnf_export_preset_label(const rnf_export_preset* p) {
  if (!p) return dup("");
  RNF_GUARD_BEGIN
  if (p->kind == RNF_PRESET_NATIVE) return dup(trf(RNF_L("Native ×%lld"), {argI(p->scale)}));
  return dup(std::to_string(p->width) + "×" + std::to_string(p->height));
  RNF_GUARD_END(nullptr)
}

rn_status rnf_export_validate(const rnf_export_settings* s, char** message) {
  if (message) *message = nullptr;
  if (!s) return fail(RN_ERR_INVALID_ARG, "null settings");
  bool ok = s->crop_top >= 0 && s->crop_bottom >= 0 && s->crop_left >= 0 && s->crop_right >= 0 &&
            s->crop_top + s->crop_bottom <= RN_VIDEO_HEIGHT - 16 && s->crop_left + s->crop_right <= RN_VIDEO_WIDTH - 16;
  if (!ok) {
    const char* m = tr(RNF_L("The overscan crop is too large"));
    if (message) *message = dup(m);
    return fail(RN_ERR_INVALID_ARG, m);
  }
  if (s->end_frame != 0 && s->end_frame <= s->start_frame) {
    const char* m = tr(RNF_L("The export range is empty"));
    if (message) *message = dup(m);
    return fail(RN_ERR_INVALID_ARG, m);
  }
  return RN_OK;
}

void rnf_export_geometry_compute(const rnf_export_settings* s, rnf_export_geometry* out) {
  if (!s || !out) return;
  const int sw = RN_VIDEO_WIDTH - s->crop_left - s->crop_right;
  const int sh = RN_VIDEO_HEIGHT - s->crop_top - s->crop_bottom;
  const double par = s->pixel_aspect_87 ? 8.0 / 7.0 : 1.0;
  auto even = [](int v) { return v + (v & 1); };
  auto width = [&](int scale) { return int(std::round(double(sw * scale) * par)); };
  int k, dw, dh, cw, ch;
  if (s->preset.kind == RNF_PRESET_NATIVE) {
    k = std::max(1, s->preset.scale);
    dw = even(width(k));
    dh = sh * k;
    cw = dw;
    ch = even(dh);
  } else {
    const int w = s->preset.width, h = s->preset.height;
    k = std::max(1, sh > 0 ? h / sh : 1);
    while (k > 1 && width(k) > w) k -= 1;
    dh = std::min(h, sh * k);
    dw = std::min(w, width(k));
    cw = even(w);
    ch = even(h);
  }
  out->src_x = s->crop_left;
  out->src_y = s->crop_top;
  out->src_width = sw;
  out->src_height = sh;
  out->vertical_scale = k;
  out->canvas_width = cw;
  out->canvas_height = ch;
  out->dst_width = dw;
  out->dst_height = dh;
  out->dst_x = (cw - dw) / 2;
  out->dst_y = (ch - dh) / 2;
}

void rnf_export_column_map(const rnf_export_geometry* g, int* out) {
  if (!g || !out || g->dst_width <= 0) return;
  for (int i = 0; i < g->dst_width; ++i) out[i] = g->src_x + (i * g->src_width) / g->dst_width;
}
void rnf_export_row_map(const rnf_export_geometry* g, int* out) {
  if (!g || !out || g->dst_height <= 0) return;
  for (int i = 0; i < g->dst_height; ++i) out[i] = g->src_y + (i * g->src_height) / g->dst_height;
}

int64_t rnf_export_video_bitrate(const rnf_export_geometry* g, double bits_per_pixel) {
  if (!g) return 2000000;
  double pixels = double(g->canvas_width * g->canvas_height);
  return std::max<int64_t>(2000000, int64_t(pixels * 60.0 * bits_per_pixel));
}

// ------------------------------------------------------------------ streaming output
int rnf_stream_output_multiple(rnf_stream_size size) { return streamMultiple(size); }

rnf_stream_layout rnf_stream_output_layout(rnf_stream_size size, int par87) {
  const double fw = RN_VIDEO_WIDTH, fh = RN_VIDEO_HEIGHT;
  const double par = par87 ? 8.0 / 7.0 : 1.0;
  rnf_stream_layout l{};
  if (int n = streamMultiple(size)) {
    // The canvas is exactly the scaled frame: no borders.
    int w = int(std::round(fw * par * double(n))), h = RN_VIDEO_HEIGHT * n;
    l.canvas_width = w;
    l.canvas_height = h;
    l.x = 0;
    l.y = 0;
    l.width = w;
    l.height = h;
    return l;
  }
  const int cw = size == RNF_STREAM_W1280 ? 1280 : 1920, ch = size == RNF_STREAM_W1280 ? 960 : 1440;
  // Largest scale that fits; integer when the aspect allows it (it does for both canvases).
  double s = std::min(double(cw) / (fw * par), double(ch) / fh);
  double pw = std::round(fw * par * s), ph = std::round(fh * s);
  l.canvas_width = cw;
  l.canvas_height = ch;
  l.x = std::floor((double(cw) - pw) / 2);
  l.y = std::floor((double(ch) - ph) / 2);
  l.width = pw;
  l.height = ph;
  return l;
}

char* rnf_stream_output_label(rnf_stream_size size, int par87) {
  RNF_GUARD_BEGIN
  rnf_stream_layout l = rnf_stream_output_layout(size, par87);
  std::string dims = std::to_string(l.canvas_width) + "×" + std::to_string(l.canvas_height);  // plain digits
  int n = streamMultiple(size);
  if (n == 1) return dup(trf(RNF_L("Native %@"), {argS(dims)}));
  if (n) return dup(trf(RNF_L("%lld× %@"), {argI(n), argS(dims)}));
  return dup(trf(RNF_L("%@ (4:3, with black bars)"), {argS(dims)}));
  RNF_GUARD_END(nullptr)
}

// ------------------------------------------------------------------ flash reduction
char* rnf_flash_level_label(rn_flash_level level) {
  switch (level) {
    case RN_FLASH_OFF: return dup(tr(RNF_L("Off")));
    case RN_FLASH_LOW: return dup(tr(RNF_L("Low")));
    case RN_FLASH_STANDARD: return dup(tr(RNF_L("Standard")));
    case RN_FLASH_HIGH: return dup(tr(RNF_L("High")));
  }
  return dup("");
}

char* rnf_flash_level_detail(rn_flash_level level) {
  switch (level) {
    case RN_FLASH_OFF: return dup(tr(RNF_L("Shows the screen as it is.")));
    case RN_FLASH_LOW:
      return dup(tr(RNF_L("Follows the WCAG 2.x thresholds as written: areas of 25% or more of the screen flash no more than 3 times per second.")));
    case RN_FLASH_STANDARD:
      return dup(tr(RNF_L("Detects earlier than the thresholds and limits flashes of 20% or more of the screen to 2 per second (recommended).")));
    case RN_FLASH_HIGH:
      return dup(tr(RNF_L("Limits flashes of 15% or more of the screen to 1 per second and also softens the remaining small flicker.")));
  }
  return dup("");
}

}  // extern "C"
