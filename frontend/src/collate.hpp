// Text helpers of the game database / library catalog (collate.cpp). Internal.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rnf {

// UTF-8 decoding / encoding (invalid bytes decode as U+FFFD).
uint32_t nextCodePoint(const std::string& s, size_t& i);
void appendUtf8(std::string& out, uint32_t cp);
// Full-width ASCII -> ASCII, ideographic space -> space, half-width katakana -> katakana.
uint32_t foldWidth(uint32_t cp);
bool isKana(uint32_t cp);
uint32_t katakanaToHiragana(uint32_t cp);
uint32_t hiraganaToKatakana(uint32_t cp);

struct CollationElement {
  int group;          // ordering class (see collate.cpp)
  uint32_t primary;   // plain hiragana letter / digit run value / lower-case letter / code point
  uint8_t secondary;  // marks: 0 plain, 1 small, 2 dakuten, 3 handakuten, 4 long vowel; +8 katakana
};
std::vector<CollationElement> collationElements(const std::string& text, bool japanese);
int compareElements(const std::vector<CollationElement>& a, const std::vector<CollationElement>& b);
int collate(const std::string& a, const std::string& b, bool japanese);
std::string kanaSortKey(const std::string& text);
std::string toKatakana(const std::string& s);
// Kana with digits / punctuation only (at least one kana).
bool isAllKana(const std::string& s);

}  // namespace rnf
