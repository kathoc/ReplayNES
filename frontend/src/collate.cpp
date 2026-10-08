// Title collation for the library (rnf_collate): Japanese gojūon order of kana readings and an
// English order that ignores case and a leading "The". Self-contained (no ICU, identical on every
// platform).
//
// Japanese (after JIS X 4061, simplified): a title is read as a sequence of elements.
//   - Kana (hiragana, katakana, half-width katakana) -> the plain hiragana letter: katakana folded to
//     hiragana, small kana to their full size (small a -> a, small tsu -> tsu), dakuten / handakuten
//     removed (ga -> ka, pa -> ha, vu -> u). The removed distinctions only break ties (secondary level).
//   - The long vowel mark (U+30FC) is read as the vowel of the kana before it (su-pa- -> su u ha a); the
//     iteration marks (U+309D / U+309E) repeat it.
//   - Digit runs (and the numerals Ⅰ...Ⅻ) compare by value (2 < 10); Latin letters (ASCII and full-width) case-insensitively.
//   - Spaces, punctuation and symbols are ignored.
//   - Groups: kana < digits < Latin < everything else (kanji: by code point, there is no reading).
// English: digits < Latin < kana < others, a leading "The " is ignored.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "collate.hpp"
#include "common.hpp"

namespace rnf {

uint32_t nextCodePoint(const std::string& s, size_t& i) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  auto cont = [&](size_t k) -> uint32_t {
    return i + k < s.size() ? (static_cast<unsigned char>(s[i + k]) & 0x3F) : 0;
  };
  uint32_t cp;
  size_t n;
  if (c < 0x80) cp = c, n = 1;
  else if ((c & 0xE0) == 0xC0) cp = ((c & 0x1F) << 6) | cont(1), n = 2;
  else if ((c & 0xF0) == 0xE0) cp = ((c & 0x0F) << 12) | (cont(1) << 6) | cont(2), n = 3;
  else if ((c & 0xF8) == 0xF0) cp = ((c & 0x07) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3), n = 4;
  else cp = 0xFFFD, n = 1;
  i = std::min(s.size(), i + n);
  return cp;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

namespace {

// Half-width katakana U+FF66...U+FF9D -> full-width katakana.
const char16_t kHalfKana[] = u"\u30F2\u30A1\u30A3\u30A5\u30A7\u30A9\u30E3\u30E5\u30E7\u30C3\u30FC\u30A2\u30A4\u30A6\u30A8\u30AA\u30AB\u30AD\u30AF\u30B1\u30B3\u30B5\u30B7\u30B9\u30BB\u30BD\u30BF\u30C1\u30C4\u30C6\u30C8\u30CA\u30CB\u30CC\u30CD\u30CE\u30CF\u30D2\u30D5\u30D8\u30DB\u30DE\u30DF\u30E0\u30E1\u30E2\u30E4\u30E6\u30E8\u30E9\u30EA\u30EB\u30EC\u30ED\u30EF\u30F3";

}  // namespace

uint32_t foldWidth(uint32_t cp) {
  if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFF01 + 0x21;  // full-width ASCII
  if (cp == 0x3000) return 0x20;                                  // ideographic space
  if (cp >= 0xFF66 && cp <= 0xFF9D) return kHalfKana[cp - 0xFF66];
  return cp;
}

bool isKana(uint32_t cp) {
  return (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) || cp == 0x30FC || cp == 0x309D || cp == 0x309E ||
         cp == 0x30FD || cp == 0x30FE || (cp >= 0xFF66 && cp <= 0xFF9F);
}

uint32_t katakanaToHiragana(uint32_t cp) { return (cp >= 0x30A1 && cp <= 0x30F6) ? cp - 0x60 : cp; }
uint32_t hiraganaToKatakana(uint32_t cp) { return (cp >= 0x3041 && cp <= 0x3096) ? cp + 0x60 : cp; }

namespace {

// Plain hiragana letter of a hiragana code point and the removed mark:
// 0 plain, 1 small, 2 dakuten, 3 handakuten.
struct Base {
  uint32_t cp;
  int mark;
};
Base baseOf(uint32_t h) {
  switch (h) {
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:  // small a i u e o
      return {h + 1, 1};
    case 0x3063: return {0x3064, 1};                                 // small tsu
    case 0x3083: case 0x3085: case 0x3087: return {h + 1, 1};       // small ya yu yo
    case 0x308E: return {0x308F, 1};                                 // small wa
    case 0x3095: return {0x304B, 1};                                 // small ka
    case 0x3096: return {0x3051, 1};                                 // small ke
    case 0x3094: return {0x3046, 2};                                 // vu
    default: break;
  }
  // ka (304B) ... di (3062): plain at even offsets from ka, voiced right after.
  if (h >= 0x304B && h <= 0x3062) return (h - 0x304B) % 2 == 0 ? Base{h, 0} : Base{h - 1, 2};
  // tsu (3064) du, te de, to do.
  if (h >= 0x3064 && h <= 0x3069) return (h - 0x3064) % 2 == 0 ? Base{h, 0} : Base{h - 1, 2};
  // ha (306F) ba pa ... ho bo po (307D).
  if (h >= 0x306F && h <= 0x307D) {
    int k = int(h - 0x306F) % 3;
    return Base{h - uint32_t(k), k == 0 ? 0 : k == 1 ? 2 : 3};
  }
  return {h, 0};
}

// The vowel (hiragana a i u e o) of a plain hiragana letter, 0 for n / others.
uint32_t vowelOf(uint32_t b) {
  static const char16_t kRows[] = u"\u3042\u3044\u3046\u3048\u304A\u304B\u304D\u304F\u3051\u3053\u3055\u3057\u3059\u305B\u305D\u305F\u3061\u3064\u3066\u3068\u306A\u306B\u306C\u306D\u306E\u306F\u3072\u3075\u3078\u307B\u307E\u307F\u3080\u3081\u3082\u3089\u308A\u308B\u308C\u308D";
  static const char16_t kVowels[] = u"\u3042\u3044\u3046\u3048\u304A";
  for (size_t i = 0; kRows[i]; ++i)
    if (kRows[i] == b) return kVowels[i % 5];
  switch (b) {
    case 0x3084: return 0x3042;  // ya
    case 0x3086: return 0x3046;  // yu
    case 0x3088: return 0x304A;  // yo
    case 0x308F: return 0x3042;  // wa
    case 0x3090: return 0x3044;  // wi
    case 0x3091: return 0x3048;  // we
    case 0x3092: return 0x304A;  // wo
    default: return 0;
  }
}

bool isAsciiAlpha(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(uint32_t c) { return c >= '0' && c <= '9'; }

bool isIgnorable(uint32_t c) {
  if (c < 0x80) return !isAsciiAlpha(c) && !isDigit(c);
  if (c >= 0x2000 && c <= 0x2BFF) return true;   // general punctuation, arrows, symbols, stars ...
  if (c >= 0x3000 && c <= 0x303F) return true;   // CJK symbols and punctuation (、。「」〜 ...)
  if (c == 0x30FB || c == 0xFF65) return true;    // middle dot
  if (c == 0x00B7 || c == 0x00D7 || c == 0x00A0) return true;
  if (c >= 0x00A1 && c <= 0x00BF) return true;
  if (c >= 0xFE30 && c <= 0xFE4F) return true;
  return false;
}

}  // namespace

std::vector<CollationElement> collationElements(const std::string& text, bool japanese) {
  std::vector<CollationElement> out;
  const int gKana = japanese ? 0 : 2, gDigit = japanese ? 1 : 0, gLatin = japanese ? 2 : 1, gOther = 3;
  uint32_t lastBase = 0;  // the previous kana letter (for the long vowel / iteration marks)
  size_t i = 0;
  std::string s = text;
  if (!japanese) {
    // A leading "The " is ignored.
    if (s.size() > 4 && (s[0] == 'T' || s[0] == 't') && (s[1] == 'h' || s[1] == 'H') && (s[2] == 'e' || s[2] == 'E') && s[3] == ' ')
      s = s.substr(4);
  }
  while (i < s.size()) {
    uint32_t cp = foldWidth(nextCodePoint(s, i));
    if (cp == 0xFF9E || cp == 0xFF9F || cp == 0x309B || cp == 0x309C || cp == 0x3099 || cp == 0x309A) {
      // A separate (half-width) dakuten / handakuten: marks the previous kana.
      if (!out.empty() && out.back().group == gKana) out.back().secondary = (cp == 0xFF9F || cp == 0x309C || cp == 0x309A) ? 3 : 2;
      continue;
    }
    if ((cp >= 0x2160 && cp <= 0x216B) || (cp >= 0x2170 && cp <= 0x217B)) {  // Ⅰ...Ⅻ: by value, like digits
      out.push_back({gDigit, cp >= 0x2170 ? cp - 0x216F : cp - 0x215F, 0});
      lastBase = 0;
      continue;
    }
    if (isDigit(cp)) {
      uint64_t v = cp - '0';
      size_t j = i;
      while (j < s.size()) {
        size_t k = j;
        uint32_t d = foldWidth(nextCodePoint(s, k));
        if (!isDigit(d)) break;
        v = std::min<uint64_t>(v * 10 + (d - '0'), 0xFFFFFFFFull);
        j = k;
      }
      i = j;
      out.push_back({gDigit, uint32_t(v), 0});
      lastBase = 0;
      continue;
    }
    if (isAsciiAlpha(cp)) {
      out.push_back({gLatin, uint32_t(cp | 0x20), uint8_t(cp >= 'A' && cp <= 'Z' ? 1 : 0)});
      lastBase = 0;
      continue;
    }
    if (isKana(cp)) {
      bool kata = (cp >= 0x30A1 && cp <= 0x30FE);
      if (cp == 0x30FC || cp == 0xFF70) {  // long vowel mark
        uint32_t v = lastBase ? vowelOf(lastBase) : 0;
        if (v) out.push_back({gKana, v, 4});
        continue;
      }
      if (cp == 0x309D || cp == 0x309E || cp == 0x30FD || cp == 0x30FE) {  // iteration marks
        if (lastBase) out.push_back({gKana, lastBase, uint8_t(cp == 0x309E || cp == 0x30FE ? 2 : 0)});
        continue;
      }
      uint32_t h = katakanaToHiragana(cp);
      if (cp >= 0x30F7 && cp <= 0x30FA) {  // va vi ve vo -> wa wi we wo + dakuten
        static const uint32_t kW[] = {0x308F, 0x3090, 0x3091, 0x3092};
        out.push_back({gKana, kW[cp - 0x30F7], 2});
        lastBase = kW[cp - 0x30F7];
        continue;
      }
      Base b = baseOf(h);
      out.push_back({gKana, b.cp, uint8_t(b.mark + (kata ? 8 : 0))});
      lastBase = b.cp;
      continue;
    }
    if (isIgnorable(cp)) continue;
    // Other letters (Latin with accents, kanji ...): by code point.
    out.push_back({gOther, cp, 0});
    lastBase = 0;
  }
  return out;
}

int compareElements(const std::vector<CollationElement>& a, const std::vector<CollationElement>& b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    if (a[i].group != b[i].group) return a[i].group < b[i].group ? -1 : 1;
    if (a[i].primary != b[i].primary) return a[i].primary < b[i].primary ? -1 : 1;
  }
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  // Secondary: plain < small < voiced < semi-voiced < long vowel (marks), then hiragana < katakana.
  for (size_t i = 0; i < n; ++i) {
    int ma = a[i].secondary & 7, mb = b[i].secondary & 7;
    if (ma != mb) return ma < mb ? -1 : 1;
  }
  for (size_t i = 0; i < n; ++i) {
    int ka = a[i].secondary & 8, kb = b[i].secondary & 8;
    if (ka != kb) return ka < kb ? -1 : 1;
  }
  return 0;
}

int collate(const std::string& a, const std::string& b, bool japanese) {
  int c = compareElements(collationElements(a, japanese), collationElements(b, japanese));
  if (c) return c;
  int r = a.compare(b);
  return r < 0 ? -1 : r > 0 ? 1 : 0;
}

std::string kanaSortKey(const std::string& text) {
  std::string out;
  for (const CollationElement& e : collationElements(text, true)) {
    if (e.group == 0) appendUtf8(out, e.primary);
    else if (e.group == 1) out += std::to_string(e.primary);
    else if (e.group == 2) out += char(e.primary);
    else appendUtf8(out, e.primary);
  }
  return out;
}

std::string toKatakana(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) appendUtf8(out, hiraganaToKatakana(nextCodePoint(s, i)));
  return out;
}

bool isAllKana(const std::string& s) {
  size_t i = 0;
  bool any = false;
  while (i < s.size()) {
    uint32_t cp = foldWidth(nextCodePoint(s, i));
    if (isKana(cp)) any = true;
    else if (!(isDigit(cp) || isIgnorable(cp))) return false;
  }
  return any;
}

}  // namespace rnf

extern "C" {

int rnf_collate(const char* a, const char* b, const char* lang) {
  bool ja = lang && (lang[0] == 'j' || lang[0] == 'J') && (lang[1] == 'a' || lang[1] == 'A');
  try {
    return rnf::collate(a ? a : "", b ? b : "", ja);
  } catch (...) {
    return 0;
  }
}

char* rnf_kana_sort_key(const char* text) {
  try {
    return rnf::dup(rnf::kanaSortKey(text ? text : ""));
  } catch (...) {
    return nullptr;
  }
}

}  // extern "C"
