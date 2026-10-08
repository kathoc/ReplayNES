// Menu icons: a Tabler Icons subset (MIT; apps/desktop/resources/fonts, THIRD_PARTY_NOTICES.md)
// compiled in and merged into the UI font, so an icon is just text ("\uXXXX" in the private use
// area) drawn with the same font at any size. Names are Tabler's ("player-play", "settings"),
// the same names the shared menu model (rnf_menu) uses.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

struct ImFontAtlas;

namespace rnl::icons {

/// Merges the icon fonts into the font being built (call right after its first source).
void addToAtlas(ImFontAtlas* atlas);
/// UTF-8 of the icon `name` ("" when the subset does not have it).
const char* glyph(const char* name);

}  // namespace rnl::icons
