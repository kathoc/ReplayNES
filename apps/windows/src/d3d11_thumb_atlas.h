// CPU allocation/upload data for the D3D11 thumbnail atlas (no GPU required).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rnl {
class D3D11ThumbAtlas {
 public:
  static constexpr int cellSize = 128, size = 1024, perRow = size / cellSize;
  static constexpr int cells = perRow * perRow, maxUploads = 12;
  struct Upload { int cell; uint32_t width, height; std::vector<uint32_t> pixels; };
  std::vector<Upload> pending;

  void beginFrame() { ++frame_; }
  void clear() {
    keys_.fill(0); used_.fill(0); entries_.clear(); pending.clear();
  }
  bool get(uint64_t key, const uint32_t* pixels, uint32_t width, uint32_t height, float uv[4]) {
    if (!key || !pixels || !width || width > cellSize || !height || !uv) return false;
    const uint32_t uploadHeight = std::min(height, uint32_t(size));
    const int rows = int((uploadHeight + cellSize - 1) / cellSize);
    int cell = -1;
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      // The key identifies immutable picture content, including its dimensions.
      if (it->second.width != width || it->second.height != height) return false;
      cell = it->second.cell;
    } else {
      if (pending.size() >= maxUploads) return false;
      uint64_t best = ~uint64_t(0);
      for (int i = 0; i < cells; ++i) {
        if (i / perRow + rows > perRow) continue;
        uint64_t age = 0;
        bool available = true;
        for (int row = 0; row < rows; ++row) {
          int c = i + row * perRow;
          if (keys_[c] && used_[c] >= frame_) { available = false; break; }
          age = std::max(age, keys_[c] ? used_[c] : 0);
        }
        if (available && age < best) { cell = i; best = age; if (!age) break; }
      }
      if (cell < 0) return false;
      for (int row = 0; row < rows; ++row) {
        const uint64_t old = keys_[cell + row * perRow];
        if (!old) continue;
        const int oldCell = entries_.at(old).cell;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                      [&](const Upload& p) { return p.cell == oldCell; }), pending.end());
        entries_.erase(old);
        for (auto& k : keys_) if (k == old) k = 0;
      }
      for (int row = 0; row < rows; ++row) keys_[cell + row * perRow] = key;
      entries_[key] = Entry{cell, width, height};
      std::vector<uint32_t> copy(size_t(width) * uploadHeight);
      for (uint32_t y = 0; y < uploadHeight; ++y)
        std::copy_n(pixels + size_t(uint64_t(y) * height / uploadHeight) * width, width,
                    copy.data() + size_t(y) * width);
      pending.push_back(Upload{cell, width, uploadHeight, std::move(copy)});
    }
    for (int row = 0; row < rows; ++row) used_[cell + row * perRow] = frame_;
    const float x = float((cell % perRow) * cellSize), y = float((cell / perRow) * cellSize);
    uv[0] = x / size; uv[1] = y / size;
    uv[2] = (x + width) / size; uv[3] = (y + uploadHeight) / size;
    return true;
  }
 private:
  struct Entry { int cell; uint32_t width, height; };
  uint64_t frame_ = 0;
  std::array<uint64_t, cells> keys_{}, used_{};
  std::unordered_map<uint64_t, Entry> entries_;
};
}  // namespace rnl
