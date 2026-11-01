// QOI ("Quite OK Image", qoiformat.org, specification 1.0) to RGBA: the bezel's image format
// (docs/design/BEZEL.md). Self-contained: no SDL_image / SDL 3.4 PNG loader, which the Flatpak
// runtime's SDL does not have.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rnl {

/// RGBA bytes (row by row, 4 per pixel) of a QOI image; false when it is not one or is cut short.
bool decodeQoi(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height, std::vector<uint8_t>* rgba);

}  // namespace rnl
