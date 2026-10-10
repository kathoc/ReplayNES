// Offline MP4 export on Windows (export/mp4_export.h) through Media Foundation's sink writer:
// H.264 (a hardware MFT when the system has one - MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS - else
// Microsoft's software encoder) + AAC-LC 48 kHz mono, MPEG-4 container. Same pipeline as the
// Linux FFmpeg exporter: an rn_renderer from the engine (fresh core, logical time), the shared
// export geometry / presets / validation (frontend core), nearest scaling or the CRT processor,
// the optional flash filter, and the exact BT.709 limited-range NV12 conversion (export_frame.h).
// Timestamps come only from frame / sample counts: video frame n starts at n * 655171 / 39375000 s,
// audio sample n at n / 48000 s, both in Media Foundation's 100 ns units (export_frame.h); the
// finished file's video track is then given the exact timescale 39375000 / 655171 ticks per frame
// (mp4_retime.h: the sink rounds it to fps x 1000). No B-frames (no reordering delay).
// The file is written as "<name>.part", self-checked (mp4_check.h + Media Foundation decodes its
// first and last frames) and only then renamed: an export that dies before Finalize can't leave an
// unplayable .mp4 (no moov) under the chosen name. Never touches the session / project; a failed
// or cancelled export removes the partial file.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "export_frame.h"
#include "mp4_check.h"
#include "mp4_export.h"
#include "mp4_retime.h"

namespace rnl {

namespace {

constexpr int kFrameW = RN_VIDEO_WIDTH, kFrameH = RN_VIDEO_HEIGHT;

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(size_t(std::max(0, n - 1)), L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(std::max(0, n - 1)), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string hrText(const char* what, HRESULT h) {
  char b[200];
  std::snprintf(b, sizeof b, "%s failed (0x%08lx)", what, (unsigned long)h);
  return b;
}

/// COM (multithreaded) + Media Foundation for the export thread.
struct MfScope {
  bool com = false, mf = false;
  HRESULT init() {
    HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com = SUCCEEDED(h);  // S_FALSE: already initialised here (balanced by CoUninitialize too)
    if (FAILED(h) && h != RPC_E_CHANGED_MODE) return h;
    h = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    mf = SUCCEEDED(h);
    return h;
  }
  ~MfScope() {
    if (mf) MFShutdown();
    if (com) CoUninitialize();
  }
};

/// AAC-LC bit rates the Microsoft AAC encoder accepts (bytes per second).
UINT32 aacBytesPerSecond(int bitsPerSecond) {
  const UINT32 allowed[] = {12000, 16000, 20000, 24000};
  UINT32 best = allowed[0];
  for (UINT32 a : allowed)
    if (std::llabs(int64_t(a) * 8 - bitsPerSecond) < std::llabs(int64_t(best) * 8 - bitsPerSecond)) best = a;
  return best;
}

void setColorTags(IMFMediaType* t) {
  t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
  t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
  t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
  t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  t->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_MPEG1);  // centred (mean of 2x2)
}

struct Writer {
  IMFSinkWriter* sink = nullptr;
  IMFDXGIDeviceManager* dxgi = nullptr;  // the hardware encoder's Direct3D 11 device (may be null)
  DWORD video = 0, audio = 0;
  bool hardware = false;
  std::string encoderName;
  ~Writer() {
    release(sink);
    release(dxgi);
  }
};

/// A Direct3D 11 device (video support, multithread protected) in a DXGI device manager for the
/// sink writer: hardware encoder MFTs then get the frames uploaded to the GPU by Media Foundation
/// (some only accept Direct3D surfaces). Null when there is no hardware device.
IMFDXGIDeviceManager* createDxgiManager() {
  ID3D11Device* dev = nullptr;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                               UINT(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
    return nullptr;
  ID3D10Multithread* mt = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) {
    mt->SetMultithreadProtected(TRUE);
    mt->Release();
  }
  UINT token = 0;
  IMFDXGIDeviceManager* m = nullptr;
  if (FAILED(MFCreateDXGIDeviceManager(&token, &m)) || FAILED(m->ResetDevice(dev, token))) release(m);
  dev->Release();
  return m;
}

/// The H.264 encoder the sink writer loaded for the video stream: its name and whether it is a
/// hardware MFT.
void identifyEncoder(Writer& w) {
  w.encoderName = "Media Foundation H.264";
  IMFSinkWriterEx* ex = nullptr;
  if (FAILED(w.sink->QueryInterface(__uuidof(IMFSinkWriterEx), reinterpret_cast<void**>(&ex)))) return;
  for (DWORD i = 0;; ++i) {
    GUID category{};
    IMFTransform* t = nullptr;
    if (FAILED(ex->GetTransformForStream(w.video, i, &category, &t))) break;
    if (category == MFT_CATEGORY_VIDEO_ENCODER) {
      IMFAttributes* a = nullptr;
      if (SUCCEEDED(t->GetAttributes(&a)) && a) {
        UINT32 len = 0;
        w.hardware = SUCCEEDED(a->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &len));
        wchar_t* name = nullptr;
        if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) && name) {
          w.encoderName = narrow(name);
          CoTaskMemFree(name);
        }
        a->Release();
      }
      if (!w.hardware && w.encoderName == "Media Foundation H.264") w.encoderName = "Microsoft H.264 encoder (software)";
    }
    t->Release();
  }
  ex->Release();
}

/// Creates the sink writer with an H.264 video and an AAC audio stream and starts writing.
/// allowHardware + withDevice: hardware MFTs allowed and given a Direct3D 11 device manager.
bool openWriter(const std::wstring& path, const rnf_export_geometry& g, int64_t bitrate, int audioBitrate, bool allowHardware,
                bool withDevice, Writer& w, std::string* why) {
  IMFAttributes* attr = nullptr;
  HRESULT h = MFCreateAttributes(&attr, 4);
  if (FAILED(h)) {
    *why = hrText("MFCreateAttributes", h);
    return false;
  }
  attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, allowHardware ? TRUE : FALSE);
  if (allowHardware && withDevice) {
    w.dxgi = createDxgiManager();
    if (!w.dxgi) {
      attr->Release();
      *why = "no Direct3D 11 device";
      return false;
    }
    attr->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, w.dxgi);
  }
  attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
  h = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr, &w.sink);
  attr->Release();
  if (FAILED(h)) {
    *why = hrText("Creating the MP4 file", h);
    return false;
  }
  const UINT32 W = UINT32(g.canvas_width), H = UINT32(g.canvas_height);
  // Video: H.264 High, progressive, square pixels, 39375000/655171 fps, BT.709 limited range.
  IMFMediaType* out = nullptr;
  IMFMediaType* in = nullptr;
  MFCreateMediaType(&out);
  out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  out->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::min<int64_t>(bitrate, 0xFFFFFFFF)));
  out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
  MFSetAttributeSize(out, MF_MT_FRAME_SIZE, W, H);
  MFSetAttributeRatio(out, MF_MT_FRAME_RATE, RN_FPS_NUM, RN_FPS_DEN);
  MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  setColorTags(out);
  h = w.sink->AddStream(out, &w.video);
  if (FAILED(h)) {
    // Some (older) encoders refuse High: the default profile then.
    out->DeleteItem(MF_MT_MPEG2_PROFILE);
    h = w.sink->AddStream(out, &w.video);
  }
  out->Release();
  if (FAILED(h)) {
    *why = hrText("H.264 stream", h);
    return false;
  }
  MFCreateMediaType(&in);
  in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  in->SetUINT32(MF_MT_DEFAULT_STRIDE, W);
  MFSetAttributeSize(in, MF_MT_FRAME_SIZE, W, H);
  MFSetAttributeRatio(in, MF_MT_FRAME_RATE, RN_FPS_NUM, RN_FPS_DEN);
  MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  setColorTags(in);
  // Encoder settings (ICodecAPI properties, applied by the sink writer when it creates the
  // encoder): VBR around the average rate, a key frame every 120 frames (as the FFmpeg exporter),
  // no B-frames (no reordering delay; presentation order = decode order).
  IMFAttributes* enc = nullptr;
  MFCreateAttributes(&enc, 4);
  enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
  enc->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, UINT32(std::min<int64_t>(bitrate, 0xFFFFFFFF)));
  enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, 120);
  enc->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
  h = w.sink->SetInputMediaType(w.video, in, enc);
  enc->Release();
  in->Release();
  if (FAILED(h)) {
    *why = hrText(h == MF_E_TOPO_CODEC_NOT_FOUND ? "No H.264 encoder (Media Foundation)" : "H.264 encoder input (NV12)", h);
    return false;
  }
  // Audio: AAC-LC, 48 kHz mono, from 16-bit PCM (the engine's samples as they are).
  MFCreateMediaType(&out);
  out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
  out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RN_SAMPLE_RATE);
  out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
  out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, aacBytesPerSecond(audioBitrate));
  out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
  h = w.sink->AddStream(out, &w.audio);
  out->Release();
  if (FAILED(h)) {
    *why = hrText("AAC stream", h);
    return false;
  }
  MFCreateMediaType(&in);
  in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RN_SAMPLE_RATE);
  in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
  in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
  in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, RN_SAMPLE_RATE * 2);
  h = w.sink->SetInputMediaType(w.audio, in, nullptr);
  in->Release();
  if (FAILED(h)) {
    *why = hrText(h == MF_E_TOPO_CODEC_NOT_FOUND ? "No AAC encoder (Media Foundation)" : "AAC encoder input", h);
    return false;
  }
  identifyEncoder(w);
  h = w.sink->BeginWriting();
  if (FAILED(h)) {
    *why = hrText("Starting the encoders", h);
    return false;
  }
  return true;
}

bool writeSample(IMFSinkWriter* sink, DWORD stream, const void* data, size_t bytes, int64_t time, int64_t duration,
                 void (*fill)(BYTE*, const void*, size_t), std::string* why) {
  IMFMediaBuffer* buf = nullptr;
  IMFSample* sample = nullptr;
  HRESULT h = MFCreateMemoryBuffer(DWORD(bytes), &buf);
  BYTE* p = nullptr;
  if (SUCCEEDED(h)) h = buf->Lock(&p, nullptr, nullptr);
  if (SUCCEEDED(h)) {
    fill(p, data, bytes);
    buf->Unlock();
    h = buf->SetCurrentLength(DWORD(bytes));
  }
  if (SUCCEEDED(h)) h = MFCreateSample(&sample);
  if (SUCCEEDED(h)) h = sample->AddBuffer(buf);
  if (SUCCEEDED(h)) h = sample->SetSampleTime(time);
  if (SUCCEEDED(h)) h = sample->SetSampleDuration(duration);
  if (SUCCEEDED(h)) h = sink->WriteSample(stream, sample);
  release(sample);
  release(buf);
  if (FAILED(h)) *why = hrText("Encoding", h);
  return SUCCEEDED(h);
}

/// Plays the finished file back the way a player on this system would: Media Foundation's MPEG-4
/// source must open it with the expected duration, decode its first frame and its last seconds
/// of video (up to the final frame) and read its last seconds of audio.
/// strict = false (a file from elsewhere): the duration and the last frame's time are not compared
/// with `frames` (frame times as this exporter writes them); the end is taken from the file.
bool verifyPlayback(const std::wstring& path, uint64_t frames, bool strict, std::string* why) {
  IMFSourceReader* r = nullptr;
  HRESULT h = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r);
  if (FAILED(h)) {
    *why = hrText("Self-check: opening the finished file", h);
    return false;
  }
  std::unique_ptr<IMFSourceReader, void (*)(IMFSourceReader*)> reader(r, [](IMFSourceReader* p) { p->Release(); });
  const int64_t expected = exportframe::frameTime100ns(frames);
  PROPVARIANT v;
  PropVariantInit(&v);
  h = r->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v);
  const int64_t duration = SUCCEEDED(h) && v.vt == VT_UI8 ? int64_t(v.uhVal.QuadPart) : -1;
  PropVariantClear(&v);
  // The audio track may run up to one AAC frame (21 ms) longer than the video.
  if (duration <= 0 || (strict && (duration < expected - 10000 || duration > expected + 1000000))) {
    char b[160];
    std::snprintf(b, sizeof b, "Self-check: the finished file lasts %.3f s, expected %.3f s", double(duration) / 1e7,
                  double(expected) / 1e7);
    *why = b;
    return false;
  }
  auto seek = [&](int64_t t) {
    PROPVARIANT p;
    PropVariantInit(&p);
    p.vt = VT_I8;
    p.hVal.QuadPart = std::max<int64_t>(0, t);
    return r->SetCurrentPosition(GUID_NULL, p);
  };
  // Reads stream `stream` from `from` to its end; *count samples, *last = the last time stamp.
  auto readToEnd = [&](DWORD stream, int64_t from, bool once, uint64_t* count, int64_t* last) -> HRESULT {
    HRESULT hr = seek(from);
    if (FAILED(hr)) return hr;
    *count = 0;
    *last = -1;
    for (int guard = 0; guard < 100000; ++guard) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      IMFSample* sample = nullptr;
      hr = r->ReadSample(stream, 0, nullptr, &flags, &ts, &sample);
      if (sample) {
        ++*count;
        *last = ts;
        sample->Release();
      }
      if (FAILED(hr)) return hr;
      if (flags & MF_SOURCE_READERF_ERROR) return E_FAIL;
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return S_OK;
      if (once && *count) return S_OK;
    }
    return E_FAIL;
  };
  const DWORD V = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), A = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
  // Video, decoded to NV12.
  r->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  h = r->SetStreamSelection(V, TRUE);
  IMFMediaType* t = nullptr;
  if (SUCCEEDED(h)) h = MFCreateMediaType(&t);
  if (SUCCEEDED(h)) {
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    h = r->SetCurrentMediaType(V, nullptr, t);
    t->Release();
  }
  if (FAILED(h)) {
    *why = hrText("Self-check: H.264 decoder for the finished file", h);
    return false;
  }
  uint64_t n = 0;
  int64_t last = -1;
  h = readToEnd(V, 0, true, &n, &last);
  if (FAILED(h) || n == 0) {
    *why = hrText("Self-check: decoding the first frame of the finished file", FAILED(h) ? h : E_FAIL);
    return false;
  }
  const int64_t end = strict ? expected : duration;
  h = readToEnd(V, end - 30000000, false, &n, &last);
  const int64_t lastFrame = strict ? exportframe::frameTime100ns(frames - 1) : last;
  if (FAILED(h) || n == 0 || last < lastFrame - 10000 || last > lastFrame + 10000 || last < end - 10000000) {
    char b[200];
    std::snprintf(b, sizeof b, "Self-check: decoding the end of the finished file (0x%08lx, %llu frames, last at %.3f s, expected %.3f s)",
                  (unsigned long)h, (unsigned long long)n, double(last) / 1e7, double(lastFrame) / 1e7);
    *why = b;
    return false;
  }
  // Audio (compressed samples as stored).
  r->SetStreamSelection(V, FALSE);
  h = r->SetStreamSelection(A, TRUE);
  if (SUCCEEDED(h)) h = readToEnd(A, end - 30000000, false, &n, &last);
  if (FAILED(h) || n == 0) {
    *why = hrText("Self-check: reading the audio at the end of the finished file", FAILED(h) ? h : E_FAIL);
    return false;
  }
  return true;
}

}  // namespace

bool verifyExportedMp4(const std::string& path, uint64_t frames, std::string* error) {
  std::string localError;
  std::string& err = error ? *error : localError;
  Mp4Summary sum;
  if (!checkMp4File(path, &sum, &err)) return false;
  const Mp4TrackSummary* video = nullptr;
  for (const auto& t : sum.tracks)
    if (t.handler == "vide" && !video) video = &t;
  if (!video || !video->samples) {
    err = "MP4 check: no video";
    return false;
  }
  if (frames && video->samples != frames) {
    err = "MP4 check: " + std::to_string(video->samples) + " video frames, expected " + std::to_string(frames);
    return false;
  }
  MfScope mf;
  const HRESULT h = mf.init();
  if (FAILED(h)) {
    err = hrText("Media Foundation (MFStartup)", h);
    return false;
  }
  return verifyPlayback(widen(path), video->samples, frames != 0, &err);
}

const std::vector<ExportEncoderChoice>& exportEncoderChoices() {
  static const std::vector<ExportEncoderChoice> choices = {{"mf_hardware", "hardware (MFT)"}, {"mf_software", "software (MFT)"}};
  return choices;
}

std::string availableEncodersDescription() {
  MfScope mf;
  if (FAILED(mf.init())) return "Media Foundation unavailable";
  MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
  std::string out;
  for (UINT32 flags : {UINT32(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER),
                       UINT32(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER)}) {
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &acts, &n))) continue;
    for (UINT32 i = 0; i < n; ++i) {
      wchar_t* name = nullptr;
      UINT32 len = 0;
      if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) && name) {
        out += (out.empty() ? "" : ", ") + narrow(name) + ((flags & MFT_ENUM_FLAG_HARDWARE) ? " (hardware)" : " (software)");
        CoTaskMemFree(name);
      }
      acts[i]->Release();
    }
    CoTaskMemFree(acts);
  }
  return out.empty() ? "no H.264 encoder" : out;
}

namespace {

/// One export pass with `renderer` (not owned). *hardwareFailed: a hardware encoder was used and
/// the failure came from encoding / finishing / the self-check (worth a software retry).
bool exportOnce(rn_renderer* renderer, const ExportOptions& opt, const std::string& outPath, const ExportProgressFn& progress,
                const ExportCancelFn& cancelled, ExportResult* result, std::string* error, bool* hardwareFailed) {
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
  const int64_t bitrate = rnf_export_video_bitrate(&g, opt.videoBitsPerPixel);

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

  // ---- Media Foundation
  MfScope mf;
  HRESULT h = mf.init();
  if (FAILED(h)) {
    err = hrText("Media Foundation (MFStartup)", h);
    return false;
  }
  // Written under a temporary name next to the destination and renamed only after the file has
  // been finished, fixed up and checked: a failed / cancelled / crashed export never leaves a
  // broken file under the chosen name (nor replaces an existing one).
  const std::string partPath = outPath + ".part";
  const std::wstring wpath = widen(partPath), wfinal = widen(outPath);
  auto removeFile = [&] { DeleteFileW(wpath.c_str()); };
  removeFile();
  std::unique_ptr<Writer> w;
  {
    const bool forceHw = opt.encoder == "mf_hardware", forceSw = opt.encoder == "mf_software";
    if (!opt.encoder.empty() && !forceHw && !forceSw) {
      err = "Unknown encoder \"" + opt.encoder + "\" (mf_hardware, mf_software)";
      return false;
    }
    // Hardware first (with a Direct3D 11 device, then without one), then Microsoft's software
    // encoder. A hardware attempt that ends up with a software MFT is dropped for the plain one.
    std::string tried;
    struct Attempt {
      bool hw, device;
      const char* label;
    };
    for (const Attempt& at : {Attempt{true, true, "hardware (Direct3D 11)"}, Attempt{true, false, "hardware"}, Attempt{false, false, "software"}}) {
      if ((at.hw && forceSw) || (!at.hw && forceHw)) continue;
      auto cand = std::make_unique<Writer>();
      std::string why;
      if (openWriter(wpath, g, bitrate, opt.audioBitrate, at.hw, at.device, *cand, &why)) {
        if (at.hw && !cand->hardware) {
          why = "no hardware H.264 encoder";
        } else {
          w = std::move(cand);
          break;
        }
      }
      cand.reset();
      removeFile();
      tried += (tried.empty() ? "" : "; ") + std::string(at.label) + ": " + why;
    }
    if (!w) {
      err = "No usable H.264 encoder (" + tried + ")";
      return false;
    }
  }
  auto fail = [&](const std::string& m) {
    err = m;
    w.reset();  // abandons the file without finalising it
    removeFile();
    return false;
  };
  const bool usedHardware = w->hardware;
  auto failEncoder = [&](const std::string& m) {  // encoder / sink / file trouble (not the renderer's)
    *hardwareFailed = usedHardware;
    return fail(m);
  };

  const int W = g.canvas_width, H = g.canvas_height;
  const size_t canvasStride = size_t(W) * 4;
  std::vector<uint8_t> canvas(canvasStride * size_t(H));
  std::vector<uint8_t> nv12(size_t(W) * H * 3 / 2);
  const uint64_t startFrame = opt.settings.start_frame;
  const uint64_t sampleBase = rn_audio_samples_before(startFrame);
  uint64_t samplesQueued = 0;  // next audio sample position (relative to the start)
  uint64_t framesWritten = 0;
  std::vector<int16_t> pcm;
  auto copyBytes = [](BYTE* dst, const void* src, size_t n) { std::memcpy(dst, src, n); };

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
      exportframe::scaleNearest(pixels, canvas.data(), canvasStride, g, cols, rows);
    }
    exportframe::convertToYuv(canvas.data(), canvasStride, W, H, nv12.data(), W, nv12.data() + size_t(W) * H, W, nullptr, 0);
    const uint64_t idx = f - startFrame;
    const int64_t t0 = exportframe::frameTime100ns(idx), t1 = exportframe::frameTime100ns(idx + 1);
    std::string why;
    if (!writeSample(w->sink, w->video, nv12.data(), nv12.size(), t0, t1 - t0, copyBytes, &why)) return failEncoder(why);
    ++framesWritten;
    if (opt.simulateHardwareFailureAfter && framesWritten >= opt.simulateHardwareFailureAfter) {
      const std::string m = "simulated hardware encoder failure";
      err = m;
      w.reset();
      removeFile();
      *hardwareFailed = true;
      return false;
    }

    // Audio: sample position from the absolute count at 48 kHz. The renderer's PCM is contiguous;
    // a gap would be filled with silence and an overlap dropped so A/V never drift.
    if (n > 0 && audio) {
      const uint64_t at = rn_audio_samples_before(f) - sampleBase;
      size_t skip = 0;
      pcm.clear();
      uint64_t from = samplesQueued;
      if (at > samplesQueued) pcm.insert(pcm.end(), size_t(at - samplesQueued), int16_t(0));
      else if (at < samplesQueued) skip = size_t(std::min<uint64_t>(samplesQueued - at, n));
      pcm.insert(pcm.end(), audio + skip, audio + n);
      if (!pcm.empty()) {
        samplesQueued = from + pcm.size();
        const int64_t a0 = exportframe::sampleTime100ns(from), a1 = exportframe::sampleTime100ns(samplesQueued);
        if (!writeSample(w->sink, w->audio, pcm.data(), pcm.size() * 2, a0, a1 - a0, copyBytes, &why)) return failEncoder(why);
      }
    }
    if (progress) progress(rn_renderer_frames_done(rend.get()), total);
  }

  if (cancelled && cancelled()) return fail("cancelled");
  h = w->sink->Finalize();
  if (FAILED(h)) return failEncoder(hrText("Finishing the file", h));
  const std::string encName = w->encoderName + (w->hardware ? " (hardware)" : "");
  w.reset();
  // The MPEG-4 sink stores the video at a rounded timescale (fps x 1000): exact NTSC frame times
  // (timescale 39375000, 655171 per frame) as the FFmpeg exporter writes them.
  {
    std::string why;
    if (!retimeMp4Video(partPath, framesWritten, RN_FPS_NUM, RN_FPS_DEN, &why)) return failEncoder(why);
    // Self-check before the file gets its name: complete box structure, every chunk offset
    // pointing at its samples (also past 4 GiB), and Media Foundation plays it to the last frame.
    if (!verifyExportedMp4(partPath, framesWritten, &why)) return failEncoder(why);
  }
  // (Media Foundation may close the self-check's file handle a moment after the reader is gone.)
  for (int attempt = 0;; ++attempt) {
    if (MoveFileExW(wpath.c_str(), wfinal.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
    const DWORD e = GetLastError();
    if (attempt >= 20 || (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED))
      return fail(hrText("Saving the file under its name", HRESULT_FROM_WIN32(e)));
    Sleep(100);
  }

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
  if (exportOnce(rend.get(), opt, outPath, progress, cancelled, result, &err, &hardwareFailed)) return true;
  // Automatic encoder choice: a hardware encoder that failed while encoding, finishing or in the
  // self-check gets one retry with the software encoder (the same picture, timing and settings).
  if (!hardwareFailed || !opt.encoder.empty() || !opt.retryRenderer || (cancelled && cancelled())) return false;
  const std::string hwError = err;
  ExportOptions sw = opt;
  sw.encoder = "mf_software";
  sw.retryRenderer.reset();
  sw.simulateHardwareFailureAfter = 0;
  if (!exportOnce(opt.retryRenderer.get(), sw, outPath, progress, cancelled, result, &err, &hardwareFailed)) {
    if (err != "cancelled") err = "Hardware encoder: " + hwError + "; software encoder: " + err;
    return false;
  }
  if (result) result->encoder += " - the hardware encoder failed (" + hwError + ")";
  return true;
}

}  // namespace rnl
