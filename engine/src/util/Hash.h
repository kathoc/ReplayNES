// Hashing: SHA-256 (ROM identity), CRC32 (file/record checksums),
// Hasher64 (fast non-cryptographic 64-bit hash for determinism comparisons).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rn {

uint32_t crc32(const void* data, size_t n, uint32_t prev = 0);

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t n);
  std::array<uint8_t, 32> finish();
  static std::string hex(const void* data, size_t n);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t bufLen_ = 0;
  uint64_t total_ = 0;
};

std::string toHex(const uint8_t* p, size_t n);

// Streaming 64-bit hash. Result is independent of how input is chunked.
class Hasher64 {
 public:
  explicit Hasher64(uint64_t seed = 0);
  void update(const void* data, size_t n);
  void u64(uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (8 * i)); update(b, 8); }
  uint64_t digest() const;
  static uint64_t of(const void* data, size_t n, uint64_t seed = 0) {
    Hasher64 h(seed); h.update(data, n); return h.digest();
  }

 private:
  void mix(uint64_t w);
  uint64_t h_;
  uint64_t len_ = 0;
  uint8_t tail_[8];
  size_t tailLen_ = 0;
};

}  // namespace rn
