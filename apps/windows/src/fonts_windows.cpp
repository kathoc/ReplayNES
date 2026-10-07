// UI fonts on Windows (fonts.h): the system fonts in %WINDIR%\Fonts - Segoe UI for Latin text,
// Yu Gothic / Meiryo / MS Gothic for Japanese, Segoe UI Symbol / Cambria Math for the controller
// symbols. A candidate is used only when it has the glyph asked for (checked with stb_truetype,
// which ImGui also uses to read the fonts).
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>
#include <vector>

#include "fonts.h"

// A private copy of ImGui's stb_truetype (static functions; only the glyph lookup is used).
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace rnl {

namespace {

std::string fontsDir() {
  PWSTR p = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &p))) {
    int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      out.resize(size_t(n - 1));
      WideCharToMultiByte(CP_UTF8, 0, p, -1, out.data(), n, nullptr, nullptr);
    }
  }
  CoTaskMemFree(p);
  return out.empty() ? std::string("C:\\Windows\\Fonts") : out;
}

std::vector<unsigned char> readAll(const std::string& path) {
  std::vector<unsigned char> data;
  int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring w(size_t(n > 0 ? n : 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
  FILE* f = _wfopen(w.c_str(), L"rb");
  if (!f) return data;
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    data.resize(size_t(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
  }
  std::fclose(f);
  return data;
}

/// The face `index` of `file` (in the fonts folder) if it has `codepoint`.
FontFile withGlyph(const char* file, int index, int codepoint) {
  std::string path = fontsDir() + "\\" + file;
  std::vector<unsigned char> data = readAll(path);
  if (data.empty()) return {};
  int offset = stbtt_GetFontOffsetForIndex(data.data(), index);
  stbtt_fontinfo info;
  if (offset < 0 || !stbtt_InitFont(&info, data.data(), offset)) return {};
  if (codepoint && stbtt_FindGlyphIndex(&info, codepoint) == 0) return {};
  return FontFile{path, index};
}

FontFile first(std::initializer_list<std::pair<const char*, int>> candidates, int codepoint) {
  for (const auto& [file, index] : candidates) {
    FontFile f = withGlyph(file, index, codepoint);
    if (f.found()) return f;
  }
  return {};
}

}  // namespace

FontFile findLatinFont() { return first({{"segoeui.ttf", 0}, {"arial.ttf", 0}, {"tahoma.ttf", 0}}, 'A'); }

// 0x3042: hiragana letter a. Yu Gothic (Windows 10+, also on non-Japanese installs), then the
// Japanese supplemental fonts.
FontFile findJapaneseFont() {
  return first({{"YuGothM.ttc", 0}, {"YuGothR.ttc", 0}, {"meiryo.ttc", 0}, {"BIZ-UDGothicR.ttc", 0}, {"msgothic.ttc", 0}},
               0x3042);
}

// "⧉" (U+29C9, the View button) and the PlayStation shapes.
FontFile findSymbolFont() {
  return first({{"seguisym.ttf", 0}, {"cambria.ttc", 1}, {"cambria.ttc", 0}, {"seguiemj.ttf", 0}}, 0x29C9);
}

}  // namespace rnl
