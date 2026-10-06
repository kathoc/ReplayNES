// Minimal PNG writer (8-bit RGBA, zlib "stored" blocks: no compression library needed). Used for
// screenshots of the presented frame (--script "shot <path>").
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rnl {

/// rgba: width * height * 4 bytes, rows top to bottom.
std::vector<uint8_t> encodePNG(int width, int height, const uint8_t* rgba);
bool writePNG(const std::string& path, int width, int height, const uint8_t* rgba);

}  // namespace rnl
