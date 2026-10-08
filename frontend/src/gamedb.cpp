// Game database (rnf_gamedb_*): the catalogue compiled in from frontend/data/nesdb.tsv (see
// tools/nesdb/README.md for the sources and the format), ROM identification by PRG+CHR hashes and
// by file name, and the localized display strings of a game.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "collate.hpp"
#include "common.hpp"
#include "util/Hash.h"

#include "nesdb_data.inc"  // kNesdbData / kNesdbSize (generated from frontend/data/nesdb.tsv)

using namespace rnf;

namespace {

// ------------------------------------------------------------------ SHA-1 (FIPS 180-4)

class Sha1 {
 public:
  void update(const uint8_t* p, size_t n) {
    total_ += n;
    while (n > 0) {
      size_t take = std::min(n, size_t(64) - len_);
      std::memcpy(buf_ + len_, p, take);
      len_ += take;
      p += take;
      n -= take;
      if (len_ == 64) {
        block(buf_);
        len_ = 0;
      }
    }
  }
  std::string hexUpper() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (len_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    char out[41];
    for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08X", h_[i]);
    return std::string(out, 40);
  }

 private:
  static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
  void block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
  }
  uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t buf_[64];
  size_t len_ = 0;
  uint64_t total_ = 0;
};

// The PRG+CHR range of a ROM image ([begin, end) into data). Headerless: everything.
void romDataRange(const uint8_t* data, size_t size, size_t* begin, size_t* end) {
  *begin = 0;
  *end = size;
  if (size < 16 || std::memcmp(data, "NES\x1a", 4) != 0) return;
  size_t start = 16 + ((data[6] & 0x04) ? 512 : 0);
  uint64_t prg = data[4], chr = data[5];
  if ((data[7] & 0x0C) == 0x08) {  // NES 2.0: size MSBs (plain multiples only)
    if ((data[9] & 0x0F) != 0x0F) prg |= uint64_t(data[9] & 0x0F) << 8;
    if ((data[9] & 0xF0) != 0xF0) chr |= uint64_t(data[9] & 0xF0) << 4;
  }
  uint64_t want = prg * 16384 + chr * 8192;
  *begin = std::min(start, size);
  *end = (want == 0 || *begin + want > size) ? size : size_t(*begin + want);
}

// ------------------------------------------------------------------ database

struct Game {
  std::string id, en, ja, reading, publisher, region;
  int year = 0;
  rnf_genre genre = RNF_GENRE_UNKNOWN;
  std::vector<std::string> aliases;
};

struct Db {
  std::vector<Game> games;
  std::map<std::string, std::pair<std::string, std::string>> publishers;  // key -> (en, ja)
  std::vector<rnf_game_info> infos;                                       // parallel to games
  std::unordered_map<std::string, size_t> byId;
  std::unordered_multimap<uint32_t, std::pair<std::string, size_t>> byCrc;  // crc -> (sha1, game)
  std::unordered_map<std::string, size_t> bySha;
  std::unordered_map<std::string, std::vector<size_t>> byName, byMainName;
};

const char* const kGenreCodes[RNF_GENRE_COUNT] = {"",       "action", "shooter",  "puzzle", "rpg",   "adventure",   "sports",
                                                  "racing", "fighting", "strategy", "table", "music", "educational", "other"};

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

}  // namespace

namespace rnf {

// Matching key of a title or file name: width folded, ASCII lower-cased, "&" read as "and", a
// leading "The " / trailing ", The" dropped, katakana folded to hiragana, everything but letters,
// digits and kana / kanji removed.
std::string nameKey(const std::string& text) {
  std::string s;
  size_t i = 0;
  while (i < text.size()) {
    uint32_t cp = foldWidth(nextCodePoint(text, i));
    if (cp < 0x80) s += char(std::tolower(int(cp)));
    else appendUtf8(s, cp);
  }
  if (s.size() > 4 && s.compare(0, 4, "the ") == 0) s = s.substr(4);
  if (s.size() > 5 && s.compare(s.size() - 5, 5, ", the") == 0) s.resize(s.size() - 5);
  std::string out;
  i = 0;
  while (i < s.size()) {
    uint32_t cp = nextCodePoint(s, i);
    if (cp == '&') {
      out += "and";
    } else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
      out += char(cp);
    } else if (cp >= 0x80) {
      if (cp == 0x30FB || cp == 0x30FC || (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) ||
          (cp >= 0xA0 && cp <= 0xBF) || cp == 0xD7)
        continue;
      appendUtf8(out, katakanaToHiragana(cp));
    }
  }
  return out;
}

// The part before a subtitle (": " / " - "), "" when there is none.
std::string mainTitle(const std::string& t) {
  size_t p = std::string::npos;
  for (const char* sep : {": ", " - ", "\xEF\xBC\x9A", " \xEF\xBD\x9E", "\xEF\xBD\x9E"}) {
    size_t q = t.find(sep);
    if (q != std::string::npos && q > 0) p = std::min(p, q);
  }
  return p == std::string::npos ? "" : t.substr(0, p);
}

}  // namespace rnf

namespace {

void addName(std::unordered_map<std::string, std::vector<size_t>>& m, const std::string& text, size_t idx) {
  std::string k = nameKey(text);
  if (k.empty()) return;
  std::vector<size_t>& v = m[k];
  if (std::find(v.begin(), v.end(), idx) == v.end()) v.push_back(idx);
}

const Db& db() {
  static Db* d = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    d = new Db;
    std::string text(reinterpret_cast<const char*>(kNesdbData), kNesdbSize);
    for (const std::string& raw : split(text, '\n')) {
      std::string line = raw;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      std::vector<std::string> f = split(line, '\t');
      if (f[0] == "P" && f.size() >= 4) {
        d->publishers[f[1]] = {f[2], f[3]};
      } else if (f[0] == "G" && f.size() >= 10) {
        Game g;
        g.id = f[1];
        g.en = f[2];
        g.ja = f[3];
        g.reading = f[4];
        g.publisher = f[5];
        g.year = std::atoi(f[6].c_str());
        g.genre = rnf_genre_from_code(f[7].c_str());
        g.region = f[8];
        if (!f[9].empty()) g.aliases = split(f[9], '|');
        if (g.ja.empty()) g.ja = g.en;
        if (g.en.empty()) g.en = g.ja;
        d->byId[g.id] = d->games.size();
        d->games.push_back(std::move(g));
      } else if (f[0] == "H" && f.size() >= 4) {
        auto it = d->byId.find(f[3]);
        if (it == d->byId.end()) continue;
        uint32_t crc = uint32_t(std::strtoul(f[1].c_str(), nullptr, 16));
        std::string sha = f[2];
        for (char& c : sha) c = char(std::toupper(static_cast<unsigned char>(c)));
        d->byCrc.emplace(crc, std::make_pair(sha, it->second));
        if (!sha.empty()) d->bySha[sha] = it->second;
      }
    }
    static const std::string kEmpty;
    d->infos.resize(d->games.size());
    for (size_t i = 0; i < d->games.size(); ++i) {
      const Game& g = d->games[i];
      auto pub = d->publishers.find(g.publisher);
      const std::string& pen = pub == d->publishers.end() ? kEmpty : pub->second.first;
      const std::string& pja = pub == d->publishers.end() ? kEmpty : (pub->second.second.empty() ? pub->second.first : pub->second.second);
      d->infos[i] = rnf_game_info{g.id.c_str(), g.en.c_str(), g.ja.c_str(), g.reading.c_str(), pen.c_str(), pja.c_str(),
                                  g.year,       g.genre,      g.region.c_str()};
      addName(d->byName, g.en, i);
      addName(d->byName, g.ja, i);
      for (const std::string& a : g.aliases) addName(d->byName, a, i);
      for (const std::string* t : {&g.en, &g.ja}) {
        std::string m = mainTitle(*t);
        if (!m.empty()) addName(d->byMainName, m, i);
      }
      for (const std::string& a : g.aliases) {
        std::string m = mainTitle(a);
        if (!m.empty()) addName(d->byMainName, m, i);
      }
    }
  });
  return *d;
}

bool give(size_t idx, rnf_game_info* out) {
  const Db& d = db();
  if (idx >= d.infos.size()) return false;
  if (out) *out = d.infos[idx];
  return true;
}

// ------------------------------------------------------------------ file names

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t_");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t_");
  return s.substr(a, b - a + 1);
}

std::string withoutExtension(const std::string& name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || name.size() - dot > 5) return name;
  for (size_t i = dot + 1; i < name.size(); ++i)
    if (!std::isalnum(static_cast<unsigned char>(name[i]))) return name;
  return name.substr(0, dot);
}

// Region of a file-name tag ("Japan", "USA, Europe", "J", "JU" ...): bit 1 JP, 2 NA, 4 EU; 8 = world.
int regionOfTag(const std::string& tag) {
  int r = 0;
  for (std::string part : split(tag, ',')) {
    part = trim(part);
    std::string l = lower(part);
    if (l == "japan" || l == "j" || l == "jp" || l == "jpn") r |= 1;
    else if (l == "usa" || l == "u" || l == "us" || l == "canada") r |= 2;
    else if (l == "europe" || l == "e" || l == "eu" || l == "uk" || l == "germany" || l == "france" || l == "spain" || l == "italy" ||
             l == "sweden" || l == "australia" || l == "netherlands")
      r |= 4;
    else if (l == "world" || l == "w") r |= 8;
    else if (l == "ju" || l == "uj") r |= 3;
    else if (l == "ue" || l == "eu") r |= 6;
    else if (l == "jue") r |= 7;
    else return 0;  // not a region tag
  }
  return r;
}

bool isNoiseTag(const std::string& tag, bool square) {
  if (square) return true;  // [!], [a1], [T+Eng] ...: dump / hack info
  std::string l = lower(trim(tag));
  if (l.empty() || regionOfTag(l)) return true;
  static const char* const kWords[] = {"rev", "v1", "v2", "v0", "proto", "prototype", "beta", "sample", "demo", "unl", "alt",
                                       "virtual console", "switch online", "hack", "pirate", "aftermarket", "prg0", "prg1",
                                       "namcot collection", "namco museum archives", "retro-bit", "limited run", "en", "ja", "fr",
                                       "de", "es", "it", "nl", "sv", "pt", "translated", "t+", "t-", "kiosk", "possible proto"};
  for (const char* w : kWords) {
    size_t n = std::strlen(w);
    if (l.compare(0, n, w) == 0 && (l.size() == n || !std::isalpha(static_cast<unsigned char>(l[n])) || std::strcmp(w, "t+") == 0))
      return true;
  }
  // Language lists: "En,Ja,Fr".
  bool lang = true;
  for (std::string p : split(l, ',')) {
    p = trim(p);
    if (p.size() != 2 || !std::isalpha(static_cast<unsigned char>(p[0])) || !std::isalpha(static_cast<unsigned char>(p[1]))) lang = false;
  }
  return lang;
}

struct ParsedName {
  std::string base;      // title without tags
  std::string specific;  // base + the tags that are part of the name ("Tetris (Bulletproof)")
  int region = 0;
};

ParsedName parseFileName(const std::string& fileName) {
  std::string s = trim(withoutExtension(fileName));
  std::vector<std::pair<std::string, bool>> tags;  // in order
  std::string base;
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c == '(' || c == '[') {
      char close = c == '(' ? ')' : ']';
      size_t j = s.find(close, i + 1);
      if (j == std::string::npos) {
        base += s.substr(i);
        break;
      }
      tags.push_back({s.substr(i + 1, j - i - 1), c == '['});
      i = j + 1;
      continue;
    }
    base += c;  // text after a tag stays part of the name
    ++i;
  }
  ParsedName p;
  std::string collapsed;
  for (char c : base)
    if (!(c == ' ' && !collapsed.empty() && collapsed.back() == ' ')) collapsed += c;
  p.base = trim(collapsed);
  std::string specific = p.base;
  for (const auto& t : tags) {
    int r = regionOfTag(t.first);
    if (!t.second && r) p.region |= r;
    if (!isNoiseTag(t.first, t.second)) specific += " (" + t.first + ")";
  }
  p.specific = specific;
  return p;
}

int regionBits(const std::string& region) {
  int r = 0;
  for (const std::string& p : split(region, ',')) {
    if (p == "JP") r |= 1;
    else if (p == "NA") r |= 2;
    else if (p == "EU") r |= 4;
  }
  return r;
}

// The best of several games with the same name for a file of the given region.
size_t pick(const std::vector<size_t>& cands, int region) {
  const Db& d = db();
  if (region && !(region & 8)) {
    for (size_t c : cands)
      if (regionBits(d.games[c].region) & region) return c;
  }
  return cands.front();
}

bool findByName(const std::string& fileName, size_t* idx) {
  const Db& d = db();
  ParsedName p = parseFileName(fileName);
  std::vector<std::string> tries = {p.specific, p.base};
  for (const std::string& t : tries) {
    auto it = d.byName.find(nameKey(t));
    if (it != d.byName.end() && !it->second.empty()) {
      *idx = pick(it->second, p.region);
      return true;
    }
  }
  // "Abadox - The Deadly Inner War" vs "Abadox": the main titles.
  std::string m = mainTitle(p.base);
  for (const std::string& t : {p.base, m}) {
    if (t.empty()) continue;
    auto it = d.byMainName.find(nameKey(t));
    if (it != d.byMainName.end() && !it->second.empty()) {
      *idx = pick(it->second, p.region);
      return true;
    }
    auto it2 = d.byName.find(nameKey(t));
    if (it2 != d.byName.end() && !it2->second.empty()) {
      *idx = pick(it2->second, p.region);
      return true;
    }
  }
  return false;
}

bool findByCrc(uint32_t crc, const std::string* sha1, size_t* idx) {
  const Db& d = db();
  auto range = d.byCrc.equal_range(crc);
  for (auto it = range.first; it != range.second; ++it) {
    if (sha1 && !it->second.first.empty() && it->second.first != *sha1) continue;
    *idx = it->second.second;
    return true;
  }
  return false;
}

std::string lastPathComponent(const std::string& path) {
  size_t i = path.size();
  while (i > 0 && !isPathSep(path[i - 1])) --i;
  return path.substr(i);
}

}  // namespace

namespace rnf {

const rnf_game_info* gameInfoAt(size_t idx) { return idx < db().infos.size() ? &db().infos[idx] : nullptr; }

std::string titleFromFileName(const std::string& fileName) {
  ParsedName p = parseFileName(lastPathComponent(fileName));
  std::string t = p.base;
  if (t.size() > 5) {
    std::string tail = lower(t.substr(t.size() - 5));
    if (tail == ", the") t = "The " + t.substr(0, t.size() - 5);
  }
  return t.empty() ? trim(withoutExtension(lastPathComponent(fileName))) : t;
}

}  // namespace rnf

extern "C" {

size_t rnf_gamedb_count(void) {
  try {
    return db().games.size();
  } catch (...) {
    return 0;
  }
}

int rnf_gamedb_get(size_t index, rnf_game_info* out) {
  try {
    return give(index, out) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

int rnf_gamedb_find_id(const char* id, rnf_game_info* out) {
  if (!id) return 0;
  try {
    auto it = db().byId.find(id);
    return it != db().byId.end() && give(it->second, out) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

int rnf_gamedb_find_crc32(uint32_t crc, rnf_game_info* out) {
  try {
    size_t idx;
    return findByCrc(crc, nullptr, &idx) && give(idx, out) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

int rnf_gamedb_find_sha1(const char* sha1_hex, rnf_game_info* out) {
  if (!sha1_hex) return 0;
  try {
    std::string s = sha1_hex;
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    auto it = db().bySha.find(s);
    return it != db().bySha.end() && give(it->second, out) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

int rnf_gamedb_find_name(const char* file_name, rnf_game_info* out) {
  if (!file_name) return 0;
  try {
    size_t idx;
    return findByName(file_name, &idx) && give(idx, out) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

uint32_t rnf_rom_data_crc32(const uint8_t* data, size_t size) {
  if (!data) return 0;
  size_t a, b;
  romDataRange(data, size, &a, &b);
  return rn::crc32(data + a, b - a);
}

void rnf_rom_data_sha1(const uint8_t* data, size_t size, char out_hex[41]) {
  if (!out_hex) return;
  out_hex[0] = 0;
  if (!data) return;
  size_t a, b;
  romDataRange(data, size, &a, &b);
  Sha1 h;
  h.update(data + a, b - a);
  std::string hex = h.hexUpper();
  std::memcpy(out_hex, hex.c_str(), 41);
}

rnf_game_match rnf_gamedb_identify(const uint8_t* data, size_t size, const char* file_name, rnf_game_info* out) {
  try {
    if (data && size > 0) {
      size_t a, b;
      romDataRange(data, size, &a, &b);
      uint32_t crc = rn::crc32(data + a, b - a);
      auto range = db().byCrc.equal_range(crc);
      if (range.first != range.second) {
        Sha1 h;
        h.update(data + a, b - a);
        std::string sha = h.hexUpper();
        size_t idx;
        if (findByCrc(crc, &sha, &idx) && give(idx, out)) return RNF_GAME_MATCH_HASH;
      }
      // Everything after the header (extra data past CHR), then the whole file (headered dumps).
      size_t idx;
      if (a < size && b != size && findByCrc(rn::crc32(data + a, size - a), nullptr, &idx) && give(idx, out)) return RNF_GAME_MATCH_HASH;
      if (a > 0 && findByCrc(rn::crc32(data, size), nullptr, &idx) && give(idx, out)) return RNF_GAME_MATCH_HASH;
    }
    if (file_name && *file_name) {
      size_t idx;
      if (findByName(lastPathComponent(file_name), &idx) && give(idx, out)) return RNF_GAME_MATCH_NAME;
    }
  } catch (...) {
  }
  return RNF_GAME_MATCH_NONE;
}

rnf_game_match rnf_gamedb_identify_file(const char* path, rnf_game_info* out) {
  if (!path) return RNF_GAME_MATCH_NONE;
  try {
    std::vector<uint8_t> bytes;
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(std::filesystem::u8path(path).c_str(), L"rb");
#else
    f = std::fopen(path, "rb");
#endif
    if (f) {
      uint8_t buf[65536];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() < (8u << 20)) bytes.insert(bytes.end(), buf, buf + n);
      std::fclose(f);
    }
    return rnf_gamedb_identify(bytes.data(), bytes.size(), path, out);
  } catch (...) {
    return RNF_GAME_MATCH_NONE;
  }
}

const char* rnf_genre_code(rnf_genre g) { return (int(g) >= 0 && g < RNF_GENRE_COUNT) ? kGenreCodes[g] : ""; }

rnf_genre rnf_genre_from_code(const char* code) {
  if (!code || !*code) return RNF_GENRE_UNKNOWN;
  for (int i = 1; i < RNF_GENRE_COUNT; ++i)
    if (std::strcmp(kGenreCodes[i], code) == 0) return rnf_genre(i);
  return RNF_GENRE_OTHER;
}

const char* rnf_genre_name(rnf_genre g) {
  switch (g) {
    case RNF_GENRE_ACTION: return tr(RNF_L("Action (genre)"));
    case RNF_GENRE_SHOOTER: return tr(RNF_L("Shooter (genre)"));
    case RNF_GENRE_PUZZLE: return tr(RNF_L("Puzzle (genre)"));
    case RNF_GENRE_RPG: return tr(RNF_L("RPG (genre)"));
    case RNF_GENRE_ADVENTURE: return tr(RNF_L("Adventure (genre)"));
    case RNF_GENRE_SPORTS: return tr(RNF_L("Sports (genre)"));
    case RNF_GENRE_RACING: return tr(RNF_L("Racing (genre)"));
    case RNF_GENRE_FIGHTING: return tr(RNF_L("Fighting (genre)"));
    case RNF_GENRE_STRATEGY: return tr(RNF_L("Strategy (genre)"));
    case RNF_GENRE_TABLE: return tr(RNF_L("Table Games (genre)"));
    case RNF_GENRE_MUSIC: return tr(RNF_L("Music (genre)"));
    case RNF_GENRE_EDUCATIONAL: return tr(RNF_L("Educational (genre)"));
    case RNF_GENRE_OTHER: return tr(RNF_L("Other (genre)"));
    default: return "";
  }
}

char* rnf_title_from_file_name(const char* file_name) {
  try {
    return dup(titleFromFileName(file_name ? file_name : ""));
  } catch (...) {
    return nullptr;
  }
}

}  // extern "C"
