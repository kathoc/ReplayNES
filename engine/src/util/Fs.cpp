#include "util/Fs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#define RN_FSYNC(fd) _commit(fd)
#define RN_FILENO(f) _fileno(f)
#else
#include <fcntl.h>
#include <unistd.h>
#define RN_FSYNC(fd) ::fsync(fd)
#define RN_FILENO(f) ::fileno(f)
#endif

namespace stdfs = std::filesystem;

namespace rn {
namespace fs {

namespace {
int64_t gDiskFullAfter = -1;
std::atomic<unsigned> gTempCounter{0};

Status ioErr(const std::string& what, const std::string& path, int err) {
  if (err == ENOSPC
#ifdef EDQUOT
      || err == EDQUOT
#endif
  )
    return Error(Err::DiskFull, what + " failed (disk full): " + path);
  return Error(Err::Io, what + " failed: " + path + ": " + std::strerror(err));
}

// Writes with fault injection; returns errno-like code (0 = ok).
int checkedWrite(std::FILE* f, const void* data, size_t n) {
  if (gDiskFullAfter >= 0) {
    if (int64_t(n) > gDiskFullAfter) {
      size_t part = size_t(gDiskFullAfter);
      if (part) std::fwrite(data, 1, part, f);
      gDiskFullAfter = 0;
      return ENOSPC;
    }
    gDiskFullAfter -= int64_t(n);
  }
  if (n && std::fwrite(data, 1, n, f) != n) return errno ? errno : EIO;
  return 0;
}

int flushAndSync(std::FILE* f) {
  if (std::fflush(f) != 0) return errno ? errno : EIO;
  if (RN_FSYNC(RN_FILENO(f)) != 0) return errno ? errno : EIO;
  return 0;
}
}  // namespace

void setDiskFullAfterBytes(int64_t bytes) { gDiskFullAfter = bytes; }

std::string join(const std::string& a, const std::string& b) {
  return (stdfs::u8path(a) / stdfs::u8path(b)).u8string();
}

bool exists(const std::string& path) {
  std::error_code ec;
  return stdfs::exists(stdfs::u8path(path), ec);
}

bool isDir(const std::string& path) {
  std::error_code ec;
  return stdfs::is_directory(stdfs::u8path(path), ec);
}

int64_t fileSize(const std::string& path) {
  std::error_code ec;
  auto s = stdfs::file_size(stdfs::u8path(path), ec);
  return ec ? -1 : int64_t(s);
}

Status createDirs(const std::string& path) {
  std::error_code ec;
  stdfs::create_directories(stdfs::u8path(path), ec);
  if (ec) return Error(Err::Io, "cannot create directory " + path + ": " + ec.message());
  return Status::Ok();
}

static std::FILE* openFile(const std::string& path, const char* mode) {
#if defined(_WIN32)
  std::wstring wmode(mode, mode + std::strlen(mode));
  return _wfopen(stdfs::u8path(path).c_str(), wmode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

Status readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::FILE* f = openFile(path, "rb");
  if (!f) {
    if (errno == ENOENT) return Error(Err::NotFound, "file not found: " + path);
    return ioErr("open", path, errno);
  }
  out.clear();
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  bool err = std::ferror(f) != 0;
  std::fclose(f);
  if (err) return Error(Err::Io, "read failed: " + path);
  return Status::Ok();
}

Status readText(const std::string& path, std::string& out) {
  std::vector<uint8_t> b;
  RN_TRY(readFile(path, b));
  out.assign(b.begin(), b.end());
  return Status::Ok();
}

Status syncDir(const std::string& path) {
#if defined(_WIN32)
  (void)path;  // NTFS metadata journaling; no directory handle fsync available via CRT.
  return Status::Ok();
#else
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return ioErr("open dir", path, errno);
  int r = ::fsync(fd);
  int e = errno;
  ::close(fd);
  // Some filesystems reject fsync on directories (EINVAL); that is not a data-loss condition.
  if (r != 0 && e != EINVAL && e != ENOTSUP) return ioErr("fsync dir", path, e);
  return Status::Ok();
#endif
}

Status writeFileAtomic(const std::string& path, const void* data, size_t n) {
  stdfs::path target = stdfs::u8path(path);
  stdfs::path dir = target.parent_path();
  std::string tmp = (dir / stdfs::u8path("." + target.filename().u8string() + ".tmp" +
                                         std::to_string(++gTempCounter)))
                        .u8string();
  std::FILE* f = openFile(tmp, "wb");
  if (!f) return ioErr("create", tmp, errno);
  int e = checkedWrite(f, data, n);
  if (!e) e = flushAndSync(f);
  std::fclose(f);
  if (e) {
    std::error_code ec;
    stdfs::remove(stdfs::u8path(tmp), ec);
    return ioErr("write", path, e);
  }
  std::error_code ec;
  stdfs::rename(stdfs::u8path(tmp), target, ec);  // atomic replace (POSIX rename / MoveFileEx)
  if (ec) {
    stdfs::remove(stdfs::u8path(tmp), ec);
    return Error(Err::Io, "rename failed: " + path);
  }
  return syncDir(dir.empty() ? "." : dir.u8string());
}

Status appendFileSync(const std::string& path, const void* data, size_t n) {
  std::FILE* f = openFile(path, "ab");
  if (!f) return ioErr("open", path, errno);
  int e = checkedWrite(f, data, n);
  if (!e) e = flushAndSync(f);
  std::fclose(f);
  if (e) return ioErr("append", path, e);
  return Status::Ok();
}

Status truncateFile(const std::string& path, uint64_t size) {
  std::error_code ec;
  stdfs::resize_file(stdfs::u8path(path), size, ec);
  if (ec) return Error(Err::Io, "truncate failed: " + path + ": " + ec.message());
  return Status::Ok();
}

Status removeFile(const std::string& path) {
  std::error_code ec;
  stdfs::remove(stdfs::u8path(path), ec);
  if (ec) return Error(Err::Io, "remove failed: " + path + ": " + ec.message());
  return Status::Ok();
}

Status removeAll(const std::string& path) {
  std::error_code ec;
  stdfs::remove_all(stdfs::u8path(path), ec);
  if (ec) return Error(Err::Io, "remove failed: " + path + ": " + ec.message());
  return Status::Ok();
}

Status listDir(const std::string& path, std::vector<std::string>& names) {
  names.clear();
  std::error_code ec;
  for (auto it = stdfs::directory_iterator(stdfs::u8path(path), ec); !ec && it != stdfs::directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) names.push_back(it->path().filename().u8string());
  }
  if (ec) return Error(Err::Io, "list failed: " + path + ": " + ec.message());
  std::sort(names.begin(), names.end());
  return Status::Ok();
}

}  // namespace fs
}  // namespace rn
