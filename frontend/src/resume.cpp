// Always-on session persistence: the resume record (resume.json, the format the macOS app has
// written since 0.2: JSON object with ISO 8601 "updated"), the launch-time resume decision, the
// single-instance lock and resume helpers on an engine session.
//
//   <session root>/
//     current.nesrec   temporary project of a session that has no project yet (a normal .nesrec:
//                      full save + autosave journal, so a crash is recovered like any project)
//     resume.json      what to reopen at the next launch (atomic write)
//     .lock            locked by the instance that owns this folder
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include <sys/stat.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "common.hpp"
#include "util/Fs.h"
#include "util/Json.h"

using namespace rnf;
namespace stdfs = std::filesystem;

namespace {

bool pathExists(const std::string& p) {
  if (p.empty()) return false;
#ifdef _WIN32
  std::error_code ec;
  return stdfs::exists(stdfs::u8path(p), ec);
#else
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
#endif
}

std::string joinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  if (dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

std::string stripTrailing(std::string s) {
  while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  return s;
}

std::string normalized(const std::string& p) {
  stdfs::path path = stdfs::u8path(stripTrailing(p));
  std::error_code ec;
  stdfs::path c = stdfs::weakly_canonical(path, ec);
  if (ec) c = stdfs::absolute(path, ec).lexically_normal();
  return stripTrailing(c.generic_u8string());
}

// ------------------------------------------------------------------ ISO 8601 (whole seconds, UTC)
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}

void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = int64_t(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

std::string iso8601(double t) {
  int64_t s = int64_t(std::floor(t));
  int64_t days = s >= 0 ? s / 86400 : (s - 86399) / 86400;
  int64_t rem = s - days * 86400;
  int64_t y;
  unsigned m, d;
  civilFromDays(days, y, m, d);
  char buf[96];  // room for any int64 (GCC -Wformat-truncation)
  std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", (long long)y, m, d, (long long)(rem / 3600),
                (long long)(rem / 60 % 60), (long long)(rem % 60));
  return buf;
}

bool digits(const std::string& s, size_t pos, size_t n, int& out) {
  if (pos + n > s.size()) return false;
  int v = 0;
  for (size_t i = pos; i < pos + n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  out = v;
  return true;
}

// "yyyy-MM-ddTHH:mm:ssZ" or with a "+HH:MM" / "-HH:MM" offset (internet date-time, no fraction).
bool parseISO8601(const std::string& s, double& out) {
  int y, mo, d, h, mi, se;
  if (!digits(s, 0, 4, y) || s.size() < 20 || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' ||
      !digits(s, 8, 2, d) || (s[10] != 'T' && s[10] != 't') || !digits(s, 11, 2, h) || s[13] != ':' ||
      !digits(s, 14, 2, mi) || s[16] != ':' || !digits(s, 17, 2, se))
    return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
  int offset = 0;
  std::string tz = s.substr(19);
  if (tz == "Z" || tz == "z") {
    offset = 0;
  } else if (tz.size() == 6 && (tz[0] == '+' || tz[0] == '-') && tz[3] == ':') {
    int oh, om;
    if (!digits(tz, 1, 2, oh) || !digits(tz, 4, 2, om)) return false;
    offset = (oh * 60 + om) * 60 * (tz[0] == '-' ? -1 : 1);
  } else {
    return false;
  }
  int64_t days = daysFromCivil(y, unsigned(mo), unsigned(d));
  out = double(days * 86400 + h * 3600 + mi * 60 + se - offset);
  return true;
}

// ------------------------------------------------------------------ JSON
void escapeTo(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '/': out += "\\/"; break;  // as Foundation's JSONEncoder
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += char(c);
        }
    }
  }
  out += '"';
}

std::string str(const char* s) { return s ? s : ""; }

// Pretty-printed with sorted keys (the layout Foundation's JSONEncoder wrote).
std::string encode(const rnf_resume_record& r) {
  std::vector<std::pair<std::string, std::string>> kv;
  if (r.has_at_take_end) kv.emplace_back("atTakeEnd", r.at_take_end ? "true" : "false");
  if (r.has_frame) kv.emplace_back("frame", std::to_string(r.frame));
  kv.emplace_back("hasContent", r.has_content ? "true" : "false");
  kv.emplace_back("isTemp", r.is_temp ? "true" : "false");
  if (r.mode == RNF_RESUME_MODE_RECORD || r.mode == RNF_RESUME_MODE_REPLAY) {
    std::string m;
    escapeTo(m, r.mode == RNF_RESUME_MODE_RECORD ? "record" : "replay");
    kv.emplace_back("mode", m);
  }
  if (r.has_practice_slot) kv.emplace_back("practiceSlot", std::to_string(r.practice_slot));
  std::string v;
  escapeTo(v, str(r.project_path));
  kv.emplace_back("projectPath", v);
  v.clear();
  escapeTo(v, str(r.rom_path));
  kv.emplace_back("romPath", v);
  v.clear();
  escapeTo(v, str(r.rom_sha256));
  kv.emplace_back("romSHA256", v);
  v.clear();
  escapeTo(v, iso8601(r.updated));
  kv.emplace_back("updated", v);
  kv.emplace_back("version", std::to_string(r.version));
  std::string out = "{\n";
  for (size_t i = 0; i < kv.size(); ++i) {
    out += "  ";
    escapeTo(out, kv[i].first);
    out += " : " + kv[i].second + (i + 1 < kv.size() ? ",\n" : "\n");
  }
  out += "}";
  return out;
}

bool asInteger(const rn::Json& j, int64_t& out) {
  if (j.type() == rn::Json::Type::Int) { out = j.asInt(); return true; }
  if (j.type() == rn::Json::Type::Double) {
    double d = j.asDouble();
    if (std::floor(d) != d || std::fabs(d) > 9.0e18) return false;
    out = int64_t(d);
    return true;
  }
  return false;
}

// Decodes like the original Codable record: required keys must be present with the right type,
// optional ones may be missing or null.
rn_status decode(const std::string& text, rnf_resume_record& r) {
  rn::Json root;
  std::string err;
  if (!rn::Json::parse(text, root, &err) || !root.isObject()) return fail(RN_ERR_CORRUPT, "resume record: " + err);
  auto missing = [](const char* k) { return fail(RN_ERR_CORRUPT, std::string("resume record: bad or missing ") + k); };
  int64_t iv;
  if (!root.has("version") || !asInteger(root["version"], iv)) return missing("version");
  r.version = int(iv);
  const rn::Json& pp = root["projectPath"];
  if (!pp.isString()) return missing("projectPath");
  const rn::Json& it = root["isTemp"];
  if (!it.isBool()) return missing("isTemp");
  const rn::Json& rp = root["romPath"];
  if (!rp.isString()) return missing("romPath");
  const rn::Json& rs = root["romSHA256"];
  if (!rs.isString()) return missing("romSHA256");
  const rn::Json& hc = root["hasContent"];
  if (!hc.isBool()) return missing("hasContent");
  const rn::Json& up = root["updated"];
  if (!up.isString() || !parseISO8601(up.asString(), r.updated)) return missing("updated");
  r.is_temp = it.asBool();
  r.has_content = hc.asBool();
  const rn::Json& fr = root["frame"];
  if (!fr.isNull()) {
    if (!asInteger(fr, iv) || iv < 0) return missing("frame");
    r.has_frame = 1;
    r.frame = uint64_t(iv);
  }
  const rn::Json& ae = root["atTakeEnd"];
  if (!ae.isNull()) {
    if (!ae.isBool()) return missing("atTakeEnd");
    r.has_at_take_end = 1;
    r.at_take_end = ae.asBool();
  }
  const rn::Json& mo = root["mode"];
  r.mode = RNF_RESUME_MODE_NONE;
  if (!mo.isNull()) {
    if (mo.isString() && mo.asString() == "record") r.mode = RNF_RESUME_MODE_RECORD;
    else if (mo.isString() && mo.asString() == "replay") r.mode = RNF_RESUME_MODE_REPLAY;
    else return missing("mode");
  }
  const rn::Json& ps = root["practiceSlot"];
  if (!ps.isNull()) {
    if (!asInteger(ps, iv)) return missing("practiceSlot");
    r.has_practice_slot = 1;
    r.practice_slot = int(iv);
  }
  r.project_path = dup(pp.asString());
  r.rom_path = dup(rp.asString());
  r.rom_sha256 = dup(rs.asString());
  return RN_OK;
}

void freeStrings(rnf_resume_record& r) {
  std::free(const_cast<char*>(r.project_path));
  std::free(const_cast<char*>(r.rom_path));
  std::free(const_cast<char*>(r.rom_sha256));
}

rnf_resume_record fresh(const std::string& projectPath, bool isTemp) {
  rnf_resume_record r{};
  r.version = RNF_RESUME_VERSION;
  r.project_path = dup(projectPath);
  r.is_temp = isTemp;
  r.rom_path = dup("");
  r.rom_sha256 = dup("");
  r.mode = RNF_RESUME_MODE_NONE;
  r.practice_slot = 0;
  r.has_content = 1;
  r.updated = double(std::time(nullptr));
  return r;
}

}  // namespace

struct rnf_session_lock {
#ifdef _WIN32
  HANDLE h = INVALID_HANDLE_VALUE;
#else
  int fd = -1;
#endif
};

extern "C" {

void rnf_resume_record_clear(rnf_resume_record* r) {
  if (!r) return;
  freeStrings(*r);
  *r = rnf_resume_record{};
}

int rnf_resume_same_state(const rnf_resume_record* a, const rnf_resume_record* b) {
  if (!a || !b) return a == b;
  auto optEq = [](int ha, uint64_t va, int hb, uint64_t vb) { return ha == hb && (!ha || va == vb); };
  return a->version == b->version && str(a->project_path) == str(b->project_path) && !a->is_temp == !b->is_temp &&
         str(a->rom_path) == str(b->rom_path) && str(a->rom_sha256) == str(b->rom_sha256) &&
         optEq(a->has_frame, a->frame, b->has_frame, b->frame) &&
         optEq(a->has_at_take_end, uint64_t(!!a->at_take_end), b->has_at_take_end, uint64_t(!!b->at_take_end)) &&
         a->mode == b->mode &&
         optEq(a->has_practice_slot, uint64_t(int64_t(a->practice_slot)), b->has_practice_slot,
               uint64_t(int64_t(b->practice_slot))) &&
         !a->has_content == !b->has_content;
}

rn_status rnf_resume_read(const char* path, rnf_resume_record* out, int* found) {
  if (found) *found = 0;
  if (!path || !out) return fail(RN_ERR_INVALID_ARG, "null argument");
  *out = rnf_resume_record{};
  RNF_GUARD_BEGIN
  if (!pathExists(path)) return RN_OK;
  std::string text;
  rn::Status st = rn::fs::readText(path, text);
  if (!st.ok()) return fail(RN_ERR_IO, "cannot read " + std::string(path));
  rnf_resume_record r{};
  rn_status ds = decode(text, r);
  if (ds != RN_OK) {
    freeStrings(r);
    return ds;
  }
  if (r.version > RNF_RESUME_VERSION || str(r.project_path).empty()) {
    freeStrings(r);
    return fail(RN_ERR_CORRUPT, "resume record: unsupported version or empty project path");
  }
  *out = r;
  if (found) *found = 1;
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

rn_status rnf_resume_write(const char* path, const rnf_resume_record* r) {
  if (!path || !r) return fail(RN_ERR_INVALID_ARG, "null argument");
  RNF_GUARD_BEGIN
  rn::Status st = rn::fs::writeFileAtomic(path, encode(*r));
  if (!st.ok()) return fail(RN_ERR_IO, "cannot write " + std::string(path));
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

void rnf_resume_clear(const char* path) {
  if (!path) return;
  std::error_code ec;
  stdfs::remove(stdfs::u8path(path), ec);
}

rnf_resume_decision rnf_resume_decide(const char* session_root, int explicit_open, const char* legacy,
                                      rnf_resume_record* out) {
  if (out) *out = rnf_resume_record{};
  if (explicit_open || !session_root || !out) return RNF_RESUME_NONE;
  RNF_GUARD_BEGIN
  std::string root = session_root;
  std::string temp = joinPath(root, RNF_SESSION_TEMP_PROJECT);
  std::string resume = joinPath(root, RNF_SESSION_RESUME_FILE);
  rnf_resume_record r{};
  int found = 0;
  if (rnf_resume_read(resume.c_str(), &r, &found) != RN_OK) found = 0;
  if (found) {
    if (r.is_temp) {
      // Always the temporary project of this folder (never a path from the record).
      if (!pathExists(temp)) {
        freeStrings(r);
        return RNF_RESUME_NONE;
      }
      std::free(const_cast<char*>(r.project_path));
      r.project_path = dup(temp);
      *out = r;
      return RNF_RESUME_RESUME;
    }
    bool exists = pathExists(str(r.project_path));
    *out = r;
    return exists ? RNF_RESUME_RESUME : RNF_RESUME_PROJECT_MISSING;
  }
  // No (or unreadable) record: a temporary project left behind is still resumed, at its own cursor.
  if (pathExists(temp)) {
    *out = fresh(temp, true);
    return RNF_RESUME_RESUME;
  }
  if (legacy && *legacy && pathExists(legacy)) {
    *out = fresh(legacy, false);
    return RNF_RESUME_RESUME;
  }
  return RNF_RESUME_NONE;
  RNF_GUARD_END(RNF_RESUME_NONE)
}

int rnf_resume_target_frame(const rnf_resume_record* r, uint64_t take_length, uint64_t* out) {
  if (!r) return 0;
  if (r->has_at_take_end && r->at_take_end) {
    if (out) *out = take_length;
    return 1;
  }
  if (!r->has_frame) return 0;
  if (out) *out = r->frame < take_length ? r->frame : take_length;
  return 1;
}

rn_status rnf_resume_apply(const rnf_resume_record* r, rn_session* s) {
  if (!r || !s) return fail(RN_ERR_INVALID_ARG, "null argument");
  if (r->mode != RNF_RESUME_MODE_NONE && rn_get_mode(s) != RN_MODE_PRACTICE) {
    rn_mode want = r->mode == RNF_RESUME_MODE_RECORD ? RN_MODE_RECORD : RN_MODE_REPLAY;
    if (rn_get_mode(s) != want) {
      rn_status st = rn_set_mode(s, want);
      if (st != RN_OK) return st;  // rn_last_error() has the engine's message
    }
  }
  uint64_t f;
  if (rnf_resume_target_frame(r, rn_take_length(s), &f) && f != rn_frame(s)) return rn_seek(s, f);
  return RN_OK;
}

int rnf_session_has_recorded_content(rn_session* s) {
  if (!s) return 0;
  size_t n = rn_take_count(s);
  for (size_t i = 0; i < n; ++i) {
    rn_take_info t{};
    if (rn_take_get(s, i, &t) == RN_OK && t.length > 0) return 1;
  }
  size_t nb = rn_bookmark_count(s);
  for (size_t i = 0; i < nb; ++i) {
    rn_bookmark_info b{};
    if (rn_bookmark_get(s, i, &b) == RN_OK) return 1;
  }
  for (uint32_t i = 0; i < RN_PRACTICE_SLOTS; ++i) {
    rn_practice_slot_info p{};
    if (rn_practice_slot_get(s, i, &p) == RN_OK && p.has_a) return 1;
  }
  return 0;
}

int rnf_paths_equal(const char* a, const char* b) {
  if (!a || !b || !*a || !*b) return 0;
  try {
    return normalized(a) == normalized(b);
  } catch (...) {
    return 0;
  }
}

rnf_session_lock* rnf_session_lock_acquire(const char* lock_file) {
  if (!lock_file) return nullptr;
#ifdef _WIN32
  int n = MultiByteToWideChar(CP_UTF8, 0, lock_file, -1, nullptr, 0);
  std::wstring w(size_t(n > 0 ? n : 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, lock_file, -1, &w[0], n);
  HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  OVERLAPPED ov{};
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
    CloseHandle(h);
    return nullptr;
  }
  auto* l = new rnf_session_lock;
  l->h = h;
  return l;
#else
  int fd = ::open(lock_file, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) return nullptr;
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return nullptr;
  }
  auto* l = new rnf_session_lock;
  l->fd = fd;
  return l;
#endif
}

void rnf_session_lock_release(rnf_session_lock* l) {
  if (!l) return;
#ifdef _WIN32
  OVERLAPPED ov{};
  UnlockFileEx(l->h, 0, 1, 0, &ov);
  CloseHandle(l->h);
#else
  ::flock(l->fd, LOCK_UN);
  ::close(l->fd);
#endif
  delete l;
}

}  // extern "C"
