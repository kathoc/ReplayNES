// Little-endian byte writer/reader with LEB128 varints. Used by all binary formats.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rn {

class ByteWriter {
 public:
  std::vector<uint8_t> buf;
  void u8(uint8_t v) { buf.push_back(v); }
  void u16(uint16_t v) { for (int i = 0; i < 2; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
  void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
  void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
  void varint(uint64_t v) {
    while (v >= 0x80) { buf.push_back(uint8_t(v) | 0x80); v >>= 7; }
    buf.push_back(uint8_t(v));
  }
  void bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), b, b + n);
  }
  void str(const std::string& s) { varint(s.size()); bytes(s.data(), s.size()); }
  size_t size() const { return buf.size(); }
};

class ByteReader {
 public:
  ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  size_t pos() const { return pos_; }
  size_t remaining() const { return ok_ ? n_ - pos_ : 0; }
  uint8_t u8() { uint8_t v = 0; take(&v, 1); return v; }
  uint16_t u16() { uint8_t b[2] = {}; take(b, 2); return uint16_t(b[0] | b[1] << 8); }
  uint32_t u32() {
    uint8_t b[4] = {}; take(b, 4);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
  }
  uint64_t u64() { uint64_t lo = u32(); uint64_t hi = u32(); return lo | hi << 32; }
  uint64_t varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      uint8_t b = u8();
      if (!ok_) return 0;
      v |= uint64_t(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    ok_ = false;  // overlong
    return 0;
  }
  bool bytes(void* out, size_t n) { return take(out, n); }
  const uint8_t* ptr(size_t n) {
    if (!ok_ || n > n_ - pos_) { ok_ = false; return nullptr; }
    const uint8_t* r = p_ + pos_; pos_ += n; return r;
  }
  std::string str(size_t maxLen = 1 << 20) {
    uint64_t n = varint();
    if (!ok_ || n > maxLen) { ok_ = false; return {}; }
    const uint8_t* s = ptr(size_t(n));
    return s ? std::string(reinterpret_cast<const char*>(s), size_t(n)) : std::string();
  }

 private:
  bool take(void* out, size_t n) {
    if (!ok_ || n > n_ - pos_) { ok_ = false; std::memset(out, 0, n); return false; }
    std::memcpy(out, p_ + pos_, n); pos_ += n; return true;
  }
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace rn
