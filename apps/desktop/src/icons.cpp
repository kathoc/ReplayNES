// SPDX-License-Identifier: GPL-2.0-or-later
#include "icons.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "imgui.h"

extern const unsigned char rnl_icon_font[];
extern const unsigned int rnl_icon_font_size;
extern const unsigned char rnl_icon_font_filled[];
extern const unsigned int rnl_icon_font_filled_size;

namespace rnl::icons {

namespace {
struct Entry {
  const char* name;
  unsigned code;
};
const Entry kIcons[] = {
#include "../resources/fonts/icons.inc"
};

std::string utf8(unsigned c) {
  std::string s;
  if (c < 0x80) s += char(c);
  else if (c < 0x800) s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
  else s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
  return s;
}
}  // namespace

void addToAtlas(ImFontAtlas* atlas) {
  for (int i = 0; i < 2; ++i) {
    ImFontConfig c;
    c.MergeMode = true;
    c.FontDataOwnedByAtlas = false;  // static data
    c.PixelSnapH = true;
    c.GlyphOffset = ImVec2(0, 0);
    std::snprintf(c.Name, sizeof c.Name, "%s", i == 0 ? "Tabler Icons" : "Tabler Icons Filled");
    const unsigned char* data = i == 0 ? rnl_icon_font : rnl_icon_font_filled;
    int size = int(i == 0 ? rnl_icon_font_size : rnl_icon_font_filled_size);
    atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), size, 0.0f, &c);
  }
}

const char* glyph(const char* name) {
  static const std::map<std::string, std::string> table = [] {
    std::map<std::string, std::string> m;
    for (const Entry& e : kIcons) m[e.name] = utf8(e.code);
    return m;
  }();
  if (!name || !*name) return "";
  auto it = table.find(name);
  return it == table.end() ? "" : it->second.c_str();
}

}  // namespace rnl::icons
