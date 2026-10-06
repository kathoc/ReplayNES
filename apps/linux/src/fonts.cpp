// SPDX-License-Identifier: GPL-2.0-or-later
#include "fonts.h"

#include <fontconfig/fontconfig.h>

#include <filesystem>
#include <system_error>

namespace rnl {

namespace {
bool fileExists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec);
}

/// fontconfig match for a pattern; requireChar: the font must have this code point.
FontFile match(const char* pattern, FcChar32 requireChar) {
  FontFile out;
  if (!FcInit()) return out;
  FcPattern* p = FcNameParse(reinterpret_cast<const FcChar8*>(pattern));
  if (!p) return out;
  FcConfigSubstitute(nullptr, p, FcMatchPattern);
  FcDefaultSubstitute(p);
  FcResult r = FcResultNoMatch;
  FcFontSet* set = FcFontSort(nullptr, p, FcTrue, nullptr, &r);
  if (set) {
    for (int i = 0; i < set->nfont && !out.found(); ++i) {
      FcPattern* f = set->fonts[i];
      FcChar8* file = nullptr;
      int index = 0;
      FcCharSet* cs = nullptr;
      if (FcPatternGetString(f, FC_FILE, 0, &file) != FcResultMatch || !file) continue;
      FcPatternGetInteger(f, FC_INDEX, 0, &index);
      if (requireChar && (FcPatternGetCharSet(f, FC_CHARSET, 0, &cs) != FcResultMatch || !FcCharSetHasChar(cs, requireChar)))
        continue;
      std::string path = reinterpret_cast<const char*>(file);
      // stb_truetype reads TrueType and CFF OpenType (.ttf / .otf / .ttc), not bitmap fonts.
      std::string ext = path.size() > 4 ? path.substr(path.size() - 4) : "";
      if (ext != ".ttf" && ext != ".otf" && ext != ".ttc" && ext != ".TTF" && ext != ".OTF" && ext != ".TTC") continue;
      out.path = path;
      out.index = index;
    }
    FcFontSetDestroy(set);
  }
  FcPatternDestroy(p);
  return out;
}
}  // namespace

FontFile findLatinFont() {
  for (const char* f : {"/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"})
    if (fileExists(f)) return FontFile{f, 0};
  return match("sans-serif:lang=en", 'A');
}

FontFile findJapaneseFont() {
  // 0x3042: hiragana letter a.
  for (const char* pat : {"Noto Sans CJK JP:lang=ja", "Noto Sans JP:lang=ja", "sans-serif:lang=ja"}) {
    FontFile f = match(pat, 0x3042);
    if (f.found()) return f;
  }
  // Without fontconfig's view of the host fonts: known locations (SteamOS host, distributions).
  const std::pair<const char*, int> known[] = {
      {"/run/host/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 0},
      {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 0},
      {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0},
      {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", 0},
  };
  for (const auto& [p, i] : known)
    if (fileExists(p)) return FontFile{p, i};
  return FontFile{};
}

FontFile findSymbolFont() { return match("sans-serif", 0x29C9); }

}  // namespace rnl
