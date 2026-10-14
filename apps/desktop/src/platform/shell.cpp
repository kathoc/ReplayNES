// openFolder / openFile (platform.h): Explorer / the shell on Windows, the desktop's file manager or
// default application through SDL elsewhere.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string>

#include "platform/platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <SDL3/SDL.h>
#endif

namespace rnl {

#ifdef _WIN32
namespace {
bool shellExecute(const wchar_t* verb, const std::string& path) {
  int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (n <= 0) return false;
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  HINSTANCE r = ShellExecuteW(nullptr, verb, w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(r) > 32;
}
}  // namespace
#endif

bool openFolder(const std::string& dir) {
#ifdef _WIN32
  return shellExecute(L"explore", dir);
#else
  return SDL_OpenURL(("file://" + dir).c_str());
#endif
}

bool openFile(const std::string& path) {
#ifdef _WIN32
  return shellExecute(L"open", path);
#else
  return SDL_OpenURL(("file://" + path).c_str());
#endif
}

}  // namespace rnl
