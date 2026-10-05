// Platform file layer. The ONLY place in the engine with OS-specific calls
// (fsync / FlushFileBuffers, directory sync). Everything else uses these wrappers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "util/Status.h"

namespace rn {
namespace fs {

std::string join(const std::string& a, const std::string& b);
bool exists(const std::string& path);
bool isDir(const std::string& path);
Status createDirs(const std::string& path);
Status readFile(const std::string& path, std::vector<uint8_t>& out);
Status readText(const std::string& path, std::string& out);
// temp file in same directory -> write -> flush+fsync -> rename over target -> fsync directory.
Status writeFileAtomic(const std::string& path, const void* data, size_t n);
inline Status writeFileAtomic(const std::string& path, const std::string& s) {
  return writeFileAtomic(path, s.data(), s.size());
}
// Append + fsync (journal). Creates the file if missing.
Status appendFileSync(const std::string& path, const void* data, size_t n);
Status truncateFile(const std::string& path, uint64_t size);
Status removeFile(const std::string& path);  // missing file is not an error
Status removeAll(const std::string& path);
Status listDir(const std::string& path, std::vector<std::string>& names);  // file names only, sorted
Status syncDir(const std::string& path);
int64_t fileSize(const std::string& path);  // -1 if missing

// Test hook: after `bytes` more bytes are written through this layer, writes fail with
// DiskFull (simulates ENOSPC). Pass -1 to disable. Not thread-safe; tests only.
void setDiskFullAfterBytes(int64_t bytes);

}  // namespace fs
}  // namespace rn
