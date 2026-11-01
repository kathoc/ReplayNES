// Android's own fonts (/system/fonts): Roboto, Noto Sans CJK (a collection: JP first) and the
// symbols font for the controller glyphs. SPDX-License-Identifier: GPL-2.0-or-later
#include <sys/stat.h>

#include <initializer_list>

#include "fonts.h"

namespace rnl {
namespace {
FontFile first(std::initializer_list<const char*> paths) {
  struct stat st;
  for (const char* p : paths)
    if (stat(p, &st) == 0) return FontFile{p, 0};
  return {};
}
}  // namespace

FontFile findLatinFont() { return first({"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoFlex-Regular.ttf"}); }
FontFile findJapaneseFont() {
  return first({"/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoSansJP-Regular.otf", "/system/fonts/DroidSansJapanese.ttf"});
}
FontFile findSymbolFont() {
  return first({"/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf", "/system/fonts/NotoSansSymbols-Regular-Subsetted2.ttf"});
}

}  // namespace rnl
