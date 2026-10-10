// SPDX-License-Identifier: GPL-2.0-or-later
#include "mp4_check.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace rnl {

namespace {

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) << 32 | rd32(p + 4); }

struct Node {
  std::string type;
  const uint8_t* body = nullptr;  // payload (after the header)
  size_t size = 0;
  std::vector<Node> children;
  const Node* child(const char* t) const {
    for (const auto& c : children)
      if (c.type == t) return &c;
    return nullptr;
  }
};

bool isContainer(const std::string& t) {
  return t == "moov" || t == "trak" || t == "mdia" || t == "minf" || t == "stbl" || t == "edts" || t == "dinf";
}

bool parse(const uint8_t* p, size_t n, std::vector<Node>* out) {
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
    Node b;
    b.type.assign(reinterpret_cast<const char*>(p + off + 4), 4);
    b.body = p + off + header;
    b.size = size_t(size) - header;
    if (isContainer(b.type) && !parse(b.body, b.size, &b.children)) return false;
    out->push_back(std::move(b));
    off += size_t(size);
  }
  return true;
}

struct Chunk {
  uint64_t offset, bytes;
  uint64_t firstSample;
  uint32_t samples;
};

}  // namespace

bool checkMp4File(const std::string& path, Mp4Summary* summary, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = "MP4 check: " + m;
    return false;
  };
  Mp4Summary local;
  Mp4Summary& sum = summary ? *summary : local;
  sum = Mp4Summary{};
  const fs::path fp = fs::u8path(path);
  std::error_code ec;
  const uint64_t fileSize = fs::file_size(fp, ec);
  if (ec) return fail("cannot read the file size");
  sum.fileSize = fileSize;
  std::ifstream f(fp, std::ios::binary);
  if (!f) return fail("cannot open the file");

  // ---- top level: boxes back to back up to the end of the file, one moov, the mdat ranges
  std::vector<std::pair<uint64_t, uint64_t>> mdats;  // payload [begin, end)
  uint64_t off = 0, moovOff = 0, moovSize = 0, moovHeader = 0;
  int moovCount = 0;
  while (off < fileSize) {
    if (fileSize - off < 8) return fail("truncated box header at " + std::to_string(off));
    uint8_t h[16];
    f.seekg(std::streamoff(off));
    f.read(reinterpret_cast<char*>(h), 16);
    if (f.gcount() < 8) return fail("cannot read at " + std::to_string(off));
    f.clear();
    uint64_t size = rd32(h);
    uint64_t header = 8;
    if (size == 1) {
      if (f.gcount() < 16) return fail("truncated box header at " + std::to_string(off));
      size = rd64(h + 8);
      header = 16;
    } else if (size == 0) {
      return fail("unterminated box (size 0) at " + std::to_string(off) + " - the file was never finished");
    }
    if (size < header || size > fileSize - off)
      return fail("box '" + std::string(reinterpret_cast<char*>(h + 4), 4) + "' at " + std::to_string(off) + " is " +
                  std::to_string(size) + " bytes, past the end of the file (" + std::to_string(fileSize) + ")");
    if (std::memcmp(h + 4, "moov", 4) == 0) {
      ++moovCount;
      moovOff = off;
      moovSize = size;
      moovHeader = header;
    } else if (std::memcmp(h + 4, "mdat", 4) == 0) {
      mdats.emplace_back(off + header, off + size);
      sum.mdatBytes += size - header;
    } else if (std::memcmp(h + 4, "moof", 4) == 0) {
      return fail("fragmented MP4 (moof) is not expected");
    }
    off += size;
  }
  if (moovCount == 0) return fail("no moov box - the file was never finished");
  if (moovCount > 1) return fail("more than one moov box");
  if (mdats.empty()) return fail("no mdat box");
  sum.moovAtEnd = moovOff + moovSize == fileSize;
  if (moovSize > (uint64_t(1) << 30)) return fail("moov box too large");
  std::vector<uint8_t> raw(size_t(moovSize - moovHeader));
  f.seekg(std::streamoff(moovOff + moovHeader));
  f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
  if (size_t(f.gcount()) != raw.size()) return fail("cannot read moov");
  f.clear();
  Node moov;
  moov.type = "moov";
  if (!parse(raw.data(), raw.size(), &moov.children)) return fail("cannot parse moov");

  std::vector<std::pair<uint64_t, uint64_t>> allChunks;  // [offset, end) of every chunk, all tracks
  for (const Node& trak : moov.children) {
    if (trak.type != "trak") continue;
    Mp4TrackSummary t;
    const Node* mdia = trak.child("mdia");
    const Node* hdlr = mdia ? mdia->child("hdlr") : nullptr;
    const Node* mdhd = mdia ? mdia->child("mdhd") : nullptr;
    const Node* minf = mdia ? mdia->child("minf") : nullptr;
    const Node* stbl = minf ? minf->child("stbl") : nullptr;
    if (!hdlr || hdlr->size < 12 || !mdhd || mdhd->size < 20 || !stbl) return fail("incomplete track");
    t.handler.assign(reinterpret_cast<const char*>(hdlr->body + 8), 4);
    if (mdhd->body[0] == 1) {
      if (mdhd->size < 32) return fail("bad mdhd");
      t.timescale = rd32(mdhd->body + 20);
      t.duration = rd64(mdhd->body + 24);
    } else {
      t.timescale = rd32(mdhd->body + 12);
      t.duration = rd32(mdhd->body + 16);
    }
    const std::string who = "track '" + t.handler + "': ";
    const Node* stsd = stbl->child("stsd");
    const Node* stsz = stbl->child("stsz");
    const Node* stsc = stbl->child("stsc");
    const Node* stco = stbl->child("stco");
    const Node* co64 = stbl->child("co64");
    if (!stsd || !stsz || !stsc || (!stco && !co64)) return fail(who + "incomplete sample table");
    if (stco && co64) return fail(who + "both stco and co64");
    // Sample entry (codec) and, for H.264, the NAL length size.
    int nalLength = 0;
    if (stsd->size >= 16) {
      const uint8_t* e = stsd->body + 8;
      const uint32_t esize = rd32(e);
      t.codec.assign(reinterpret_cast<const char*>(e + 4), 4);
      if (esize >= 8 && esize <= stsd->size - 8 && (t.codec == "avc1" || t.codec == "avc3")) {
        // VisualSampleEntry: 8 header + 78 bytes, then the child boxes (avcC, colr, pasp, ...).
        std::vector<Node> kids;
        if (esize < 86 || !parse(e + 86, esize - 86, &kids)) return fail(who + "bad avc1 sample entry");
        for (const auto& k : kids)
          if (k.type == "avcC" && k.size >= 5) nalLength = (k.body[4] & 3) + 1;
        if (!nalLength) return fail(who + "no avcC");
      }
    }
    // Sample sizes.
    if (stsz->size < 12) return fail(who + "bad stsz");
    const uint32_t fixedSize = rd32(stsz->body + 4);
    const uint32_t sampleCount = rd32(stsz->body + 8);
    if (!fixedSize && stsz->size < 12 + size_t(sampleCount) * 4) return fail(who + "bad stsz");
    auto sampleSize = [&](uint64_t i) -> uint64_t { return fixedSize ? fixedSize : rd32(stsz->body + 12 + i * 4); };
    t.samples = sampleCount;
    // Chunk offsets.
    const Node* co = co64 ? co64 : stco;
    t.co64 = co64 != nullptr;
    if (co->size < 8) return fail(who + "bad chunk offsets");
    const uint32_t chunkCount = rd32(co->body + 4);
    const size_t es = co64 ? 8 : 4;
    if (co->size < 8 + size_t(chunkCount) * es) return fail(who + "bad chunk offsets");
    t.chunks = chunkCount;
    // Samples per chunk (stsc runs).
    if (stsc->size < 8) return fail(who + "bad stsc");
    const uint32_t runs = rd32(stsc->body + 4);
    if (stsc->size < 8 + size_t(runs) * 12 || (chunkCount && !runs)) return fail(who + "bad stsc");
    std::vector<Chunk> chunks(chunkCount);
    uint64_t sample = 0;
    for (uint32_t r = 0; r < runs; ++r) {
      const uint8_t* e = stsc->body + 8 + size_t(r) * 12;
      const uint32_t first = rd32(e), per = rd32(e + 4);
      const uint32_t next = r + 1 < runs ? rd32(e + 12) : chunkCount + 1;
      if (first < 1 || next <= first || next > chunkCount + 1 || (r == 0 && first != 1)) return fail(who + "bad stsc run");
      for (uint32_t c = first; c < next; ++c) {
        Chunk& ch = chunks[c - 1];
        ch.offset = co64 ? rd64(co->body + 8 + size_t(c - 1) * 8) : rd32(co->body + 8 + size_t(c - 1) * 4);
        ch.firstSample = sample;
        ch.samples = per;
        ch.bytes = 0;
        for (uint32_t k = 0; k < per; ++k) {
          if (sample >= sampleCount) return fail(who + "the chunks hold more samples than stsz lists");
          ch.bytes += sampleSize(sample++);
        }
      }
    }
    if (sample != sampleCount) return fail(who + "the chunks hold " + std::to_string(sample) + " of " + std::to_string(sampleCount) + " samples");
    for (const Chunk& ch : chunks) {
      bool inside = false;
      for (const auto& m : mdats) inside |= ch.offset >= m.first && ch.offset <= m.second && ch.bytes <= m.second - ch.offset;
      if (!inside) return fail(who + "a chunk at " + std::to_string(ch.offset) + " is outside the media data");
      allChunks.emplace_back(ch.offset, ch.offset + ch.bytes);
      t.dataEnd = std::max(t.dataEnd, ch.offset + ch.bytes);
    }
    // H.264: the sampled frames must be NAL units back to back (catches offsets that point at the
    // wrong place, e.g. 32-bit offsets that wrapped past 4 GiB).
    if (nalLength) {
      auto checkSample = [&](uint64_t at, uint64_t size) -> bool {
        uint64_t pos = 0;
        uint8_t h[5];
        while (pos < size) {
          if (size - pos < uint64_t(nalLength) + 1) return false;
          f.seekg(std::streamoff(at + pos));
          f.read(reinterpret_cast<char*>(h), nalLength + 1);
          if (f.gcount() != nalLength + 1) return false;
          uint64_t len = 0;
          for (int i = 0; i < nalLength; ++i) len = len << 8 | h[i];
          const uint8_t nal = h[nalLength];
          if (len == 0 || (nal & 0x80) || (nal & 0x1F) == 0 || len > size - pos - uint64_t(nalLength)) return false;
          pos += uint64_t(nalLength) + len;
        }
        return true;
      };
      for (const Chunk& ch : chunks) {
        if (!ch.samples) continue;
        if (!checkSample(ch.offset, sampleSize(ch.firstSample))) {
          f.clear();
          return fail(who + "sample " + std::to_string(ch.firstSample) + " at " + std::to_string(ch.offset) +
                      " is not H.264 data (wrong chunk offset)");
        }
      }
      if (!chunks.empty()) {
        const Chunk& last = chunks.back();
        uint64_t at = last.offset;
        for (uint32_t k = 0; k + 1 < last.samples; ++k) at += sampleSize(last.firstSample + k);
        if (last.samples && !checkSample(at, sampleSize(last.firstSample + last.samples - 1))) {
          f.clear();
          return fail(who + "the last sample is not H.264 data");
        }
      }
      f.clear();
    }
    sum.tracks.push_back(t);
  }
  if (sum.tracks.empty()) return fail("no tracks");
  std::sort(allChunks.begin(), allChunks.end());
  for (size_t i = 1; i < allChunks.size(); ++i)
    if (allChunks[i].first < allChunks[i - 1].second) return fail("chunks overlap at " + std::to_string(allChunks[i].first));
  return true;
}

}  // namespace rnl
