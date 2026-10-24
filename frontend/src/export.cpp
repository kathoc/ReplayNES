// Export settings validation + geometry (nearest-neighbour mapping of the cropped source
// into the output canvas), the streaming output canvas (Syphon / PipeWire / NDI-style outputs) and
// the flash reduction level texts. Encoders stay in the frontends (AVFoundation, FFmpeg).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <limits>

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

const rn_media_info kNesMedia = {RN_VIDEO_WIDTH, RN_VIDEO_HEIGHT, RN_FPS_NUM, RN_FPS_DEN, 1, 8, 7, 0};
bool validMedia(const rn_media_info* m) {
  return m && m->max_width && m->max_height && m->max_width <= uint32_t(std::numeric_limits<int>::max()) &&
         m->max_height <= uint32_t(std::numeric_limits<int>::max()) &&
         m->fps_num && m->fps_den && (m->audio_channels == 1 || m->audio_channels == 2) &&
         m->pixel_aspect_num && m->pixel_aspect_den && m->rotation < 4;
}
bool sourceSize(const rn_media_info* m, uint32_t& w, uint32_t& h) {
  if (!validMedia(m)) return false;
  if (!w) w = m->max_width;
  if (!h) h = m->max_height;
  return w <= m->max_width && h <= m->max_height;
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
  return rnf_export_validate_media(s, &kNesMedia, 0, 0, message);
}
rn_status rnf_export_validate_media(const rnf_export_settings* s, const rn_media_info* media,
                                    uint32_t width, uint32_t height, char** message) {
  if (message) *message = nullptr;
  if (!sourceSize(media, width, height)) return fail(RN_ERR_INVALID_ARG, "invalid media or source size");
  if (!s) return fail(RN_ERR_INVALID_ARG, "null settings");
  bool ok = s->crop_top >= 0 && s->crop_bottom >= 0 && s->crop_left >= 0 && s->crop_right >= 0 &&
            int64_t(s->crop_top) + s->crop_bottom <= int64_t(height) - 16 &&
            int64_t(s->crop_left) + s->crop_right <= int64_t(width) - 16;
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
  rnf_export_geometry_compute_media(s, &kNesMedia, 0, 0, out);
}
void rnf_export_geometry_compute_media(const rnf_export_settings* s, const rn_media_info* media,
                                       uint32_t widthPixels, uint32_t heightPixels, rnf_export_geometry* out) {
  if (!out) return;
  *out = {};
  if (!s) return;
  auto geometrySettings = *s;
  geometrySettings.end_frame = 0;  // Range validity does not affect picture geometry.
  if (rnf_export_validate_media(&geometrySettings, media, widthPixels, heightPixels, nullptr) != RN_OK) return;
  sourceSize(media, widthPixels, heightPixels);
  const int sw = int(widthPixels) - s->crop_left - s->crop_right;
  const int sh = int(heightPixels) - s->crop_top - s->crop_bottom;
  const double par = s->pixel_aspect_87 ? double(media->pixel_aspect_num) / media->pixel_aspect_den : 1.0;
  const bool turned = (media->rotation & 1) != 0;
  const double displayHeight = turned ? sw * par : sh;
  auto even = [](int v) { return v + (v & 1); };
  // Keep the NES multiplication order exactly as in the original geometry path.
  auto width = [&](int scale) { return std::round(turned ? double(sh) * scale : double(int64_t(sw) * scale) * par); };
  auto height = [&](int scale) { return turned ? std::round(double(int64_t(sw) * scale) * par) : double(int64_t(sh) * scale); };
  int k, dw, dh, cw, ch;
  if (s->preset.kind == RNF_PRESET_NATIVE) {
    k = std::max(1, s->preset.scale);
    if (width(k) > std::numeric_limits<int>::max() - 1 || height(k) > std::numeric_limits<int>::max() - 1) return;
    dw = even(int(width(k)));
    dh = int(height(k));
    cw = dw;
    ch = even(dh);
  } else {
    const int w = s->preset.width, h = s->preset.height;
    if (w <= 0 || h <= 0 || w == std::numeric_limits<int>::max() || h == std::numeric_limits<int>::max()) return;
    const double maxScale = std::min(double(h) / displayHeight, double(std::numeric_limits<int>::max()));
    k = std::max(1, int(maxScale));
    // Jump near the width limit before the rounding check, even for extreme pixel aspects.
    const double displayWidth = turned ? double(sh) : sw * par;
    if (width(k) > w) k = std::max(1, int(std::min(double(k), (double(w) + 0.5) / displayWidth)));
    while (k > 1 && width(k) > w) k -= 1;
    dh = int(std::min(double(h), height(k)));
    dw = int(std::min(double(w), width(k)));
    cw = even(w);
    ch = even(h);
  }
  out->rotation = media->rotation;
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

int rnf_export_source_point(const rnf_export_geometry* g, int x, int y, int* sx, int* sy) {
  if (!g || !sx || !sy || x < 0 || y < 0 || x >= g->dst_width || y >= g->dst_height) return 0;
  const int u = int(int64_t(x) * ((g->rotation & 1) ? g->src_height : g->src_width) / g->dst_width);
  const int v = int(int64_t(y) * ((g->rotation & 1) ? g->src_width : g->src_height) / g->dst_height);
  switch (g->rotation) {
    case 1: *sx = v; *sy = g->src_height - 1 - u; break;
    case 2: *sx = g->src_width - 1 - u; *sy = g->src_height - 1 - v; break;
    case 3: *sx = g->src_width - 1 - v; *sy = u; break;
    default: *sx = u; *sy = v; break;
  }
  *sx += g->src_x;
  *sy += g->src_y;
  return 1;
}

rn_status rnf_export_timing_compute(const rn_media_info* media, double audioRate, rnf_export_timing* out) {
  if (!out) return fail(RN_ERR_INVALID_ARG, "null timing");
  *out = {};
  if (!validMedia(media) || !std::isfinite(audioRate) || audioRate <= 0)
    return fail(RN_ERR_INVALID_ARG, "invalid export timing");
  out->timescale = media->fps_num;
  out->frame_duration = media->fps_den;
  out->audio_channels = media->audio_channels;
  out->audio_sample_rate = audioRate;
  out->gop_frames = uint64_t(std::max(1.0, std::round(double(media->fps_num) / media->fps_den / 2)));
  return RN_OK;
}

int64_t rnf_export_video_bitrate(const rnf_export_geometry* g, int hevc, int quality) {
  // YouTube's recommended SDR bit rates for 48-60 fps uploads (Mbit/s) by height.
  static const struct { double h, mbit; } kTable[] = {{360, 1.5}, {480, 4}, {720, 7.5}, {1080, 12}, {1440, 24}, {2160, 60}};
  const double floorMbit = 1.5;
  const double h = g ? double(std::max(1, g->canvas_height)) : 360.0;
  const double h2 = h * h;
  const int n = int(sizeof kTable / sizeof kTable[0]);
  double mbit;
  if (h2 <= kTable[0].h * kTable[0].h) {
    mbit = kTable[0].mbit * h2 / (kTable[0].h * kTable[0].h);
  } else if (h2 >= kTable[n - 1].h * kTable[n - 1].h) {
    mbit = kTable[n - 1].mbit * h2 / (kTable[n - 1].h * kTable[n - 1].h);
  } else {
    int i = 0;
    while (i + 2 < n && h2 > kTable[i + 1].h * kTable[i + 1].h) ++i;
    const double a = kTable[i].h * kTable[i].h, b = kTable[i + 1].h * kTable[i + 1].h;
    mbit = kTable[i].mbit + (kTable[i + 1].mbit - kTable[i].mbit) * (h2 - a) / (b - a);
  }
  mbit = std::max(floorMbit, mbit);
  if (hevc) mbit *= 0.7;
  if (quality == RNF_QUALITY_LIGHT) mbit *= 0.5;
  else if (quality == RNF_QUALITY_HIGH) mbit *= 2.0;
  return int64_t(std::llround(mbit * 1e6));
}

int64_t rnf_export_audio_bitrate(void) { return 128000; }

int64_t rnf_export_predict_size(const rnf_export_geometry* g, int hevc, int quality, uint64_t frames,
                                rnf_export_prediction* detail) {
  return rnf_export_predict_size_media(g, hevc, quality, frames, &kNesMedia, RN_SAMPLE_RATE, detail);
}
int64_t rnf_export_predict_size_media(const rnf_export_geometry* g, int hevc, int quality, uint64_t frames,
                                      const rn_media_info* media, double audioRate, rnf_export_prediction* detail) {
  if (detail) *detail = {};
  rnf_export_timing timing{};
  if (rnf_export_timing_compute(media, audioRate, &timing) != RN_OK) return 0;
  rnf_export_prediction p{};
  p.video_bitrate = rnf_export_video_bitrate(g, hevc, quality);
  p.audio_bitrate = rnf_export_audio_bitrate() * media->audio_channels;
  p.seconds = double(frames) * double(media->fps_den) / double(media->fps_num);
  p.video_bytes = int64_t(std::llround(p.seconds * double(p.video_bitrate) / 8.0));
  p.audio_bytes = int64_t(std::llround(p.seconds * double(p.audio_bitrate) / 8.0));
  // An estimate: the encoders' average rate (capped VBR) is reached by detailed pictures; simple
  // ones give smaller files. Container: the boxes the platform's muxer writes (ISO 14496-12; sizes of the boxes of a real
  // file of each muxer, tables per sample / chunk), see container notes below.
  const int64_t n = int64_t(frames);
  const int64_t keys = (n + int64_t(timing.gop_frames) - 1) / int64_t(timing.gop_frames);
  const int64_t aac = int64_t(std::ceil(p.seconds * audioRate / 1024.0));
  const bool big = p.video_bytes + p.audio_bytes > int64_t(4) * 1024 * 1024 * 1024;  // co64 / 64-bit mdat
  const int64_t offset = big ? 8 : 4;
  auto table = [&](int64_t entries, int64_t entryBytes) { return 16 + entries * entryBytes; };
  // trak: tkhd 92, edts 36, mdhd 32, hdlr ~47, xmhd ~18, dinf 36 + the box headers.
  const int64_t trackFixed = 8 + 92 + 36 + 8 + 32 + 47 + 8 + 18 + 36 + 8;
  int64_t vChunks, vStsc, aChunks, aStsc, perSample = 4, vStsd = hevc ? 260 : 200, aStsd = 110, extra;
#if defined(__APPLE__)
  // AVAssetWriter: a chunk per track every 0.5 s or ~800 kB of video, B-frames (stsz 4 + ctts 8 +
  // sdtp 1 byte per frame; sdtp box header 12), stsd avc1 185 / hvc1 254 / mp4a 103.
  perSample += 9;
  vStsd = hevc ? 254 : 185;
  aStsd = 103;
  vChunks = std::max<int64_t>(1, std::max<int64_t>(int64_t(std::ceil(p.seconds / 0.5)), (p.video_bytes + 799999) / 800000));
  vStsc = std::max<int64_t>(1, vChunks / 5);
  aChunks = vChunks;
  aStsc = std::max<int64_t>(1, aChunks * 4 / 5);
  extra = 12 + 54 /* sdtp header, sgpd + sbgp */;
#elif defined(_WIN32)
  // Media Foundation MPEG-4 sink: ~1 s chunks, no B-frames.
  vChunks = aChunks = std::max<int64_t>(1, int64_t(std::ceil(p.seconds)));
  vStsc = aStsc = 1;
  extra = 100;
#else
  // libav mov muxer: one chunk per interleaved AAC frame; the stsc run table changes whenever the
  // frames per chunk change (N / aac = frames per audio frame, e.g. 1.28 -> chunks of 1 or 2).
  aChunks = std::max<int64_t>(1, aac);
  vChunks = aChunks;
  const double r = aac > 0 ? double(n) / double(aac) : 1.0, f = r - std::floor(r);
  vStsc = std::max<int64_t>(1, int64_t(std::llround(2.0 * std::min(f, 1.0 - f) * double(aChunks))));
  aStsc = 1;
  extra = 8 /* free */ + 98 /* udta */ + 54 /* sgpd, sbgp */;
#endif
  int64_t c = 32 /* ftyp */ + (big ? 16 : 8) /* mdat header */ + 8 + 108 /* moov, mvhd */ + extra;
  // video: stsd, stts (one run), stss (key frames), stsz (20 + 4/sample), stsc, stco
  c += trackFixed + vStsd + table(1, 8) + table(keys, 4) + 4 + table(n, perSample) + table(vStsc, 12) + table(vChunks, offset);
  // audio: stsd, stts, stsz (one entry per AAC frame), stsc, stco
  c += trackFixed + aStsd + table(1, 8) + 4 + table(aac, 4) + table(aStsc, 12) + table(aChunks, offset);
  p.container_bytes = c;
  p.total_bytes = p.video_bytes + p.audio_bytes + p.container_bytes;
  if (detail) *detail = p;
  return p.total_bytes;
}

// ------------------------------------------------------------------ streaming output
int rnf_stream_output_multiple(rnf_stream_size size) { return streamMultiple(size); }

rnf_stream_layout rnf_stream_output_layout(rnf_stream_size size, int par87) {
  return rnf_stream_output_layout_media(size, par87, &kNesMedia, 0, 0);
}
rnf_stream_layout rnf_stream_output_layout_media(rnf_stream_size size, int gameAspect, const rn_media_info* media,
                                                  uint32_t width, uint32_t height) {
  rnf_stream_layout l{};
  if (!sourceSize(media, width, height)) return l;
  const double par = gameAspect ? double(media->pixel_aspect_num) / media->pixel_aspect_den : 1.0;
  double fw = width * par, fh = height;
  if (media->rotation & 1) std::swap(fw, fh);
  if (int n = streamMultiple(size)) {
    // The canvas is exactly the scaled frame: no borders.
    if (std::round(fw * n) > std::numeric_limits<int>::max() ||
        std::round(fh * n) > std::numeric_limits<int>::max()) return l;
    int w = int(std::round(fw * double(n))), h = int(std::round(fh * n));
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
  double s = std::min(double(cw) / fw, double(ch) / fh);
  double pw = std::round(fw * s), ph = std::round(fh * s);
  l.canvas_width = cw;
  l.canvas_height = ch;
  l.x = std::floor((double(cw) - pw) / 2);
  l.y = std::floor((double(ch) - ph) / 2);
  l.width = pw;
  l.height = ph;
  return l;
}

char* rnf_stream_output_label(rnf_stream_size size, int par87) {
  return rnf_stream_output_label_media(size, par87, &kNesMedia, 0, 0);
}
char* rnf_stream_output_label_media(rnf_stream_size size, int gameAspect, const rn_media_info* media,
                                   uint32_t width, uint32_t height) {
  RNF_GUARD_BEGIN
  rnf_stream_layout l = rnf_stream_output_layout_media(size, gameAspect, media, width, height);
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
      return dup(tr(RNF_L("Like Standard, limits flashes to 2 per second, but also reacts to smaller areas (15% or more of the screen) and smaller changes, and softens the remaining small flicker.")));
  }
  return dup("");
}

}  // extern "C"
