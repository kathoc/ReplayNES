// Test helper: an empty file that the file system stores sparse (holes take no disk space), so the
// MP4 tests can lay out boxes past 4 GiB cheaply. APFS / ext4 / tmpfs make holes on their own;
// NTFS needs the sparse attribute first.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "sparse_file.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

#include <filesystem>
#include <fstream>

bool createSparseFile(const std::string& path) {
#ifdef _WIN32
  const std::wstring w = std::filesystem::u8path(path).wstring();
  HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD ret = 0;
  const bool ok = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr) != 0;
  CloseHandle(h);
  return ok;
#else
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
  return bool(f);
#endif
}
