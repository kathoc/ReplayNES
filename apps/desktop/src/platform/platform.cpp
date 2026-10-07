// Portable parts of the platform services (platform.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/platform.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace rnl {

bool copyTree(const std::string& from, const std::string& to, std::string* error) {
  std::error_code ec;
  if (fs::exists(to, ec)) {
    if (error) *error = to + ": already exists";
    return false;
  }
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
  if (ec) {
    std::error_code ec2;
    fs::remove_all(to, ec2);
    if (error) *error = from + " -> " + to + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace rnl
