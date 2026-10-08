// Title collation for the library (rnf_collate): Japanese gojūon order of kana readings and an
// English order that ignores case and a leading "The". Self-contained (no ICU, identical on every
// platform).
//
// Japanese (after JIS X 4061, simplified): a title is read as a sequence of elements.
//   - Kana (hiragana, katakana, half-width katakana) -> the plain hiragana letter: katakana folded to
//     hiragana, small kana to their full size (ぁ -> あ, っ -> つ), dakuten / handakuten removed
//     (が -> か, ぱ -> は, ヴ -> う). The removed distinctions only break ties (secondary level).
//   - The long vowel mark ー is read as the vowel of the kana before it (スーパー -> すうはあ); the
//     iteration marks ゝ / ゞ repeat it.
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
const char16_t kHalfKana[] = u"ヲァィゥェォャュョッーアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワン";

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
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:  // ぁぃぅぇぉ
      return {h + 1, 1};
    case 0x3063: return {0x3064, 1};                                 // っ
    case 0x3083: case 0x3085: case 0x3087: return {h + 1, 1};       // ゃゅょ
    case 0x308E: return {0x308F, 1};                                 // ゎ
    case 0x3095: return {0x304B, 1};                                 // ゕ
    case 0x3096: return {0x3051, 1};                                 // ゖ
    case 0x3094: return {0x3046, 2};                                 // ゔ
    default: break;
  }
  // か (304B) ... ぢ (3062): plain at odd offsets from か, voiced right after.
  if (h >= 0x304B && h <= 0x3062) return (h - 0x304B) % 2 == 0 ? Base{h, 0} : Base{h - 1, 2};
  // つ (3064) づ, て で, と ど.
  if (h >= 0x3064 && h <= 0x3069) return (h - 0x3064) % 2 == 0 ? Base{h, 0} : Base{h - 1, 2};
  // は (306F) ば ぱ ... ほ ぼ ぽ (307D).
  if (h >= 0x306F && h <= 0x307D) {
    int k = int(h - 0x306F) % 3;
    return Base{h - uint32_t(k), k == 0 ? 0 : k == 1 ? 2 : 3};
  }
  return {h, 0};
}

// The vowel (あいうえお as hiragana) of a plain hiragana letter, 0 for ん / others.
uint32_t vowelOf(uint32_t b) {
  static const char16_t kRows[] = u"あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもらりるれろ";
  static const char16_t kVowels[] = u"あいうえお";
  for (size_t i = 0; kRows[i]; ++i)
    if (kRows[i] == b) return kVowels[i % 5];
  switch (b) {
    case 0x3084: return 0x3042;  // や
    case 0x3086: return 0x3046;  // ゆ
    case 0x3088: return 0x304A;  // よ
    case 0x308F: return 0x3042;  // わ
    case 0x3090: return 0x3044;  // ゐ
    case 0x3091: return 0x3048;  // ゑ
    case 0x3092: return 0x304A;  // を
    default: return 0;
  }
}

bool isAsciiAlpha(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(uint32_t c) { return c >= '0' && c <= '9'; }

bool isIgnorable(uint32_t c) {
  if (c < 0x80) return !isAsciiAlpha(c) && !isDigit(c);
  if (c >= 0x2000 && c <= 0x2BFF) return true;   // general punctuation, arrows, symbols, stars ...
  if (c >= 0x3000 && c <= 0x303F) return true;   // CJK symbols and punctuation (、。「」〜 ...)
  if (c == 0x30FB || c == 0xFF65) return true;    // ・
  if (c == 0x00B7 || c == 0x00D7 || c == 0x00A0) return true;
  if (c >= 0x00A1 && c <= 0x00BF) return true;
  if (c >= 0xFE30 && c <= 0xFE4F) return true;
  return false;
}

}  // namespace

std::vector<CollationElement> collationElements(const std::string& text, bool japanese) {
  std::vector<CollationElement> out;
  const int gKana = japanese ? 0 : 2, gDigit = japanese ? 1 : 0, gLatin = japanese ? 2 : 1, gOther = 3;
  uint32_t lastBase = 0;  // the previous kana letter (for ー / ゝ)
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
      if (cp == 0x30FC || cp == 0xFF70) {  // ー
        uint32_t v = lastBase ? vowelOf(lastBase) : 0;
        if (v) out.push_back({gKana, v, 4});
        continue;
      }
      if (cp == 0x309D || cp == 0x309E || cp == 0x30FD || cp == 0x30FE) {  // ゝゞヽヾ
        if (lastBase) out.push_back({gKana, lastBase, uint8_t(cp == 0x309E || cp == 0x30FE ? 2 : 0)});
        continue;
      }
      uint32_t h = katakanaToHiragana(cp);
      if (cp >= 0x30F7 && cp <= 0x30FA) {  // ヷヸヹヺ -> わゐゑを + dakuten
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
