// replaynes-export: headless MP4 export (the Linux exporter without the UI) + a self-test.
//   replaynes-export --project DIR --out FILE.mp4 [--start N] [--end N] [--preset INDEX] [--flash 0-3]
//                    [--par87] [--no-crop] [--encoder NAME] [--bpp X] [--verify-hash] [--quiet]
//   replaynes-export --self-test DIR [--frames N] [--encoder NAME]
//   replaynes-export --list-presets | --encoders
// Prints progress on stderr and one JSON result line on stdout.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "mp4_export.h"
#ifdef RNL_EXPORT_CRT
#include "render/crt_export.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

namespace fs = std::filesystem;
using rnl::ExportOptions;
using rnl::ExportResult;

namespace {

struct Args {
  std::vector<std::string> v;
  bool has(const char* k) const { return std::find(v.begin(), v.end(), k) != v.end(); }
  const char* get(const char* k) const {
    for (size_t i = 0; i + 1 < v.size(); ++i)
      if (v[i] == k) return v[i + 1].c_str();
    return nullptr;
  }
  uint64_t num(const char* k, uint64_t def) const {
    const char* s = get(k);
    return s ? std::strtoull(s, nullptr, 10) : def;
  }
};

std::string jsonStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (uint8_t(c) < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
      continue;
    }
    o += c;
  }
  return o + "\"";
}

std::string hex(uint64_t h) {
  char b[24];
  std::snprintf(b, sizeof b, "%016" PRIx64, h);
  return b;
}

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  replaynes-export --project DIR --out FILE.mp4 [--start N] [--end N] [--preset INDEX]\n"
               "                   [--flash 0-3] [--par87] [--no-crop] [--encoder NAME] [--bpp X]\n"
               "                   [--verify-hash] [--quiet]\n"
               "                   [--crt [--crt-lines N] [--crt-dbuv X] [--crt-no-growth|-persistence|-supply]]\n"
               "  replaynes-export --self-test DIR [--frames N] [--encoder NAME]\n"
               "  replaynes-export --list-presets | --encoders\n");
  return 2;
}

/// Hash of a second, fresh renderer over the same range (determinism check).
bool freshHash(rn_session* s, uint64_t start, uint64_t end, uint64_t* hash, uint64_t* frames) {
  rn_renderer* r = nullptr;
  if (rn_renderer_new(s, start, end, &r) != RN_OK) return false;
  uint64_t n = 0;
  for (;;) {
    const uint32_t* v;
    const int16_t* a;
    size_t c;
    uint64_t f;
    rn_status st = rn_renderer_next(r, &v, &a, &c, &f);
    if (st == RN_ERR_END_OF_TAKE) break;
    if (st != RN_OK) {
      rn_renderer_free(r);
      return false;
    }
    ++n;
  }
  *hash = rn_renderer_hash(r);
  if (frames) *frames = n;
  rn_renderer_free(r);
  return true;
}

struct RunOut {
  bool ok = false;
  ExportResult res;
  std::string error;
  double seconds = 0;
};

RunOut runExport(rn_session* s, const ExportOptions& opt, const std::string& out, bool showProgress,
                 uint64_t cancelAfter = 0) {
  RunOut o;
  rn_renderer* r = nullptr;
  rn_status st = rn_renderer_new(s, opt.settings.start_frame, opt.settings.end_frame, &r);
  if (st != RN_OK) {
    o.error = std::string(rn_status_name(st)) + ": " + rn_last_error();
    return o;
  }
  uint64_t lastShown = 0, doneNow = 0;
  const double t0 = now();
  o.ok = rnl::exportProject(
      r, opt, out,
      [&](uint64_t done, uint64_t total) {
        doneNow = done;
        if (showProgress && (done - lastShown >= 60 || done == total)) {
          lastShown = done;
          std::fprintf(stderr, "\rexport %" PRIu64 "/%" PRIu64 " (%.0f%%)", done, total, 100.0 * double(done) / double(total));
          if (done == total) std::fprintf(stderr, "\n");
        }
      },
      [&]() { return cancelAfter != 0 && doneNow >= cancelAfter; }, &o.res, &o.error);
  o.seconds = now() - t0;
  return o;
}

std::string resultJson(const RunOut& o, const std::string& out) {
  std::string j = "{\"ok\":" + std::string(o.ok ? "true" : "false");
  if (o.ok) {
    char b[512];
    std::snprintf(b, sizeof b,
                  ",\"frames\":%" PRIu64 ",\"audio_samples\":%" PRIu64 ",\"renderer_hash\":\"%s\",\"duration\":%.6f,"
                  "\"encoder\":%s,\"seconds\":%.3f,\"fps\":%.1f",
                  o.res.frames, o.res.audioSamples, hex(o.res.rendererHash).c_str(), o.res.duration,
                  jsonStr(o.res.encoder).c_str(), o.seconds, o.seconds > 0 ? double(o.res.frames) / o.seconds : 0.0);
    j += b;
  } else {
    j += ",\"error\":" + jsonStr(o.error);
  }
  return j + ",\"out\":" + jsonStr(out) + "}";
}

// ------------------------------------------------------------------ verification by demuxing the file
struct Probe {
  std::string error;
  AVCodecID vcodec = AV_CODEC_ID_NONE, acodec = AV_CODEC_ID_NONE;
  int width = 0, height = 0, profile = 0;
  AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
  AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
  AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
  AVRational vtb{0, 1}, avg{0, 1};
  uint64_t vpackets = 0, decoded = 0;
  bool decoderMissing = false;
  bool ptsExact = false;
  int64_t vDuration = 0, aDuration = 0;  // stream durations in their time bases
  int aRate = 0, aChannels = 0;
};

Probe probe(const std::string& path) {
  Probe p;
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
    p.error = "cannot open";
    return p;
  }
  avformat_find_stream_info(fmt, nullptr);
  int vi = -1, ai = -1;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    const AVCodecParameters* cp = fmt->streams[i]->codecpar;
    if (cp->codec_type == AVMEDIA_TYPE_VIDEO && vi < 0) vi = int(i);
    if (cp->codec_type == AVMEDIA_TYPE_AUDIO && ai < 0) ai = int(i);
  }
  if (vi < 0 || ai < 0) {
    p.error = "missing stream";
    avformat_close_input(&fmt);
    return p;
  }
  AVStream* vs = fmt->streams[vi];
  AVStream* as = fmt->streams[ai];
  p.vcodec = vs->codecpar->codec_id;
  p.acodec = as->codecpar->codec_id;
  p.width = vs->codecpar->width;
  p.height = vs->codecpar->height;
  p.profile = vs->codecpar->profile;
  p.primaries = vs->codecpar->color_primaries;
  p.range = vs->codecpar->color_range;
  p.matrix = vs->codecpar->color_space;
  p.vtb = vs->time_base;
  p.avg = vs->avg_frame_rate;
  p.vDuration = vs->duration;
  p.aDuration = as->duration;
  p.aRate = as->codecpar->sample_rate;
  p.aChannels = as->codecpar->ch_layout.nb_channels;

  const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecContext* dc = nullptr;
  if (dec && (dc = avcodec_alloc_context3(dec)) && avcodec_parameters_to_context(dc, vs->codecpar) >= 0 &&
      avcodec_open2(dc, dec, nullptr) >= 0) {
  } else {
    avcodec_free_context(&dc);
    p.decoderMissing = true;
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* fr = av_frame_alloc();
  std::vector<int64_t> pts;
  auto drain = [&]() {
    while (avcodec_receive_frame(dc, fr) >= 0) {
      ++p.decoded;
      av_frame_unref(fr);
    }
  };
  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vi) {
      ++p.vpackets;
      pts.push_back(pkt->pts);
      if (dc && avcodec_send_packet(dc, pkt) >= 0) drain();
    }
    av_packet_unref(pkt);
  }
  if (dc) {
    avcodec_send_packet(dc, nullptr);
    drain();
  }
  std::sort(pts.begin(), pts.end());
  p.ptsExact = !pts.empty() && p.vtb.num == 1 && p.vtb.den == RN_FPS_NUM;
  for (size_t i = 0; p.ptsExact && i < pts.size(); ++i)
    if (pts[i] != int64_t(i) * RN_FPS_DEN) p.ptsExact = false;
  av_frame_free(&fr);
  av_packet_free(&pkt);
  avcodec_free_context(&dc);
  avformat_close_input(&fmt);
  return p;
}

/// Checks a finished export file against the export result; prints a line, returns failures.
int checkFile(const char* label, const std::string& path, const ExportResult& res, uint64_t expectedSamples,
              int expectWidth, int expectHeight) {
  Probe p = probe(path);
  std::vector<std::string> bad;
  if (!p.error.empty()) bad.push_back(p.error);
  if (p.vcodec != AV_CODEC_ID_H264) bad.push_back("video codec not H.264");
  if (p.acodec != AV_CODEC_ID_AAC) bad.push_back("audio codec not AAC");
  if (res.encoder == "libx264" && p.profile != AV_PROFILE_H264_HIGH) bad.push_back("profile not High");
  if (p.width != expectWidth || p.height != expectHeight) bad.push_back("size");
  if (p.primaries != AVCOL_PRI_BT709 || p.matrix != AVCOL_SPC_BT709 || p.range != AVCOL_RANGE_MPEG)
    bad.push_back("colour tags");
  if (p.vpackets != res.frames) bad.push_back("video packet count");
  if (!p.decoderMissing && p.decoded != res.frames) bad.push_back("decoded frame count");
  if (!p.ptsExact) bad.push_back("video PTS not (f - start) * 655171 / 39375000");
  if (p.avg.num * int64_t(RN_FPS_DEN) != p.avg.den * int64_t(RN_FPS_NUM)) bad.push_back("avg_frame_rate");
  if (p.vDuration != int64_t(res.frames) * RN_FPS_DEN) bad.push_back("video duration");
  if (p.aRate != RN_SAMPLE_RATE || p.aChannels != 1) bad.push_back("audio format");
  if (std::llabs(p.aDuration - int64_t(expectedSamples)) > 1) bad.push_back("audio duration");
  std::printf("  %-10s %s: %dx%d, %" PRIu64 " video packets, %" PRIu64 " decoded%s, avg_frame_rate %d/%d, video %" PRId64
              " ticks (%.4f s), audio %" PRId64 " samples (expected %" PRIu64 ")%s%s\n",
              label, bad.empty() ? "OK" : "FAIL", p.width, p.height, p.vpackets, p.decoded,
              p.decoderMissing ? " (no H.264 decoder)" : "", p.avg.num, p.avg.den, p.vDuration,
              double(p.vDuration) / RN_FPS_NUM, p.aDuration, expectedSamples, bad.empty() ? "" : " -- ", "");
  for (const auto& b : bad) std::printf("             - %s\n", b.c_str());
  return int(bad.size());
}

/// Test processor (stands in for the CRT): greyscale from the PPU codes when present, otherwise
/// from the RGB picture; counts calls and checks the ordinals are consecutive.
class TestProcessor : public rnl::ExportVideoProcessor {
 public:
  uint64_t* calls;
  bool* ordered;
  bool* sawCodes;
  explicit TestProcessor(uint64_t* c, bool* o, bool* s) : calls(c), ordered(o), sawCodes(s) {}
  rnf_export_geometry g{};
  uint64_t last = 0;
  bool begin(const rnf_export_settings&, const rnf_export_geometry& geo, std::string*) override {
    g = geo;
    return true;
  }
  bool render(const rnl::ExportFrameSignal& sig, const uint32_t* pixels, uint8_t* canvas, size_t stride,
              std::string*) override {
    if (*calls > 0 && sig.ordinal != last + 1) *ordered = false;
    last = sig.ordinal;
    if (sig.codes) *sawCodes = true;
    for (int y = 0; y < g.canvas_height; ++y) {
      uint32_t* row = reinterpret_cast<uint32_t*>(canvas + size_t(y) * stride);
      const int sy = y * RN_VIDEO_HEIGHT / g.canvas_height;
      for (int x = 0; x < g.canvas_width; ++x) {
        const int sx = x * RN_VIDEO_WIDTH / g.canvas_width;
        uint32_t v;
        if (sig.codes && !sig.flashAltered) v = uint32_t(sig.codes[sy * RN_VIDEO_WIDTH + sx] & 0x3F) * 4;
        else v = (pixels[sy * RN_VIDEO_WIDTH + sx] >> 8) & 0xFF;
        row[x] = 0xFF000000u | v << 16 | v << 8 | v;
      }
    }
    ++*calls;
    return true;
  }
};

int selfTest(const Args& a) {
  const fs::path dir = a.get("--self-test");
  const uint64_t frames = std::max<uint64_t>(a.num("--frames", 300), 260);
  const std::string encoder = a.get("--encoder") ? a.get("--encoder") : "";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path rom = dir / "selftest.nes", proj = dir / "selftest.nesrec";
  fs::remove_all(proj, ec);
  std::printf("replaynes-export self-test in %s (%" PRIu64 " frames)\n", dir.c_str(), frames);
  std::printf("  libavcodec %s, encoders: %s\n", AV_STRINGIFY(LIBAVCODEC_VERSION), rnl::availableEncodersDescription().c_str());
  if (rn_write_test_rom(rom.c_str()) != RN_OK) {
    std::printf("  FAIL: write test ROM: %s\n", rn_last_error());
    return 1;
  }
  rn_session_options so;
  rn_session_options_init(&so);
  rn_session* s = nullptr;
  if (rn_session_new(rom.c_str(), proj.c_str(), &so, &s) != RN_OK) {
    std::printf("  FAIL: new session: %s\n", rn_last_error());
    return 1;
  }
  // Some input: d-pad sweeps, A/B taps, Start now and then.
  for (uint64_t f = 0; f < frames; ++f) {
    uint8_t p1 = 0;
    if ((f / 20) % 4 == 1) p1 |= RN_BTN_RIGHT;
    if ((f / 20) % 4 == 3) p1 |= RN_BTN_LEFT;
    if (f % 7 == 0) p1 |= RN_BTN_A;
    if (f % 11 == 0) p1 |= RN_BTN_B;
    if (f % 90 == 45) p1 |= RN_BTN_START;
    if (rn_step(s, p1, uint8_t(f % 13 == 0 ? RN_BTN_UP : 0), 0, nullptr) != RN_OK) {
      std::printf("  FAIL: step: %s\n", rn_last_error());
      return 1;
    }
  }
  if (rn_session_save(s) != RN_OK) {
    std::printf("  FAIL: save: %s\n", rn_last_error());
    return 1;
  }
  rn_session_close(s);
  s = nullptr;
  if (rn_session_open(proj.c_str(), nullptr, &so, &s) != RN_OK) {
    std::printf("  FAIL: reopen: %s\n", rn_last_error());
    return 1;
  }
  const uint64_t take = rn_take_length(s);
  std::printf("  recorded %" PRIu64 " frames into %s\n", take, proj.c_str());

  int failures = 0;
  auto expectTrue = [&](bool ok, const char* what) {
    std::printf("  %-48s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  };

  // 1) whole take, defaults (1280x960, crop 8/8)
  ExportOptions base;
  base.encoder = encoder;
  const std::string fullOut = (dir / "full.mp4").string();
  RunOut full = runExport(s, base, fullOut, false);
  std::printf("  full:      %s\n", resultJson(full, fullOut).c_str());
  if (!full.ok) {
    rn_session_close(s);
    return 1;
  }
  uint64_t h = 0, n = 0;
  expectTrue(freshHash(s, 0, 0, &h, &n) && h == full.res.rendererHash && n == full.res.frames,
             "full: renderer hash == fresh renderer");
  expectTrue(full.res.frames == take, "full: frames == take length");
  failures += checkFile("full", fullOut, full.res, rn_audio_samples_before(take), 1280, 960);

  // 2) flash reduction High + a processor: same renderer hash (display only)
  ExportOptions fx = base;
  fx.flash = RN_FLASH_HIGH;
  uint64_t calls = 0;
  bool ordered = true, sawCodes = false;
  fx.makeProcessor = [&](std::string*) { return std::make_unique<TestProcessor>(&calls, &ordered, &sawCodes); };
  const std::string fxOut = (dir / "flash-processor.mp4").string();
  RunOut fxr = runExport(s, fx, fxOut, false);
  std::printf("  processor: %s\n", resultJson(fxr, fxOut).c_str());
  expectTrue(fxr.ok && fxr.res.rendererHash == full.res.rendererHash, "flash+processor: same renderer hash");
  expectTrue(calls == take && ordered, "processor: one call per frame, consecutive ordinals");
  std::printf("  processor saw PPU codes: %s\n", sawCodes ? "yes" : "no (core without raw output)");
  if (fxr.ok) failures += checkFile("processor", fxOut, fxr.res, rn_audio_samples_before(take), 1280, 960);

  // 3) sub-range [100, 250)
  ExportOptions sub = base;
  sub.settings.start_frame = 100;
  sub.settings.end_frame = 250;
  const std::string subOut = (dir / "range-100-250.mp4").string();
  RunOut subr = runExport(s, sub, subOut, false);
  std::printf("  range:     %s\n", resultJson(subr, subOut).c_str());
  expectTrue(subr.ok && subr.res.frames == 150, "range: 150 frames");
  expectTrue(subr.ok && freshHash(s, 100, 250, &h, &n) && h == subr.res.rendererHash, "range: renderer hash == fresh renderer");
  if (subr.ok)
    failures += checkFile("range", subOut, subr.res, rn_audio_samples_before(250) - rn_audio_samples_before(100), 1280, 960);

  // 4) Native x1, 8:7, no crop -> 294x240
  ExportOptions nat = base;
  rnf_export_preset_get(0, &nat.settings.preset);
  nat.settings.pixel_aspect_87 = 1;
  nat.settings.crop_top = nat.settings.crop_bottom = 0;
  const std::string natOut = (dir / "native-87.mp4").string();
  RunOut natr = runExport(s, nat, natOut, false);
  std::printf("  native:    %s\n", resultJson(natr, natOut).c_str());
  rnf_export_geometry g{};
  rnf_export_geometry_compute(&nat.settings, &g);
  expectTrue(natr.ok && natr.res.rendererHash == full.res.rendererHash, "native 8:7: same renderer hash");
  if (natr.ok) failures += checkFile("native", natOut, natr.res, rn_audio_samples_before(take), g.canvas_width, g.canvas_height);

  // 5) cancel after 50 frames: false, "cancelled", no file left
  const std::string cancelOut = (dir / "cancelled.mp4").string();
  RunOut can = runExport(s, base, cancelOut, false, 50);
  expectTrue(!can.ok && can.error == "cancelled" && !fs::exists(cancelOut), "cancel: error \"cancelled\", partial file removed");

  // 6) invalid settings + empty range
  ExportOptions badCrop = base;
  badCrop.settings.crop_top = 230;  // + 8 bottom > 224
  fs::remove(dir / "bad.mp4", ec);
  RunOut bad = runExport(s, badCrop, (dir / "bad.mp4").string(), false);
  expectTrue(!bad.ok && !fs::exists(dir / "bad.mp4"), "invalid crop: rejected");

  // 7) ExportJob (worker thread, polled)
  {
    rnl::ExportJob job;
    ExportOptions jo = base;
    jo.settings.start_frame = 30;
    const std::string jobOut = (dir / "job.mp4").string();
    bool started = job.start(s, jo, jobOut);
    while (started && !job.finished()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    job.wait();
    expectTrue(started && job.succeeded() && job.result().frames == take - 30 && job.progress() == 1.0,
               "ExportJob: worker export of [30, end)");
    rnl::ExportJob job2;
    started = job2.start(s, base, (dir / "job-cancel.mp4").string());
    job2.cancel();
    job2.wait();
    expectTrue(started && job2.finished() && job2.wasCancelled() && !fs::exists(dir / "job-cancel.mp4"),
               "ExportJob: cancel");
  }

  rn_session_close(s);
  std::printf("{\"self_test\":%s,\"failures\":%d,\"encoder\":%s,\"full_seconds\":%.3f,\"full_fps\":%.1f,\"renderer_hash\":\"%s\"}\n",
              failures == 0 ? "true" : "false", failures, jsonStr(full.res.encoder).c_str(), full.seconds,
              full.seconds > 0 ? double(full.res.frames) / full.seconds : 0.0, hex(full.res.rendererHash).c_str());
  return failures == 0 ? 0 : 1;
}

int exportCmd(const Args& a) {
  const char* project = a.get("--project");
  const char* out = a.get("--out");
  if (!project || !out) return usage();
  ExportOptions opt;
  opt.settings.start_frame = a.num("--start", 0);
  opt.settings.end_frame = a.num("--end", 0);
  if (const char* p = a.get("--preset")) {
    if (!rnf_export_preset_get(size_t(std::strtoul(p, nullptr, 10)), &opt.settings.preset)) {
      std::fprintf(stderr, "invalid --preset (see --list-presets)\n");
      return 2;
    }
  }
  const uint64_t flash = a.num("--flash", 0);
  if (flash > 3) return usage();
  opt.flash = rn_flash_level(flash);
  if (a.has("--par87")) opt.settings.pixel_aspect_87 = 1;
  if (a.has("--no-crop")) opt.settings.crop_top = opt.settings.crop_bottom = 0;
  if (const char* e = a.get("--encoder")) opt.encoder = e;
  if (const char* b = a.get("--bpp")) opt.videoBitsPerPixel = std::atof(b);
#ifdef RNL_EXPORT_CRT
  if (a.has("--crt")) {
    // "Apply CRT effect" (nesterm physical model, offline on a window-less Vulkan device).
    rnl::CrtSettings cs;
    if (const char* l = a.get("--crt-lines")) cs.lines = std::atoi(l);
    if (const char* d = a.get("--crt-dbuv")) cs.antennaDbuv = std::atof(d);
    if (a.has("--crt-no-growth")) cs.beamGrowth = false;
    if (a.has("--crt-no-persistence")) cs.persistence = false;
    if (a.has("--crt-no-supply")) cs.supply = false;
    opt.makeProcessor = rnl::crtExportProcessorFactory(cs);
  }
#endif

  rn_session_options so;
  rn_session_options_init(&so);
  rn_session* s = nullptr;
  rn_status st = rn_session_open(project, nullptr, &so, &s);
  if (st != RN_OK) {
    std::printf("{\"ok\":false,\"error\":%s}\n", jsonStr(std::string(rn_status_name(st)) + ": " + rn_last_error()).c_str());
    return 1;
  }
  RunOut o = runExport(s, opt, out, !a.has("--quiet"));
  std::string j = resultJson(o, out);
  if (o.ok && a.has("--verify-hash")) {
    uint64_t h = 0, n = 0;
    const bool same = freshHash(s, opt.settings.start_frame, opt.settings.end_frame, &h, &n) && h == o.res.rendererHash;
    j.pop_back();
    j += ",\"fresh_hash\":\"" + hex(h) + "\",\"hash_match\":" + (same ? "true" : "false") + "}";
    if (!same) o.ok = false;
  }
  std::printf("%s\n", j.c_str());
  rn_session_close(s);
  return o.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) a.v.emplace_back(argv[i]);
  av_log_set_level(a.has("--av-verbose") ? AV_LOG_INFO : AV_LOG_ERROR);
  if (a.has("--encoders")) {
    std::printf("%s\n", rnl::availableEncodersDescription().c_str());
    return 0;
  }
  if (a.has("--list-presets")) {
    for (size_t i = 0; i < rnf_export_preset_count(); ++i) {
      rnf_export_preset p{};
      rnf_export_preset_get(i, &p);
      char* l = rnf_export_preset_label(&p);
      std::printf("%zu  %s\n", i, l ? l : "");
      rnf_string_free(l);
    }
    return 0;
  }
  if (a.get("--self-test")) return selfTest(a);
  return exportCmd(a);
}
