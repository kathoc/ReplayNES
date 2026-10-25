#include "core/ICore.h"
#include "replaynes/product.h"

#include "core/MockCore.h"
#if RN_PRODUCT_NES
#include "core/NestopiaCore.h"
#endif
#ifdef REPLAYNES_HAS_REPLAYNESCORE
#include "core/ReplayNEScoreAdapter.h"
#endif
#include "util/Bytes.h"
#include "util/Hash.h"

namespace rn {

uint64_t ICore::machineHash() {
  std::vector<uint8_t> s;
  if (!saveState(s).ok()) return 0;
  return Hasher64::of(s.data(), s.size());
}

uint64_t ICore::videoHash() const { return videoFrameHash(videoFrame()); }

uint64_t ICore::audioHash() const {
  size_t n = 0;
  const int16_t* a = audio(&n);
  return audioFramesHash(a, n, mediaInfo().audioChannels);
}

// Rows of the visible picture only (stride padding is not part of the picture). Hasher64 does not
// depend on chunking, so a tightly packed 256x240 frame hashes exactly as it always did.
uint64_t videoFrameHash(const VideoFrame& v) {
  Hasher64 h;
  for (uint32_t y = 0; y < v.height; ++y) h.update(v.pixels + size_t(y) * v.stride, size_t(v.width) * 4);
  return h.digest();
}

uint64_t audioFramesHash(const int16_t* a, size_t frames, uint32_t channels) {
  return Hasher64::of(a, frames * channels * 2, frames);
}

namespace {
ExternalCore& externalCore() {
  static ExternalCore core;
  return core;
}
}  // namespace

void registerExternalCore(ExternalCore core) { externalCore() = std::move(core); }
void unregisterExternalCore() { externalCore() = ExternalCore{}; }

const char* coreName(CoreKind kind) {
  switch (kind) {
    case CoreKind::External: return externalCore().create ? externalCore().name.c_str() : nullptr;
    case CoreKind::Nestopia: return "nestopia";
    case CoreKind::Mock: return "mock";
    case CoreKind::ReplayNEScore: return "replaynescore";
  }
  return nullptr;
}

CoreKind defaultCoreKind() {
  const std::string name = RN_PRODUCT_DEFAULT_CORE;
  if (name == "nestopia") return CoreKind::Nestopia;
  if (name == "mock") return CoreKind::Mock;
  if (name == "replaynescore") return CoreKind::ReplayNEScore;
  if (name == "external") return CoreKind::External;
  return static_cast<CoreKind>(-1);
}

bool coreAvailable(CoreKind kind) {
  if (kind == CoreKind::Mock) return true;
#if RN_PRODUCT_NES
  if (kind == CoreKind::Nestopia) return true;
#endif
  if (kind == CoreKind::External) return bool(externalCore().create);
#ifdef REPLAYNES_HAS_REPLAYNESCORE
  if (kind == CoreKind::ReplayNEScore) return true;
#endif
  return false;
}

uint32_t coreCapabilities(CoreKind kind) {
  if (!coreAvailable(kind)) return 0;
  if (kind == CoreKind::Nestopia || kind == CoreKind::Mock)
    return uint32_t(CapAudio | CapState | CapSoftReset | CapRecording);
  if (kind == CoreKind::ReplayNEScore && coreAvailable(kind)) return uint32_t(CapAudio | CapState);
  if (kind == CoreKind::External && coreAvailable(kind)) return externalCore().capabilities;
  return 0;
}

std::unique_ptr<ICore> createCore(CoreKind kind) {
  switch (kind) {
    case CoreKind::Nestopia:
#if RN_PRODUCT_NES
      return std::unique_ptr<ICore>(new NestopiaCore());
#else
      return nullptr;
#endif
    case CoreKind::Mock: return std::unique_ptr<ICore>(new MockCore());
    case CoreKind::External: return externalCore().create ? externalCore().create() : nullptr;
    case CoreKind::ReplayNEScore:
#ifdef REPLAYNES_HAS_REPLAYNESCORE
      return std::unique_ptr<ICore>(new ReplayNEScoreAdapter());
#else
      return nullptr;
#endif
  }
  return nullptr;
}

std::string coreCompatId(CoreKind kind) {
  switch (kind) {
    case CoreKind::Nestopia:
#if RN_PRODUCT_NES
      return NestopiaCore::staticCompatId();
#else
      return {};
#endif
    case CoreKind::Mock: return MockCore::kCompatId;
    case CoreKind::External: return externalCore().create ? externalCore().compatId : std::string();
    case CoreKind::ReplayNEScore:
#ifdef REPLAYNES_HAS_REPLAYNESCORE
      return ReplayNEScoreAdapter::staticCompatId();
#else
      return {};
#endif
  }
  return {};
}

bool coreForCompatId(const std::string& compat, CoreKind& kind) {
  for (CoreKind candidate : {CoreKind::Nestopia, CoreKind::Mock, CoreKind::ReplayNEScore, CoreKind::External}) {
    if (coreAvailable(candidate) && !compat.empty() && coreCompatId(candidate) == compat) {
      kind = candidate;
      return true;
    }
  }
  return false;
}

namespace stateenv {
static const uint32_t kMagic = 0x53434E52;  // "RNCS"
static const uint32_t kVersion = 1;

void write(std::vector<uint8_t>& out, const std::string& compat, uint64_t frame, const uint8_t* payload, size_t n) {
  ByteWriter w;
  w.u32(kMagic);
  w.u32(kVersion);
  w.str(compat);
  w.u64(frame);
  w.u64(n);
  w.bytes(payload, n);
  out.swap(w.buf);
}

Status read(const uint8_t* data, size_t n, const std::string& expectCompat, uint64_t& frame, const uint8_t*& payload,
            size_t& payloadLen) {
  ByteReader r(data, n);
  if (r.u32() != kMagic) return Error(Err::StateError, "not a " RN_PRODUCT_NAME " core state");
  uint32_t v = r.u32();
  if (v != kVersion) return Error(Err::StateError, "unsupported core state envelope version " + std::to_string(v));
  std::string compat = r.str(256);
  if (!r.ok()) return Error(Err::StateError, "truncated core state");
  if (compat != expectCompat)
    return Error(Err::CoreMismatch, "state was produced by core '" + compat + "', running '" + expectCompat + "'");
  frame = r.u64();
  uint64_t len = r.u64();
  payload = r.ptr(size_t(len));
  if (!r.ok() || r.remaining() != 0) return Error(Err::StateError, "truncated core state payload");
  payloadLen = size_t(len);
  return Status::Ok();
}
}  // namespace stateenv

}  // namespace rn
