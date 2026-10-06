#include "util/Status.h"

namespace rn {
const char* errName(Err e) {
  switch (e) {
    case Err::Ok: return "ok";
    case Err::InvalidArg: return "invalid_argument";
    case Err::Io: return "io_error";
    case Err::NotFound: return "not_found";
    case Err::Corrupt: return "corrupt";
    case Err::CoreMismatch: return "core_mismatch";
    case Err::RomMismatch: return "rom_mismatch";
    case Err::RomNotFound: return "rom_not_found";
    case Err::UnsupportedFormat: return "unsupported_format";
    case Err::DiskFull: return "disk_full";
    case Err::RomInvalid: return "rom_invalid";
    case Err::StateError: return "state_error";
    case Err::OutOfRange: return "out_of_range";
    case Err::Cancelled: return "cancelled";
    case Err::AlreadyExists: return "already_exists";
    case Err::WrongMode: return "wrong_mode";
    case Err::EndOfTake: return "end_of_take";
    case Err::Discontinuity: return "discontinuity";
    case Err::Internal: return "internal";
  }
  return "unknown";
}
}  // namespace rn
