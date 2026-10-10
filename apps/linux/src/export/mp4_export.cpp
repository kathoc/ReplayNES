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

constexpr int kFrameW = RN_VIDEO_WIDTH, kFrameH = RN_VIDEO_HEIGHT;
using exportframe::convertToYuv;
using exportframe::scaleNearest;
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

bool openVideoEncoder(const std::string& name, const rnf_export_geometry& g, int64_t bitrate, bool globalHeader,
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
  c->time_base = AVRational{RN_FPS_DEN, RN_FPS_NUM};  // one tick per frame
  c->framerate = AVRational{RN_FPS_NUM, RN_FPS_DEN};
  c->bit_rate = bitrate;
  // YouTube: closed GOP of half the frame rate. B-frames stay off (exact frame timestamps, no
  // reordering delay; also not offered by the AMD VAAPI H.264 encoder).
  c->gop_size = RNF_EXPORT_GOP_FRAMES;
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

bool openAudioEncoder(int bitrate, bool globalHeader, AvState& av, std::string* error) {
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
  c->sample_rate = RN_SAMPLE_RATE;
  av_channel_layout_default(&c->ch_layout, 1);
  c->bit_rate = bitrate;
  c->time_base = AVRational{1, RN_SAMPLE_RATE};
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
  char* message = nullptr;
  if (rnf_export_validate(&opt.settings, &message) != RN_OK) {
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
  rnf_export_geometry g{};
  rnf_export_geometry_compute(&opt.settings, &g);
  std::vector<int> cols(size_t(std::max(0, g.dst_width))), rows(size_t(std::max(0, g.dst_height)));
  rnf_export_column_map(&g, cols.data());
  rnf_export_row_map(&g, rows.data());
  const int64_t bitrate = rnf_export_video_bitrate(&g, 0, opt.quality);

  // ---- optional processor (CRT), flash filter
  std::unique_ptr<ExportVideoProcessor> processor;
  if (opt.makeProcessor) {
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
  std::unique_ptr<rn_flash_filter, void (*)(rn_flash_filter*)> flash(
      opt.flash == RN_FLASH_OFF ? nullptr : rn_flash_filter_new(opt.flash), rn_flash_filter_free);
  if (opt.flash != RN_FLASH_OFF && !flash) {
    err = "invalid flash reduction level";
    return false;
  }
  std::vector<uint32_t> filtered(flash ? size_t(kFrameW) * kFrameH : 0);

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
      if (openVideoEncoder(n, g, bitrate, globalHeader, av, &why)) {
        encName = n;
        break;
      }
      tried += (tried.empty() ? "" : "; ") + why;
    }
    if (!av.video) return fail("No usable H.264 encoder (" + tried + ")");
  }
  if (!openAudioEncoder(opt.audioBitrate, globalHeader, av, &err)) return fail(err);
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
  av.vStream->time_base = AVRational{1, RN_FPS_NUM};  // MP4 track timescale 39375000 (exact PTS)
  av.vStream->avg_frame_rate = AVRational{RN_FPS_NUM, RN_FPS_DEN};
  av.aStream->time_base = AVRational{1, RN_SAMPLE_RATE};

  r = avio_open(&av.fmt->pb, partPath.c_str(), AVIO_FLAG_WRITE);
  if (r < 0) return fail("Can't create " + outPath + ": " + avError(r));
  av.fileOpen = true;
  fileCreated = true;
  AVDictionary* muxOpts = nullptr;
  av_dict_set(&muxOpts, "video_track_timescale", std::to_string(RN_FPS_NUM).c_str(), 0);
  av_dict_set(&muxOpts, "movie_timescale", std::to_string(RN_FPS_NUM).c_str(), 0);
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
  av.aFrame->sample_rate = RN_SAMPLE_RATE;
  av.aFrame->nb_samples = frameSize;
  av_channel_layout_default(&av.aFrame->ch_layout, 1);
  if (av_frame_get_buffer(av.aFrame, 0) < 0) return fail("out of memory");
  const bool smallLastFrame = (av.audio->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;

  const size_t canvasStride = size_t(g.canvas_width) * 4;
  std::vector<uint8_t> canvas(canvasStride * size_t(g.canvas_height));
  const uint64_t startFrame = opt.settings.start_frame;
  const uint64_t sampleBase = rn_audio_samples_before(startFrame);
  std::vector<float> pending;  // PCM not yet in an AAC frame
  pending.reserve(size_t(frameSize) * 4);
  uint64_t samplesQueued = 0;  // samples appended to `pending` since the start (== next sample position)
  uint64_t samplesEncoded = 0;
  uint64_t framesWritten = 0;

  auto sendAudio = [&](int count) -> bool {
    if (av_frame_make_writable(av.aFrame) < 0) return fail("out of memory");
    av.aFrame->nb_samples = count;
    std::memcpy(av.aFrame->data[0], pending.data(), size_t(count) * sizeof(float));
    if (count < frameSize && !smallLastFrame) {
      std::memset(reinterpret_cast<float*>(av.aFrame->data[0]) + count, 0, size_t(frameSize - count) * sizeof(float));
      av.aFrame->nb_samples = frameSize;
    }
    av.aFrame->pts = int64_t(samplesEncoded);
    pending.erase(pending.begin(), pending.begin() + count);
    samplesEncoded += uint64_t(count);
    if (!encodeAndWrite(av, av.audio, av.aStream, av.aFrame, &err)) return fail(err);
    return true;
  };

  for (;;) {
    if (cancelled && cancelled()) return fail("cancelled");
    const uint32_t* video = nullptr;
    const int16_t* audio = nullptr;
    size_t n = 0;
    uint64_t f = 0;
    rn_status st = rn_renderer_next(rend.get(), &video, &audio, &n, &f);
    if (st == RN_ERR_END_OF_TAKE) break;
    if (st != RN_OK) return fail(std::string(rn_status_name(st)) + ": " + rn_last_error());
    if (!video) return fail("the renderer returned no picture");

    // Flash reduction: frames strictly in order through one filter (display only).
    const uint32_t* pixels = video;
    bool altered = false;
    if (flash) {
      rn_flash_info info{};
      if (rn_flash_filter_process(flash.get(), video, filtered.data(), &info) != RN_OK)
        return fail(std::string("flash filter: ") + rn_last_error());
      pixels = filtered.data();
      altered = info.altered != 0;
    }

    if (processor) {
      rn_video_indices_info info{};
      ExportFrameSignal sig;
      if (rn_renderer_video_indices(rend.get(), &info) == RN_OK && info.codes) {
        sig.codes = info.codes;
        sig.burstPhase = info.burst_phase;
        sig.ordinal = info.frame;
      } else {
        sig.ordinal = f;
      }
      sig.flashAltered = altered;
      std::string perr;
      if (!processor->render(sig, pixels, canvas.data(), canvasStride, &perr))
        return fail(perr.empty() ? "The video effect failed" : perr);
    } else {
      scaleNearest(pixels, canvas.data(), canvasStride, g, cols, rows);
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
    // Video PTS = frame offset in ticks of 655171/39375000 s (exact rational).
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

    // Audio: sample position from the absolute count at 48 kHz. The renderer's PCM is contiguous;
    // a gap would be filled with silence and an overlap dropped so A/V never drift.
    if (n > 0 && audio) {
      const uint64_t at = rn_audio_samples_before(f) - sampleBase;
      size_t skip = 0;
      if (at > samplesQueued) {
        pending.insert(pending.end(), size_t(at - samplesQueued), 0.0f);
        samplesQueued = at;
      } else if (at < samplesQueued) {
        skip = size_t(std::min<uint64_t>(samplesQueued - at, n));
      }
      for (size_t i = skip; i < n; ++i) pending.push_back(float(audio[i]) / 32768.0f);
      samplesQueued += uint64_t(n - skip);
      while (pending.size() >= size_t(frameSize))
        if (!sendAudio(frameSize)) return false;
    }
    if (progress) progress(rn_renderer_frames_done(rend.get()), total);
  }

  // ---- flush
  if (!pending.empty() && !sendAudio(int(pending.size()))) return false;
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
    result->duration = double(framesWritten) * RN_FPS_DEN / RN_FPS_NUM;
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
