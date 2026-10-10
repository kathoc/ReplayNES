// SPDX-License-Identifier: GPL-2.0-or-later
#include "mp4_retime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace rnl {

namespace {

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) << 32 | rd32(p + 4); }
void put32(std::vector<uint8_t>& o, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}
void put64(std::vector<uint8_t>& o, uint64_t v) {
  put32(o, uint32_t(v >> 32));
  put32(o, uint32_t(v));
}
void set32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

struct Box {
  std::string type;
  std::vector<uint8_t> payload;  // leaf boxes
  std::vector<std::unique_ptr<Box>> children;
  bool container = false;
  Box* child(const char* t) {
    for (auto& c : children)
      if (c->type == t) return c.get();
    return nullptr;
  }
};

bool isContainer(const std::string& t) {
  return t == "moov" || t == "trak" || t == "mdia" || t == "minf" || t == "stbl" || t == "edts";
}

bool parse(const uint8_t* p, size_t n, std::vector<std::unique_ptr<Box>>* out) {
  size_t off = 0;
  while (off < n) {
    if (n - off < 8) return false;
    uint64_t size = rd32(p + off);
    size_t header = 8;
    if (size == 1) {
      if (n - off < 16) return false;
      size = rd64(p + off + 8);
      header = 16;
    } else if (size == 0) {
      size = n - off;
    }
    if (size < header || size > n - off) return false;
    auto b = std::make_unique<Box>();
    b->type.assign(reinterpret_cast<const char*>(p + off + 4), 4);
    b->container = isContainer(b->type);
    if (b->container) {
      if (!parse(p + off + header, size_t(size) - header, &b->children)) return false;
    } else {
      b->payload.assign(p + off + header, p + off + size_t(size));
    }
    out->push_back(std::move(b));
    off += size_t(size);
  }
  return true;
}

void serialize(const Box& b, std::vector<uint8_t>& o) {
  size_t start = o.size();
  put32(o, 0);
  o.insert(o.end(), b.type.begin(), b.type.end());
  if (b.container)
    for (const auto& c : b.children) serialize(*c, o);
  else
    o.insert(o.end(), b.payload.begin(), b.payload.end());
  set32(o.data() + start, uint32_t(o.size() - start));
}

std::string handler(Box* trak) {
  Box* mdia = trak->child("mdia");
  Box* hdlr = mdia ? mdia->child("hdlr") : nullptr;
  if (!hdlr || hdlr->payload.size() < 12) return "";
  return std::string(reinterpret_cast<const char*>(hdlr->payload.data() + 8), 4);
}

/// (timescale, duration) of an mvhd / mdhd payload.
bool readHeader(const Box& b, uint32_t* timescale, uint64_t* duration) {
  const auto& p = b.payload;
  if (p.empty()) return false;
  if (p[0] == 1) {
    if (p.size() < 32) return false;
    *timescale = rd32(&p[20]);
    *duration = rd64(&p[24]);
  } else {
    if (p.size() < 20) return false;
    *timescale = rd32(&p[12]);
    *duration = rd32(&p[16]);
  }
  return true;
}

uint64_t rescale(uint64_t v, uint64_t from, uint64_t to) {
  return uint64_t(std::llround(double(v) * double(to) / double(from)));
}

/// tkhd duration (in the movie timescale), version 1 when it doesn't fit 32 bits.
bool setTkhdDuration(Box& tkhd, uint64_t d) {
  auto& p = tkhd.payload;
  if (p.empty()) return false;
  if (p[0] == 0 && d > 0xFFFFFFFFull) {  // v0 -> v1: creation, modification 64-bit; duration 64-bit
    if (p.size() < 24) return false;
    std::vector<uint8_t> n = {1, p[1], p[2], p[3]};
    put64(n, rd32(&p[4]));
    put64(n, rd32(&p[8]));
    put32(n, rd32(&p[12]));  // track id
    put32(n, rd32(&p[16]));  // reserved
    put64(n, d);
    n.insert(n.end(), p.begin() + 24, p.end());
    p.swap(n);
    return true;
  }
  if (p[0] == 1) {
    if (p.size() < 36) return false;
    for (int i = 0; i < 8; ++i) p[28 + i] = uint8_t(d >> (56 - 8 * i));
  } else {
    if (p.size() < 24) return false;
    set32(&p[20], uint32_t(d));
  }
  return true;
}

}  // namespace

bool retimeMp4Video(const std::string& path, uint64_t frames, uint32_t timescale, uint32_t frameTicks, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = "MP4 timing: " + m;
    return false;
  };
  const fs::path fp = fs::u8path(path);
  std::error_code ec;
  const uint64_t fileSize = fs::file_size(fp, ec);
  if (ec) return fail("cannot read the file size");
  std::fstream f(fp, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) return fail("cannot open the file");
  // Top-level boxes: find moov, which must be the last one.
  uint64_t off = 0, moovOff = 0, moovSize = 0;
  while (off + 8 <= fileSize) {
    uint8_t h[16];
    f.seekg(std::streamoff(off));
    f.read(reinterpret_cast<char*>(h), 16);
    if (f.gcount() < 8) return fail("truncated box header");
    f.clear();
    uint64_t size = rd32(h);
    if (size == 1) size = rd64(h + 8);
    else if (size == 0) size = fileSize - off;
    if (size < 8 || off + size > fileSize) return fail("bad box size");
    if (std::memcmp(h + 4, "moov", 4) == 0) {
      moovOff = off;
      moovSize = size;
    }
    off += size;
  }
  if (!moovSize) return fail("no moov box");
  if (moovOff + moovSize != fileSize) return fail("moov is not at the end of the file");
  std::vector<uint8_t> raw(static_cast<size_t>(moovSize));
  f.seekg(std::streamoff(moovOff));
  f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
  if (size_t(f.gcount()) != raw.size()) return fail("cannot read moov");
  f.clear();
  std::vector<std::unique_ptr<Box>> top;
  if (!parse(raw.data(), raw.size(), &top) || top.size() != 1 || top[0]->type != "moov") return fail("cannot parse moov");
  Box& moov = *top[0];
  Box* mvhd = moov.child("mvhd");
  uint32_t movieScale = 0;
  uint64_t movieDuration = 0;
  if (!mvhd || !readHeader(*mvhd, &movieScale, &movieDuration) || !movieScale) return fail("no mvhd");

  Box* video = nullptr;
  uint64_t longest = 0;  // other tracks' durations (movie timescale)
  for (auto& c : moov.children) {
    if (c->type != "trak") continue;
    if (handler(c.get()) == "vide" && !video) {
      video = c.get();
      continue;
    }
    if (Box* tkhd = c->child("tkhd")) {
      const auto& p = tkhd->payload;
      if (!p.empty()) longest = std::max<uint64_t>(longest, p[0] == 1 && p.size() >= 36 ? rd64(&p[28]) : p.size() >= 24 ? rd32(&p[20]) : 0);
    }
  }
  if (!video) return fail("no video track");
  Box* mdia = video->child("mdia");
  Box* mdhd = mdia ? mdia->child("mdhd") : nullptr;
  Box* minf = mdia ? mdia->child("minf") : nullptr;
  Box* stbl = minf ? minf->child("stbl") : nullptr;
  Box* stts = stbl ? stbl->child("stts") : nullptr;
  Box* tkhd = video->child("tkhd");
  uint32_t oldScale = 0;
  uint64_t oldDuration = 0;
  if (!mdhd || !stts || !tkhd || !readHeader(*mdhd, &oldScale, &oldDuration) || !oldScale) return fail("incomplete video track");
  // Every video sample is one frame: the sample count must match.
  uint64_t samples = 0;
  {
    const auto& p = stts->payload;
    if (p.size() < 8) return fail("bad stts");
    const uint32_t n = rd32(&p[4]);
    if (p.size() < 8 + size_t(n) * 8) return fail("bad stts");
    for (uint32_t i = 0; i < n; ++i) samples += rd32(&p[8 + size_t(i) * 8]);
  }
  if (samples != frames) return fail("the video track has " + std::to_string(samples) + " samples, expected " + std::to_string(frames));
  const uint64_t duration = frames * uint64_t(frameTicks);

  // mdhd: version 1 (64-bit duration), the new timescale.
  {
    auto& p = mdhd->payload;
    const bool v1 = p[0] == 1;
    const size_t lang = v1 ? 32 : 20;
    if (p.size() < lang + 4) return fail("bad mdhd");
    std::vector<uint8_t> n = {1, p[1], p[2], p[3]};
    put64(n, v1 ? rd64(&p[4]) : rd32(&p[4]));
    put64(n, v1 ? rd64(&p[12]) : rd32(&p[8]));
    put32(n, timescale);
    put64(n, duration);
    n.insert(n.end(), p.begin() + long(lang), p.end());
    p.swap(n);
  }
  // stts: one run of `frames` samples of frameTicks each.
  {
    std::vector<uint8_t> n = {0, 0, 0, 0};
    put32(n, 1);
    put32(n, uint32_t(std::min<uint64_t>(frames, 0xFFFFFFFFull)));
    put32(n, frameTicks);
    stts->payload.swap(n);
  }
  // ctts: offsets in whole frames of the old timescale -> new ticks; all zero -> removed.
  for (size_t i = 0; i < stbl->children.size(); ++i) {
    Box& c = *stbl->children[i];
    if (c.type != "ctts") continue;
    auto& p = c.payload;
    if (p.size() < 8) return fail("bad ctts");
    const uint32_t n = rd32(&p[4]);
    if (p.size() < 8 + size_t(n) * 8) return fail("bad ctts");
    bool allZero = true;
    for (uint32_t k = 0; k < n; ++k) {
      uint8_t* e = &p[8 + size_t(k) * 8 + 4];
      // Signed in practice also in version 0 (Media Foundation writes negative offsets there).
      const int64_t old = int64_t(int32_t(rd32(e)));
      const double framesOff = double(old) * double(timescale) / double(oldScale) / double(frameTicks);
      const int64_t nv = int64_t(std::llround(framesOff)) * int64_t(frameTicks);
      allZero &= nv == 0;
      set32(e, uint32_t(nv));
    }
    if (allZero) {
      stbl->children.erase(stbl->children.begin() + long(i));
    }
    break;
  }
  // tkhd / edit list / mvhd durations (movie timescale).
  const uint64_t trackDuration = rescale(duration, timescale, movieScale);
  if (!setTkhdDuration(*tkhd, trackDuration)) return fail("bad tkhd");
  if (Box* edts = video->child("edts")) {
    if (Box* elst = edts->child("elst")) {
      auto& p = elst->payload;
      if (p.size() < 8) return fail("bad elst");
      const uint32_t n = rd32(&p[4]);
      const bool v1 = p[0] == 1;
      const size_t es = v1 ? 20 : 12;
      if (p.size() < 8 + n * es) return fail("bad elst");
      if (n == 1) {
        uint8_t* e = &p[8];
        const int64_t mediaTime = v1 ? int64_t(rd64(e + 8)) : int64_t(int32_t(rd32(e + 4)));
        const int64_t newMedia = mediaTime < 0 ? mediaTime
                                               : int64_t(std::llround(double(mediaTime) * timescale / oldScale / frameTicks)) * frameTicks;
        std::vector<uint8_t> nb = {1, p[1], p[2], p[3]};
        put32(nb, 1);
        put64(nb, trackDuration);
        put64(nb, uint64_t(newMedia));
        nb.insert(nb.end(), e + es - 4, e + es);  // media rate
        p.swap(nb);
      }
    }
  }
  {
    auto& p = mvhd->payload;
    const uint64_t d = std::max(longest, trackDuration);
    if (p[0] == 1) {
      if (p.size() < 32) return fail("bad mvhd");
      for (int i = 0; i < 8; ++i) p[24 + i] = uint8_t(d >> (56 - 8 * i));
    } else {
      if (p.size() < 20 || d > 0xFFFFFFFFull) return fail("movie duration does not fit mvhd v0");
      set32(&p[16], uint32_t(d));
    }
  }
  std::vector<uint8_t> out;
  serialize(moov, out);
  // A smaller moov leaves the rest of the old one as a 'free' box: the file is never truncated.
  // (std::filesystem::resize_file must not be used: with llvm-mingw's libc++ it goes through a
  // 32-bit off_t and cut files past 4 GiB to their size modulo 2^32 - moov and the end of the
  // media data were gone, the file unplayable. That was the 0.5.2 export bug on Windows.)
  if (out.size() < raw.size()) {
    size_t pad = raw.size() - out.size();
    if (pad < 8) pad += 8;  // a box needs 8 bytes: grow the file by 8 instead (fine at the end)
    const size_t at = out.size();
    out.resize(at + pad, 0);
    set32(out.data() + at, uint32_t(pad));
    std::memcpy(out.data() + at + 4, "free", 4);
  }
  f.seekp(std::streamoff(moovOff));
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  f.close();
  if (!f) return fail("cannot write moov");
  return true;
}

}  // namespace rnl
