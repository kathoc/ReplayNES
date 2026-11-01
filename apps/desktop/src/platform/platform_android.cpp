// Android (docs/design/ANDROID.md): the library (ROM/, Projects/) in the app's external files folder
// (Android/data/<app id>/files: a file manager or USB reaches it, no storage permission needed), the
// session and the settings in its internal files; the Trash a folder next to the library.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "replaynes/product.h"
#include <SDL3/SDL.h>

#include <ctime>
#include <string>

#include "paths.h"
#include "platform/platform.h"
#include "platform/trash_xdg.h"

namespace rnl {
namespace {
std::string externalFiles() {
  const char* p = SDL_GetAndroidExternalStoragePath();
  return p ? p : std::string();
}
std::string internalFiles() {
  const char* p = SDL_GetAndroidInternalStoragePath();
  return p ? p : std::string();
}
}  // namespace

Paths Paths::standard() {
  Paths p;
  std::string ext = externalFiles();
  const std::string internal = internalFiles();
  if (ext.empty()) ext = internal;  // no shared storage: the library stays private
  p.setLibraryRoot(ext + "/" RN_PRODUCT_DATA_DIR);
  p.sessionRoot = internal + "/Session";
  p.configDir = internal + "/config";
  return p;
}

std::string Paths::display(const std::string& path) {
  // "Android/data/<app id>/files/..." as a file manager shows it.
  const std::string ext = externalFiles();
  const size_t at = ext.find("/Android/data/");
  if (!ext.empty() && at != std::string::npos && path.compare(0, ext.size(), ext) == 0)
    return path.substr(at + 1);
  return path;
}

bool trashItem(const std::string& path, std::string* error) {
  std::string ext = externalFiles();
  if (ext.empty()) ext = internalFiles();
  return moveToTrash(path, ext + "/Trash", double(std::time(nullptr)), nullptr, error);
}

bool gamingMode() { return false; }

}  // namespace rnl
