// Offline MP4 export (H.264 + AAC via FFmpeg libav*), see mp4_export.h.
// Written against the libav* APIs present in both FFmpeg 7.1 (libavcodec 61, freedesktop 25.08)
// and FFmpeg 8 (libavcodec 62): AVChannelLayout, send/receive, AVFrame.duration, AV_PROFILE_*.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "mp4_export.h"

#include "export_frame.h"
#include "mp4_check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace rnl {

namespace {

using exportframe::convertToYuv;
using exportframe::scaleNearestFrame;
// Hardware first (VAAPI: the Steam Deck's AMD GPU, Intel / AMD desktops; needs /dev/dri), then
// x264, then OpenH264.
const char* const kAutoEncoders[] = {"h264_vaapi", "libx264", "libopenh264"};

std::string avError(int code) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buf, sizeof buf);
  return buf;
}

// ------------------------------------------------------------------ libav state (RAII)
struct AvState {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* video = nullptr;
  AVCodecContext* audio = nullptr;
  AVBufferRef* hwDevice = nullptr;
  AVStream* vStream = nullptr;
  AVStream* aStream = nullptr;
  AVFrame* vFrame = nullptr;  // YUV420P, or NV12 (software side of a VAAPI upload)
  AVFrame* aFrame = nullptr;
  AVPacket* pkt = nullptr;
  bool vaapi = false;
  bool fileOpen = false;

  ~AvState() {
    av_packet_free(&pkt);
    av_frame_free(&vFrame);
    av_frame_free(&aFrame);
    avcodec_free_context(&video);
    avcodec_free_context(&audio);
    av_buffer_unref(&hwDevice);
    if (fmt) {
      if (fileOpen) avio_closep(&fmt->pb);
      avformat_free_context(fmt);
    }
  }
};

bool openVideoEncoder(const std::string& name, const rnf_export_geometry& g, const rnf_export_timing& timing, int64_t bitrate, bool globalHeader,
                      AvState& av, std::string* why) {
  const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) {
    *why = name + ": not in this FFmpeg build";
    return false;
  }
  if (codec->id != AV_CODEC_ID_H264) {
    *why = name + ": not an H.264 encoder";
    return false;
  }
  AVCodecContext* c = avcodec_alloc_context3(codec);
  if (!c) {
    *why = "out of memory";
    return false;
  }
  const bool vaapi = name.find("vaapi") != std::string::npos;
  c->width = g.canvas_width;
  c->height = g.canvas_height;
  c->sample_aspect_ratio = AVRational{1, 1};
  c->time_base = AVRational{int(timing.frame_duration), int(timing.timescale)};  // one tick per frame
  c->framerate = AVRational{int(timing.timescale), int(timing.frame_duration)};
  c->bit_rate = bitrate;
  // YouTube: closed GOP of half the frame rate. B-frames stay off (exact frame timestamps, no
  // reordering delay; also not offered by the AMD VAAPI H.264 encoder).
  c->gop_size = int(timing.gop_frames);
  c->max_b_frames = 0;
  // Average bit rate, capped: peaks up to 1.5x the target through a VBV buffer of 2 s at the target
  // rate. No minimum and no filler: a simple picture (flat pixel art, a still) takes fewer bits and
  // gives a smaller file; the Export dialog's size (rnf_export_predict_size) is an estimate. VAAPI
  // picks VBR from max > average.
  c->rc_min_rate = 0;
  c->rc_max_rate = bitrate * 3 / 2;
  c->rc_buffer_size = int(std::min<int64_t>(bitrate * 2, INT32_MAX));
  c->color_primaries = AVCOL_PRI_BT709;
  c->color_trc = AVCOL_TRC_BT709;
  c->colorspace = AVCOL_SPC_BT709;
  c->color_range = AVCOL_RANGE_MPEG;
  c->chroma_sample_location = AVCHROMA_LOC_CENTER;  // chroma = mean of each 2x2 block
  if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (name == "libx264") {
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->profile = AV_PROFILE_H264_HIGH;
    av_opt_set(c->priv_data, "profile", "high", 0);
    av_opt_set(c->priv_data, "preset", "medium", 0);
    // Closed GOP, CABAC (High); ABR with the VBV cap above (no nal-hrd, so no filler data).
    av_opt_set(c->priv_data, "x264-params", "open-gop=0:cabac=1", 0);
  } else if (vaapi) {
    c->profile = AV_PROFILE_H264_HIGH;
    int r = av_hwdevice_ctx_create(&av.hwDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (r < 0) {
      *why = name + ": no VAAPI device (" + avError(r) + ")";
      avcodec_free_context(&c);
      return false;
    }
    AVBufferRef* frames = av_hwframe_ctx_alloc(av.hwDevice);
    if (!frames) {
      *why = "out of memory";
      av_buffer_unref(&av.hwDevice);
      avcodec_free_context(&c);
      return false;
    }
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = g.canvas_width;
    fc->height = g.canvas_height;
    fc->initial_pool_size = 20;
    r = av_hwframe_ctx_init(frames);
    if (r < 0) {
      *why = name + ": VAAPI frames (" + avError(r) + ")";
      av_buffer_unref(&frames);
      av_buffer_unref(&av.hwDevice);
      avcodec_free_context(&c);
      return false;
    }
    c->pix_fmt = AV_PIX_FMT_VAAPI;
    c->hw_frames_ctx = frames;  // takes the reference
  } else {
    c->pix_fmt = AV_PIX_FMT_YUV420P;  // libopenh264 & co.: their default profile
  }
  int r = avcodec_open2(c, codec, nullptr);
  if (r < 0) {
    *why = name + ": " + avError(r);
    avcodec_free_context(&c);
    av_buffer_unref(&av.hwDevice);
    return false;
  }
  av.video = c;
  av.vaapi = vaapi;
  return true;
}

bool openAudioEncoder(int bitrate, const rnf_export_timing& timing, bool globalHeader, AvState& av, std::string* error) {
  const AVCodec* codec = avcodec_find_encoder_by_name("aac");  // FFmpeg's native AAC encoder
  if (!codec) {
    *error = "The AAC encoder is missing from this FFmpeg build";
    return false;
  }
  AVCodecContext* c = avcodec_alloc_context3(codec);
  if (!c) {
    *error = "out of memory";
    return false;
  }
  c->sample_fmt = AV_SAMPLE_FMT_FLTP;
  // AAC accepts discrete rates. Preserve a supported core rate and resample otherwise.
  static constexpr int rates[] = {7350, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000};
  c->sample_rate = *std::min_element(std::begin(rates), std::end(rates), [&](int a, int b) {
    return std::abs(a - timing.audio_sample_rate) < std::abs(b - timing.audio_sample_rate);
  });
  av_channel_layout_default(&c->ch_layout, int(timing.audio_channels));
  c->bit_rate = bitrate * timing.audio_channels;
  c->time_base = AVRational{1, c->sample_rate};
  if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  int r = avcodec_open2(c, codec, nullptr);
  if (r < 0) {
    *error = "AAC encoder: " + avError(r);
    avcodec_free_context(&c);
    return false;
  }
  av.audio = c;
  return true;
}

/// Sends `frame` (nullptr = flush) and writes every packet the encoder returns.
bool encodeAndWrite(AvState& av, AVCodecContext* c, AVStream* st, AVFrame* frame, std::string* error) {
  int r = avcodec_send_frame(c, frame);
  if (r < 0 && !(frame == nullptr && r == AVERROR_EOF)) {
    *error = std::string("encoder (") + c->codec->name + "): " + avError(r);
    return false;
  }
  for (;;) {
    r = avcodec_receive_packet(c, av.pkt);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
    if (r < 0) {
      *error = std::string("encoder (") + c->codec->name + "): " + avError(r);
      return false;
    }
    if (c == av.video && av.pkt->duration <= 0) av.pkt->duration = 1;  // one frame
    av_packet_rescale_ts(av.pkt, c->time_base, st->time_base);
    av.pkt->stream_index = st->index;
    r = av_interleaved_write_frame(av.fmt, av.pkt);  // takes the packet's reference
    if (r < 0) {
      *error = "writing the file: " + avError(r);
      return false;
    }
  }
}

}  // namespace

const std::vector<ExportEncoderChoice>& exportEncoderChoices() {
  static const std::vector<ExportEncoderChoice> choices = {{"libx264", "libx264"}, {"h264_vaapi", "h264_vaapi"},
                                                           {"libopenh264", "libopenh264"}};
  return choices;
}

std::string availableEncodersDescription() {
  std::string out;
  for (const char* n : kAutoEncoders) {
    if (!out.empty()) out += ", ";
    out += n;
    out += avcodec_find_encoder_by_name(n) ? " (present)" : " (missing)";
  }
  out += avcodec_find_encoder_by_name("aac") ? "; aac (present)" : "; aac (missing)";
  return out;
}

namespace {

/// One export pass with `renderer` (not owned); softwareOnly: auto mode without VAAPI. *hardwareFailed: a hardware (VAAPI) encoder was
/// used and the failure came from encoding / finishing / the self-check (worth a software retry).
bool exportOnce(rn_renderer* renderer, const ExportOptions& opt, const std::string& outPath, const ExportProgressFn& progress,
                const ExportCancelFn& cancelled, ExportResult* result, std::string* error, bool* hardwareFailed,
                bool softwareOnly) {
  std::string localError;
  std::string& err = error ? *error : localError;
  err.clear();
  *hardwareFailed = false;
  std::unique_ptr<rn_renderer, void (*)(rn_renderer*)> rend(renderer, [](rn_renderer*) {});
  if (!rend) {
    err = "no renderer";
    return false;
  }

  // ---- settings + geometry (shared frontend core)
  rn_media_info media{};
  rnf_export_timing timing{};
  if (rn_renderer_media_info(renderer, &media) != RN_OK ||
      rnf_export_timing_compute(&media, opt.audioSampleRate, &timing) != RN_OK ||
      timing.timescale > INT32_MAX || timing.frame_duration > INT32_MAX) {
    err = "invalid export media or unsupported MP4 timescale";
    return false;
  }
  char* message = nullptr;
  if (rnf_export_validate_media(&opt.settings, &media, 0, 0, &message) != RN_OK) {
    err = message ? message : "invalid export settings";
    rnf_string_free(message);
    return false;
  }
  rnf_string_free(message);
  const uint64_t total = rn_renderer_total_frames(rend.get());
  if (total == 0) {
    err = "There are no frames to export (the take is empty)";
    return false;
  }
  const uint32_t* video = nullptr;
  const int16_t* audio = nullptr;
  size_t n = 0;
  uint64_t f = 0;
  rn_video_frame_info picture{};
  rn_status first = rn_renderer_next(renderer, &video, &audio, &n, &f);
  if (first != RN_OK || rn_renderer_video_frame(renderer, &picture) != RN_OK || !picture.pixels) {
    err = "the renderer returned no picture";
    return false;
  }
  if (rnf_export_validate_media(&opt.settings, &media, picture.width, picture.height, &message) != RN_OK) {
    err = message ? message : "invalid export settings";
    rnf_string_free(message);
    return false;
  }
  rnf_export_geometry g{};
  rnf_export_geometry_compute_media(&opt.settings, &media, picture.width, picture.height, &g);
  const int canvasWidth = g.canvas_width, canvasHeight = g.canvas_height;
  uint32_t pictureWidth = picture.width, pictureHeight = picture.height;
  rn_video_indices_info firstCodes{};
  const bool crtPicture = picture.width == RN_VIDEO_WIDTH && picture.height == RN_VIDEO_HEIGHT &&
      media.rotation == 0 && rn_renderer_video_indices(renderer, &firstCodes) == RN_OK && firstCodes.codes;
  const int64_t bitrate = rnf_export_video_bitrate(&g, 0, opt.quality);

  // ---- optional processor (CRT), flash filter
  std::unique_ptr<ExportVideoProcessor> processor;
  if (opt.makeProcessor && crtPicture) {
    std::string perr;
    processor = opt.makeProcessor(&perr);
    if (!processor) {
      err = perr.empty() ? "The video effect could not be created" : perr;
      return false;
    }
    if (!processor->begin(opt.settings, g, &perr)) {
      err = perr.empty() ? "The video effect could not be started" : perr;
      return false;
    }
  }
#if RN_PRODUCT_FLASH_FILTER
  std::unique_ptr<rn_flash_filter, void (*)(rn_flash_filter*)> flash(
      opt.flash == RN_FLASH_OFF ? nullptr : rn_flash_filter_new_sized(opt.flash, picture.width, picture.height), rn_flash_filter_free);
  if (opt.flash != RN_FLASH_OFF && !flash) {
    err = "invalid flash reduction level";
    return false;
  }
#else
  constexpr bool flash = false;
#endif
  std::vector<uint32_t> filtered(flash ? size_t(media.max_width) * media.max_height : 0);
  std::vector<uint16_t> filteredCodes(flash ? size_t(media.max_width) * media.max_height : 0);
  // The CRT effect starts with the power-on intro (and its hiss).
  std::unique_ptr<ExportCrtIntro> intro(processor ? new ExportCrtIntro(RN_PRODUCT_FLASH_FILTER && opt.flash != RN_FLASH_OFF) : nullptr);

  // ---- muxer + encoders
  // Written under a temporary name and renamed once finished and checked (a failed / cancelled
  // export never leaves a broken file under the chosen name, nor replaces an existing one).
  const std::string partPath = outPath + ".part";
  AvState av;
  bool fileCreated = false;
  auto fail = [&](const std::string& m) {
    err = m;
    if (av.fileOpen) {
      avio_closep(&av.fmt->pb);
      av.fileOpen = false;
    }
    if (fileCreated) std::remove(partPath.c_str());
    return false;
  };

  int r = avformat_alloc_output_context2(&av.fmt, nullptr, "mp4", partPath.c_str());
  if (r < 0 || !av.fmt) return fail("MP4 muxer: " + avError(r));
  const bool globalHeader = (av.fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;

  std::string encName;
  {
    std::string tried;
    std::vector<std::string> names;
    if (opt.encoder.empty()) {
      for (const char* n : kAutoEncoders)
        if (!softwareOnly || std::string(n).find("vaapi") == std::string::npos) names.push_back(n);
    } else {
      names.push_back(opt.encoder);
    }
    for (const auto& n : names) {
      std::string why;
      if (openVideoEncoder(n, g, timing, bitrate, globalHeader, av, &why)) {
        encName = n;
        break;
      }
      tried += (tried.empty() ? "" : "; ") + why;
    }
    if (!av.video) return fail("No usable H.264 encoder (" + tried + ")");
  }
  if (!openAudioEncoder(opt.audioBitrate, timing, globalHeader, av, &err)) return fail(err);
  const bool usedHardware = av.vaapi;
  auto failEncoder = [&](const std::string& m) {  // encoder / muxer / file trouble (not the renderer's)
    *hardwareFailed = usedHardware;
    return fail(m);
  };

  av.vStream = avformat_new_stream(av.fmt, nullptr);
  av.aStream = avformat_new_stream(av.fmt, nullptr);
  if (!av.vStream || !av.aStream) return fail("out of memory");
  if (avcodec_parameters_from_context(av.vStream->codecpar, av.video) < 0 ||
      avcodec_parameters_from_context(av.aStream->codecpar, av.audio) < 0)
    return fail("codec parameters");
  av.vStream->time_base = AVRational{1, int(timing.timescale)};  // exact MP4 frame timestamps
  av.vStream->avg_frame_rate = AVRational{int(timing.timescale), int(timing.frame_duration)};
  av.aStream->time_base = av.audio->time_base;

  r = avio_open(&av.fmt->pb, partPath.c_str(), AVIO_FLAG_WRITE);
  if (r < 0) return fail("Can't create " + outPath + ": " + avError(r));
  av.fileOpen = true;
  fileCreated = true;
  AVDictionary* muxOpts = nullptr;
  av_dict_set(&muxOpts, "video_track_timescale", std::to_string(timing.timescale).c_str(), 0);
  av_dict_set(&muxOpts, "movie_timescale", std::to_string(timing.timescale).c_str(), 0);
  r = avformat_write_header(av.fmt, &muxOpts);
  av_dict_free(&muxOpts);
  if (r < 0) return fail("MP4 header: " + avError(r));

  // ---- frames
  av.pkt = av_packet_alloc();
  av.vFrame = av_frame_alloc();
  av.aFrame = av_frame_alloc();
  if (!av.pkt || !av.vFrame || !av.aFrame) return fail("out of memory");
  av.vFrame->format = av.vaapi ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  av.vFrame->width = g.canvas_width;
  av.vFrame->height = g.canvas_height;
  if (av_frame_get_buffer(av.vFrame, 0) < 0) return fail("out of memory");
  const int frameSize = av.audio->frame_size > 0 ? av.audio->frame_size : 1024;
  av.aFrame->format = AV_SAMPLE_FMT_FLTP;
  av.aFrame->sample_rate = av.audio->sample_rate;
  av.aFrame->nb_samples = frameSize;
  av_channel_layout_default(&av.aFrame->ch_layout, int(media.audio_channels));
  if (av_frame_get_buffer(av.aFrame, 0) < 0) return fail("out of memory");
  const bool smallLastFrame = (av.audio->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;

  const size_t canvasStride = size_t(g.canvas_width) * 4;
  std::vector<uint8_t> canvas(canvasStride * size_t(g.canvas_height));
  const uint64_t startFrame = opt.settings.start_frame;
  const size_t channels = media.audio_channels;
  const double resampleRatio = double(av.audio->sample_rate) / timing.audio_sample_rate;
  const bool needsResample = resampleRatio != 1.0;
  if (needsResample && (resampleRatio < 0.5 || resampleRatio > 2.0)) return fail("unsupported AAC resampling ratio");
  std::unique_ptr<rnf_resampler, void (*)(rnf_resampler*)> resampler(
      needsResample ? rnf_resampler_new_with_channels(media.audio_channels) : nullptr, rnf_resampler_free);
  if (needsResample && !resampler) return fail("out of memory");
  std::vector<int16_t> resampled;
  uint64_t inputSamples = 0;
  std::vector<float> pending;  // PCM not yet in an AAC frame
  pending.reserve(size_t(frameSize) * 4);
  uint64_t samplesQueued = 0;  // samples appended to `pending` since the start (== next sample position)
  uint64_t samplesEncoded = 0;
  uint64_t framesWritten = 0;

  auto sendAudio = [&](int count) -> bool {
    if (av_frame_make_writable(av.aFrame) < 0) return fail("out of memory");
    av.aFrame->nb_samples = count;
    for (size_t ch = 0; ch < channels; ++ch) {
      auto* plane = reinterpret_cast<float*>(av.aFrame->extended_data[ch]);
      for (int i = 0; i < count; ++i) plane[i] = pending[size_t(i) * channels + ch];
      if (count < frameSize && !smallLastFrame) std::fill(plane + count, plane + frameSize, 0.0f);
    }
    if (count < frameSize && !smallLastFrame) av.aFrame->nb_samples = frameSize;
    av.aFrame->pts = int64_t(samplesEncoded);
    pending.erase(pending.begin(), pending.begin() + size_t(count) * channels);
    samplesEncoded += uint64_t(count);
    if (!encodeAndWrite(av, av.audio, av.aStream, av.aFrame, &err)) return fail(err);
    return true;
  };

  for (;;) {
    if (cancelled && cancelled()) return fail("cancelled");
    if (framesWritten) {
      rn_status st = rn_renderer_next(rend.get(), &video, &audio, &n, &f);
      if (st == RN_ERR_END_OF_TAKE) break;
      if (st != RN_OK) return fail(std::string(rn_status_name(st)) + ": " + rn_last_error());
      if (rn_renderer_video_frame(renderer, &picture) != RN_OK || !picture.pixels)
        return fail("the renderer returned no picture");
    }
    if (picture.width != pictureWidth || picture.height != pictureHeight) {
#if RN_PRODUCT_FLASH_FILTER
      if (flash && rn_flash_filter_set_size(flash.get(), picture.width, picture.height) != RN_OK)
        return fail("unsupported flash filter picture size");
#endif
      pictureWidth = picture.width;
      pictureHeight = picture.height;
      if (rnf_export_validate_media(&opt.settings, &media, pictureWidth, pictureHeight, nullptr) != RN_OK)
        return fail("invalid crop for the current picture size");
      auto settings = opt.settings;
      settings.preset.kind = RNF_PRESET_CANVAS;
      settings.preset.width = canvasWidth;
      settings.preset.height = canvasHeight;
      rnf_export_geometry_compute_media(&settings, &media, pictureWidth, pictureHeight, &g);
    }
    rn_video_indices_info frameCodes{};
    const bool useProcessor = processor && picture.width == RN_VIDEO_WIDTH && picture.height == RN_VIDEO_HEIGHT &&
        rn_renderer_video_indices(renderer, &frameCodes) == RN_OK && frameCodes.codes;

    // Flash reduction: frames strictly in order through one filter (display only).
    const uint32_t* pixels = picture.pixels;
    bool altered = false, codesOk = false;
#if RN_PRODUCT_FLASH_FILTER
    if (flash) {
      rn_flash_info info{};
      // With a CRT processor the PPU codes go through the filter too (the RF path stays in use).
      rn_video_indices_info vi{};
      if (useProcessor && picture.stride == RN_VIDEO_WIDTH && rn_renderer_video_indices(rend.get(), &vi) == RN_OK && vi.codes) {
        int ok = 0;
        if (rn_flash_filter_process_codes(flash.get(), video, filtered.data(), vi.codes, filteredCodes.data(), &info, &ok) != RN_OK)
          return fail(std::string("flash filter: ") + rn_last_error());
        codesOk = ok != 0;
      } else if (rn_flash_filter_process_frame(flash.get(), &picture, filtered.data(), &info) != RN_OK) {
        return fail(std::string("flash filter: ") + rn_last_error());
      }
      pixels = filtered.data();
      altered = info.altered != 0;
    }
#endif

    if (useProcessor) {
      rn_video_indices_info info{};
      ExportFrameSignal sig;
      if (rn_renderer_video_indices(rend.get(), &info) == RN_OK && info.codes) {
        sig.codes = codesOk ? filteredCodes.data() : info.codes;
        sig.burstPhase = info.burst_phase;
        sig.ordinal = info.frame;
      } else {
        sig.ordinal = f;
      }
      sig.flashAltered = altered && !codesOk;  // the codes do not describe the filtered picture
      intro->frame(&sig);
      std::string perr;
      if (!processor->render(sig, pixels, canvas.data(), canvasStride, &perr))
        return fail(perr.empty() ? "The video effect failed" : perr);
    } else {
      auto shown = picture;
      shown.pixels = pixels;
      if (flash) shown.stride = picture.width;
      scaleNearestFrame(shown, canvas.data(), canvasStride, g);
    }

    if (av_frame_make_writable(av.vFrame) < 0) return fail("out of memory");
    AVFrame* fr = av.vFrame;
    if (av.vaapi) {
      convertToYuv(canvas.data(), canvasStride, g.canvas_width, g.canvas_height, fr->data[0], fr->linesize[0],
                   fr->data[1], fr->linesize[1], nullptr, 0);
    } else {
      convertToYuv(canvas.data(), canvasStride, g.canvas_width, g.canvas_height, fr->data[0], fr->linesize[0],
                   fr->data[1], fr->linesize[1], fr->data[2], fr->linesize[2]);
    }
    // Video PTS = frame offset in the exact media frame period.
    const int64_t pts = int64_t(f - startFrame);
    if (av.vaapi) {
      AVFrame* hw = av_frame_alloc();
      if (!hw) return fail("out of memory");
      r = av_hwframe_get_buffer(av.video->hw_frames_ctx, hw, 0);
      if (r >= 0) r = av_hwframe_transfer_data(hw, fr, 0);
      if (r < 0) {
        av_frame_free(&hw);
        return failEncoder("VAAPI upload: " + avError(r));
      }
      hw->pts = pts;
      hw->duration = 1;
      const bool ok = encodeAndWrite(av, av.video, av.vStream, hw, &err);
      av_frame_free(&hw);
      if (!ok) return failEncoder(err);
    } else {
      fr->pts = pts;
      fr->duration = 1;
      if (!encodeAndWrite(av, av.video, av.vStream, fr, &err)) return failEncoder(err);
    }
    ++framesWritten;
    if (opt.simulateHardwareFailureAfter && framesWritten >= opt.simulateHardwareFailureAfter) {
      fail("simulated hardware encoder failure");
      *hardwareFailed = true;
      return false;
    }

    // The renderer emits contiguous sample frames; channel counts never advance timestamps.
    if (n > 0 && audio) {
      if (useProcessor && channels == 1) audio = intro->mixAudio(audio, n);
      inputSamples += n;
      size_t outCount = n;
      if (resampler) {
        const size_t capacity = rnf_resampler_output_bound(n);
        resampled.resize(capacity * channels);
        if (rnf_resampler_process(resampler.get(), audio, n, resampleRatio, resampled.data(), capacity, &outCount) != RN_OK)
          return fail("audio resampling failed");
        audio = resampled.data();
      }
      for (size_t i = 0; i < outCount * channels; ++i) pending.push_back(float(audio[i]) / 32768.0f);
      samplesQueued += outCount;
      while (pending.size() >= size_t(frameSize) * channels)
        if (!sendAudio(frameSize)) return false;
    }
    if (progress) progress(rn_renderer_frames_done(rend.get()), total);
  }

  // ---- flush
  if (resampler) {
    const uint64_t target = uint64_t(std::llround(double(inputSamples) * resampleRatio));
    // Complete the resampler's sub-sample tail without extending the soundtrack.
    if (target > samplesQueued) pending.insert(pending.end(), size_t(target - samplesQueued) * channels, 0.0f);
    else if (target < samplesQueued && samplesQueued - target <= pending.size() / channels)
      pending.resize(pending.size() - size_t(samplesQueued - target) * channels);
    samplesQueued = target;
    while (pending.size() >= size_t(frameSize) * channels)
      if (!sendAudio(frameSize)) return false;
  }
  if (!pending.empty() && !sendAudio(int(pending.size() / channels))) return false;
  if (!encodeAndWrite(av, av.video, av.vStream, nullptr, &err)) return failEncoder(err);
  if (!encodeAndWrite(av, av.audio, av.aStream, nullptr, &err)) return failEncoder(err);
  r = av_write_trailer(av.fmt);
  if (r < 0) return failEncoder("finishing the file: " + avError(r));
  r = avio_closep(&av.fmt->pb);
  av.fileOpen = false;
  if (r < 0) return fail("closing the file: " + avError(r));
  {
    std::string why;
    if (!verifyExportedMp4(partPath, framesWritten, &why)) return failEncoder(why);
  }
  if (std::rename(partPath.c_str(), outPath.c_str()) != 0) return fail("saving the file under its name: " + std::string(std::strerror(errno)));

  if (result) {
    result->frames = framesWritten;
    result->audioSamples = samplesQueued;
    result->rendererHash = rn_renderer_hash(rend.get());
    result->duration = double(framesWritten) * rnf_frame_period_for_media(&media);
    result->encoder = encName;
    result->hardware = usedHardware;
  }
  return true;
}

}  // namespace

bool exportProject(rn_renderer* renderer, const ExportOptions& opt, const std::string& outPath,
                   const ExportProgressFn& progress, const ExportCancelFn& cancelled, ExportResult* result,
                   std::string* error) {
  std::unique_ptr<rn_renderer, void (*)(rn_renderer*)> rend(renderer, rn_renderer_free);
  std::string localError;
  std::string& err = error ? *error : localError;
  if (!rend) {
    err = "no renderer";
    return false;
  }
  bool hardwareFailed = false;
  if (exportOnce(rend.get(), opt, outPath, progress, cancelled, result, &err, &hardwareFailed, false)) return true;
  // Automatic encoder choice: a hardware encoder that failed while encoding, finishing or in the
  // self-check gets one retry with the software encoders (the same picture, timing and settings).
  if (!hardwareFailed || !opt.encoder.empty() || !opt.retryRenderer || (cancelled && cancelled())) return false;
  const std::string hwError = err;
  ExportOptions sw = opt;
  sw.retryRenderer.reset();
  sw.simulateHardwareFailureAfter = 0;
  if (!exportOnce(opt.retryRenderer.get(), sw, outPath, progress, cancelled, result, &err, &hardwareFailed, true)) {
    if (err != "cancelled") err = "Hardware encoder: " + hwError + "; software encoder: " + err;
    return false;
  }
  if (result) result->encoder += " - the hardware encoder failed (" + hwError + ")";
  return true;
}

bool verifyExportedMp4(const std::string& path, uint64_t frames, std::string* error) {
  std::string localError;
  std::string& err = error ? *error : localError;
  Mp4Summary sum;
  if (!checkMp4File(path, &sum, &err)) return false;
  for (const auto& t : sum.tracks)
    if (t.handler == "vide") {
      if (frames && t.samples != frames) {
        err = "MP4 check: " + std::to_string(t.samples) + " video frames, expected " + std::to_string(frames);
        return false;
      }
      return t.samples > 0 || (err = "MP4 check: no video frames", false);
    }
  err = "MP4 check: no video track";
  return false;
}

}  // namespace rnl
