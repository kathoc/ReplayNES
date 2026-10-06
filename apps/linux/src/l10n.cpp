// SPDX-License-Identifier: GPL-2.0-or-later
#include "l10n.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>

namespace rnl {

namespace {
bool hasVariationSelector(const char* s) { return std::strstr(s, "\xEF\xB8\x8E") || std::strstr(s, "\xEF\xB8\x8F"); }
std::string stripVariationSelectors(const char* s) {
  std::string out;
  for (const char* p = s; *p; ++p) {
    if (p[0] == '\xEF' && p[1] == '\xB8' && (p[2] == '\x8E' || p[2] == '\x8F')) {
      p += 2;
      continue;
    }
    out += *p;
  }
  return out;
}
}  // namespace

const char* TR(const char* key) {
  const char* v = rnf_l10n_lookup(key);
  if (!hasVariationSelector(v)) return v;
  static std::mutex m;
  static std::unordered_map<const char*, std::string> cache;  // keyed by the table's static string
  std::lock_guard<std::mutex> lk(m);
  auto it = cache.find(v);
  if (it == cache.end()) it = cache.emplace(v, stripVariationSelectors(v)).first;
  return it->second.c_str();
}

std::string TRF(const char* key, std::initializer_list<L10nArg> args) {
  std::vector<rnf_arg> v;
  v.reserve(args.size());
  for (const L10nArg& a : args) {
    rnf_arg x = a.a;
    if (x.type == RNF_ARG_STRING) x.s = a.keep.c_str();
    v.push_back(x);
  }
  char* s = rnf_l10n_format(key, v.data(), v.size());
  std::string out = s ? s : key;
  rnf_string_free(s);
  return hasVariationSelector(out.c_str()) ? stripVariationSelectors(out.c_str()) : out;
}

std::string chooseUILanguage(const std::vector<std::string>& preferred, const char* override_) {
  if (override_ && *override_) {
    const char* one[] = {override_};
    return rnf_ui_language_choose(one, 1);
  }
  std::vector<const char*> p;
  p.reserve(preferred.size());
  for (const std::string& s : preferred) p.push_back(s.c_str());
  return rnf_ui_language_choose(p.data(), p.size());
}

std::string timecode(uint64_t frame) {
  double t = double(frame) * rnf_frame_period();
  int m = int(t) / 60;
  double s = t - double(m * 60);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%05.2f", m, s);
  return buf;
}

std::string localDateTime(double seconds) {
  std::time_t t = std::time_t(std::floor(seconds));
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

}  // namespace rnl
