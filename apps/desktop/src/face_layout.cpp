// SPDX-License-Identifier: GPL-2.0-or-later
#include "face_layout.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace rnl {

// One line per model: <0 | 1> TAB <model> (SDL GUID / name).
void FaceLayouts::load() {
  if (file_.empty()) return;
  std::ifstream in(file_);
  for (std::string line; std::getline(in, line);) {
    if (line.size() < 3 || line[1] != '\t' || (line[0] != '0' && line[0] != '1')) continue;
    layouts_[line.substr(2)] = line[0] == '1';
  }
}

void FaceLayouts::save() const {
  if (file_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(file_).parent_path(), ec);
  std::ofstream out(file_, std::ios::trunc);
  for (const auto& [model, swapped] : layouts_) out << (swapped ? '1' : '0') << '\t' << model << '\n';
}

void FaceLayouts::set(const std::string& model, bool swapped) {
  layouts_[model] = swapped;
  save();
}

void FaceLayouts::forget(const std::string& model) {
  if (layouts_.erase(model)) save();
}

}  // namespace rnl
