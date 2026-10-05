#include "core/ICore.h"

#include "core/MockCore.h"
#include "core/NestopiaCore.h"
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

std::unique_ptr<ICore> createCore(CoreKind kind) {
  if (kind == CoreKind::Mock) return std::unique_ptr<ICore>(new MockCore());
  return std::unique_ptr<ICore>(new NestopiaCore());
}

std::string coreCompatId(CoreKind kind) {
  return kind == CoreKind::Mock ? MockCore::kCompatId : NestopiaCore::staticCompatId();
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
