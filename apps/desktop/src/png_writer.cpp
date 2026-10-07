// SPDX-License-Identifier: GPL-2.0-or-later
#include "png_writer.h"

#include <algorithm>
#include <cstdio>

namespace rnl {

namespace {
uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
void be32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(uint8_t(v >> 24));
  o.push_back(uint8_t(v >> 16));
  o.push_back(uint8_t(v >> 8));
  o.push_back(uint8_t(v));
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  be32(out, uint32_t(data.size()));
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  be32(out, crc32(out.data() + start, out.size() - start));
}
}  // namespace

std::vector<uint8_t> encodePNG(int w, int h, const uint8_t* rgba) {
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  be32(ihdr, uint32_t(w));
  be32(ihdr, uint32_t(h));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8 bit, RGBA, deflate, adaptive filter, no interlace
  chunk(out, "IHDR", ihdr);
  // Raw scanlines (filter byte 0) in zlib stored blocks.
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  size_t off = 0;
  do {
    size_t n = std::min<size_t>(65535, raw.size() - off);
    bool last = off + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + long(off), raw.begin() + long(off + n));
    off += n;
  } while (off < raw.size());
  be32(z, (b << 16) | a);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  return out;
}

bool writePNG(const std::string& path, int w, int h, const uint8_t* rgba) {
  std::vector<uint8_t> data = encodePNG(w, h, rgba);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace rnl
