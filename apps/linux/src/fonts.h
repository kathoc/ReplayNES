// UI fonts: a Latin UI font and a Japanese (CJK) font merged into one ImGui font (ImGui 1.92
// loads glyphs on demand, so the whole CJK font is usable without glyph ranges).
// The freedesktop 25.08 runtime ships no CJK font, so the CJK font comes from the host through
// fontconfig (Flatpak exposes the host's fonts as /run/host/fonts; SteamOS has Noto Sans CJK);
// a well-known path list is the fallback. Without a CJK font the UI falls back to English.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace rnl {

struct FontFile {
  std::string path;
  int index = 0;  // face index in a collection (.ttc)
  bool found() const { return !path.empty(); }
};

/// Sans-serif UI font for Latin text.
FontFile findLatinFont();
/// A font with Japanese glyphs (prefers Noto Sans CJK JP / Noto Sans JP).
FontFile findJapaneseFont();
/// A font with the controller symbols the Latin font lacks (e.g. "⧉", the Steam Deck View button).
FontFile findSymbolFont();

}  // namespace rnl
