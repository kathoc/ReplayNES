#include "core/ICore.h"

#include "core/MockCore.h"
#include "core/NestopiaCore.h"
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

uint64_t ICore::videoHash() const {
  return Hasher64::of(video(), size_t(kVideoWidth) * kVideoHeight * 4);
}

uint64_t ICore::audioHash() const {
  size_t n = 0;
  const int16_t* a = audio(&n);
  return Hasher64::of(a, n * 2, n);
}

const char* coreName(CoreKind kind) {
  switch (kind) {
    case CoreKind::Nestopia: return "nestopia";
    case CoreKind::Mock: return "mock";
    case CoreKind::ReplayNEScore: return "replaynescore";
  }
  return nullptr;
}

bool coreAvailable(CoreKind kind) {
  if (kind == CoreKind::Nestopia || kind == CoreKind::Mock) return true;
#ifdef REPLAYNES_HAS_REPLAYNESCORE
  if (kind == CoreKind::ReplayNEScore) return true;
#endif
  return false;
}

uint32_t coreCapabilities(CoreKind kind) {
  if (kind == CoreKind::Nestopia || kind == CoreKind::Mock)
    return uint32_t(CapAudio | CapState | CapSoftReset | CapRecording);
  if (kind == CoreKind::ReplayNEScore && coreAvailable(kind)) return uint32_t(CapAudio | CapState);
  return 0;
}

std::unique_ptr<ICore> createCore(CoreKind kind) {
  switch (kind) {
    case CoreKind::Nestopia: return std::unique_ptr<ICore>(new NestopiaCore());
    case CoreKind::Mock: return std::unique_ptr<ICore>(new MockCore());
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
    case CoreKind::Nestopia: return NestopiaCore::staticCompatId();
    case CoreKind::Mock: return MockCore::kCompatId;
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
  for (CoreKind candidate : {CoreKind::Nestopia, CoreKind::Mock, CoreKind::ReplayNEScore}) {
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
  if (r.u32() != kMagic) return Error(Err::StateError, "not a ReplayNES core state");
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
