// Internal helpers of the frontend core (not part of the C API).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "replaynes/frontend.h"

// Marks a localization key: check-l10n.py collects RNF_L("...") literals from frontend/src as
// keys used by the app, and the generated table resolves them at run time.
#define RNF_L(s) s

namespace rnf {

struct L10nEntry {
  const char* key;
  const char* en;
  const char* ja;
};

void setError(const std::string& msg);
rn_status fail(rn_status st, const std::string& msg);
char* dup(const std::string& s);

// Localized value of key in the current language.
const char* tr(const char* key);
// Apple-style format (see rnf_format).
std::string format(const char* fmt, const std::vector<rnf_arg>& args);
inline rnf_arg argS(const std::string& s) { rnf_arg a{}; a.type = RNF_ARG_STRING; a.s = s.c_str(); return a; }
inline rnf_arg argI(int64_t v) { rnf_arg a{}; a.type = RNF_ARG_INT; a.i = v; return a; }
inline rnf_arg argU(uint64_t v) { rnf_arg a{}; a.type = RNF_ARG_UINT; a.u = v; return a; }
// tr(key) formatted with args.
std::string trf(const char* key, const std::vector<rnf_arg>& args);

// Paths are UTF-8 strings. Windows accepts both separators and joins with '\\' (as the engine's
// rn::fs::join does); elsewhere only '/' is a separator.
#ifdef _WIN32
constexpr char kPathSep = '\\';
inline bool isPathSep(char c) { return c == '/' || c == '\\'; }
#else
constexpr char kPathSep = '/';
inline bool isPathSep(char c) { return c == '/'; }
#endif
// dir + separator + name (no doubled separator; empty dir -> name).
inline std::string joinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  return isPathSep(dir.back()) ? dir + name : dir + kPathSep + name;
}
// Without trailing separators; a root ("/", "C:\\") is kept.
inline std::string stripTrailingSeps(std::string s) {
  while (s.size() > 1 && isPathSep(s.back())) {
#ifdef _WIN32
    if (s.size() == 3 && s[1] == ':') break;
#endif
    s.pop_back();
  }
  return s;
}
// Index of the last separator, npos if none.
inline size_t lastPathSep(const std::string& s) {
  for (size_t i = s.size(); i-- > 0;)
    if (isPathSep(s[i])) return i;
  return std::string::npos;
}

bool hasPrefix(const std::string& s, const std::string& p);
bool hasSuffix(const std::string& s, const std::string& p);
std::string lower(const std::string& s);  // ASCII

}  // namespace rnf

// Immutable (a, b, value) list behind rnf_list.
struct rnf_list {
  struct Item {
    std::string a, b;
    int64_t value = 0;
  };
  std::vector<Item> items;
};

namespace rnf {
using Pairs = std::vector<std::pair<std::string, std::string>>;
rnf_list* makeList(const Pairs& p);
Pairs toPairs(const rnf_binding* b, size_t n);
}  // namespace rnf

// Wraps a C API body: no exception crosses the boundary.
#define RNF_GUARD_BEGIN try {
#define RNF_GUARD_END(ret)                                       \
  }                                                              \
  catch (const std::exception& e) {                              \
    rnf::setError(std::string("internal error: ") + e.what());   \
    return ret;                                                  \
  }                                                              \
  catch (...) {                                                  \
    rnf::setError("internal error");                             \
    return ret;                                                  \
  }
