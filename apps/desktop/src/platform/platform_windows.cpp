// Windows: Known Folders, the Recycle Bin (IFileOperation), no gamescope.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "paths.h"
#include "platform/platform.h"

namespace fs = std::filesystem;

namespace rnl {

namespace {

std::string toUtf8(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(std::max(0, n - 1)), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = toUtf8(p);
  CoTaskMemFree(p);
  return out;
}

std::string envUtf8(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(name, buf, DWORD(sizeof buf / sizeof buf[0]));
  return n > 0 && n < sizeof buf / sizeof buf[0] ? toUtf8(buf) : std::string();
}

std::string hresultText(HRESULT hr) {
  char b[64];
  std::snprintf(b, sizeof b, "HRESULT 0x%08lx", (unsigned long)hr);
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 DWORD(hr), 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
  std::string text = toUtf8(msg);
  LocalFree(msg);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  return text.empty() ? std::string(b) : text + " (" + b + ")";
}

/// COM for the calling thread for the lifetime of the object (balanced; a thread already in the
/// multithreaded apartment keeps it).
struct ComScope {
  HRESULT hr;
  ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
  ~ComScope() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
};

}  // namespace

Paths Paths::standard() {
  Paths p;
  std::string docs = knownFolder(FOLDERID_Documents);
  if (docs.empty()) docs = envUtf8(L"USERPROFILE") + "\\Documents";
  p.setLibraryRoot(docs + "\\ReplayNES");
  std::string local = knownFolder(FOLDERID_LocalAppData);
  if (local.empty()) local = envUtf8(L"LOCALAPPDATA");
  std::string roaming = knownFolder(FOLDERID_RoamingAppData);
  if (roaming.empty()) roaming = envUtf8(L"APPDATA");
  if (roaming.empty()) roaming = local;
  p.sessionRoot = local + "\\ReplayNES\\Session";
  p.configDir = roaming + "\\ReplayNES";
  return p;
}

std::string Paths::display(const std::string& path) {
  std::string out = path;
  std::replace(out.begin(), out.end(), '/', '\\');
  return out;
}

bool trashItem(const std::string& path, std::string* error) {
  std::error_code ec;
  fs::path abs = fs::absolute(fs::path(path), ec);
  if (ec || !fs::exists(fs::symlink_status(abs, ec))) {
    if (error) *error = path + ": not found";
    return false;
  }
  std::wstring wpath = abs.lexically_normal().make_preferred().wstring();
  ComScope com;
  IFileOperation* op = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
  if (SUCCEEDED(hr)) {
    // Recycle; no confirmation / progress / error dialogs - except the system's warning when an
    // item cannot be recycled and would be deleted permanently (FOF_WANTNUKEWARNING).
    DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING;
#ifdef FOFX_RECYCLEONDELETE
    flags |= FOFX_RECYCLEONDELETE;
#else
    flags |= 0x00080000;  // FOFX_RECYCLEONDELETE (Windows 8+)
#endif
    hr = op->SetOperationFlags(flags);
  }
  IShellItem* item = nullptr;
  if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&item));
  if (SUCCEEDED(hr)) hr = op->DeleteItem(item, nullptr);
  if (SUCCEEDED(hr)) hr = op->PerformOperations();
  BOOL aborted = FALSE;
  if (SUCCEEDED(hr)) op->GetAnyOperationsAborted(&aborted);
  if (item) item->Release();
  if (op) op->Release();
  if (FAILED(hr) || aborted) {
    if (error) *error = Paths::display(path) + ": " + (aborted ? std::string("cancelled") : hresultText(hr));
    return false;
  }
  if (fs::exists(fs::symlink_status(abs, ec))) {
    if (error) *error = Paths::display(path) + ": still there after moving it to the Recycle Bin";
    return false;
  }
  return true;
}

bool gamingMode() { return false; }

}  // namespace rnl
