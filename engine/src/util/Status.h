// ReplayNES engine - status/error type shared by all modules.
// Error codes are numerically identical to rn_status in replaynes.h.
#pragma once
#include <string>
#include <utility>

namespace rn {

enum class Err : int {
  Ok = 0,
  InvalidArg = 1,
  Io = 2,
  NotFound = 3,
  Corrupt = 4,
  CoreMismatch = 5,
  RomMismatch = 6,
  RomNotFound = 7,
  UnsupportedFormat = 8,
  DiskFull = 9,
  RomInvalid = 10,
  StateError = 11,
  OutOfRange = 12,
  Cancelled = 13,
  AlreadyExists = 14,
  WrongMode = 15,
  EndOfTake = 16,
  Discontinuity = 17,  // practice: B needs unbroken emulation since that slot's A anchor
  Internal = 99,
};

struct Status {
  Err code = Err::Ok;
  std::string message;

  Status() = default;
  Status(Err c, std::string m) : code(c), message(std::move(m)) {}
  bool ok() const { return code == Err::Ok; }
  explicit operator bool() const { return ok(); }
  static Status Ok() { return Status(); }
};

inline Status Error(Err c, std::string m) { return Status(c, std::move(m)); }

const char* errName(Err e);

#define RN_TRY(expr)                 \
  do {                               \
    ::rn::Status _rn_st = (expr);    \
    if (!_rn_st.ok()) return _rn_st; \
  } while (0)

}  // namespace rn
