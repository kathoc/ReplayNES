// Android's own fonts (/system/fonts): Roboto, Noto Sans CJK (a collection: JP first) and the
// symbols font for the controller glyphs. SPDX-License-Identifier: GPL-2.0-or-later
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include <initializer_list>

#include "fonts.h"

namespace rnl {
namespace {
constexpr const char* kBundledJapanese = "fonts/NotoSansJP-Regular.otf";
FontFile first(std::initializer_list<const char*> paths) {
  struct stat st;
  for (const char* p : paths)
    if (stat(p, &st) == 0) return FontFile{p, 0};
  return {};
}
}  // namespace

FontFile findLatinFont() { return first({"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoFlex-Regular.ttf"}); }
// The bundled Noto Sans JP (assets/fonts, third_party/fonts/noto-sans-jp, SIL OFL 1.1) first: recent Android
// versions carry Noto Sans CJK as a variable font (CFF2, seen on Android 16), which stb_truetype cannot read,
// so the UI showed "?" for Japanese; the bundled static one looks the same on every version.
FontFile findJapaneseFont() {
  if (SDL_IOStream* io = SDL_IOFromFile(kBundledJapanese, "rb")) {
    SDL_CloseIO(io);
    return FontFile{kBundledJapanese, 0, true};
  }
  return first({"/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoSansJP-Regular.otf", "/system/fonts/DroidSansJapanese.ttf"});
}
FontFile findSymbolFont() {
  return first({"/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf", "/system/fonts/NotoSansSymbols-Regular-Subsetted2.ttf"});
}

}  // namespace rnl
