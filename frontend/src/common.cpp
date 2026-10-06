// Error reporting, strings, lists, localization table and Apple-style formatting.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "common.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace rnf {
#include "l10n_table.inc"
}

namespace rnf {

namespace {
thread_local std::string tlsError;
std::atomic<int> gLanguage{0};  // 0 = en, 1 = ja
constexpr size_t kTableSize = sizeof(kL10nTable) / sizeof(kL10nTable[0]);

const L10nEntry* findEntry(const char* key) {
  if (!key) return nullptr;
  size_t lo = 0, hi = kTableSize;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = std::strcmp(kL10nTable[mid].key, key);
    if (c == 0) return &kL10nTable[mid];
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return nullptr;
}

int langIndex(const char* lang) {
  if (!lang) return 0;
  return (lang[0] == 'j' || lang[0] == 'J') && (lang[1] == 'a' || lang[1] == 'A') ? 1 : 0;
}

const char* lookup(int lang, const char* key) {
  const L10nEntry* e = findEntry(key);
  if (!e) return key;
  return lang == 1 ? e->ja : e->en;
}
}  // namespace

void setError(const std::string& msg) { tlsError = msg; }
rn_status fail(rn_status st, const std::string& msg) {
  tlsError = msg;
  return st;
}

char* dup(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) return nullptr;
  std::memcpy(p, s.data(), s.size());
  p[s.size()] = 0;
  return p;
}

const char* tr(const char* key) { return lookup(gLanguage.load(std::memory_order_relaxed), key); }

std::string format(const char* fmt, const std::vector<rnf_arg>& args) {
  std::string out;
  if (!fmt) return out;
  size_t seq = 0;
  const char* p = fmt;
  while (*p) {
    if (*p != '%') { out += *p++; continue; }
    const char* start = p++;
    if (*p == '%') { out += '%'; ++p; continue; }
    // Optional positional index "n$".
    size_t pos = 0;
    const char* q = p;
    while (*q >= '0' && *q <= '9') ++q;
    if (q != p && *q == '$') {
      pos = std::strtoul(p, nullptr, 10);
      p = q + 1;
    }
    std::string spec = "%";
    while (*p && std::strchr("-+#0 ", *p)) spec += *p++;
    while (*p >= '0' && *p <= '9') spec += *p++;
    if (*p == '.') { spec += *p++; while (*p >= '0' && *p <= '9') spec += *p++; }
    while (*p && std::strchr("lhqztjL", *p)) ++p;  // length modifiers: normalized below
    char conv = *p;
    if (!conv) { out.append(start); break; }
    ++p;
    size_t idx = pos ? pos - 1 : seq++;
    rnf_arg a{};
    bool have = idx < args.size();
    if (have) a = args[idx];
    char buf[128];
    switch (conv) {
      case '@': case 's': {
        if (!have) break;
        if (a.type == RNF_ARG_STRING) out += a.s ? a.s : "(null)";
        else if (a.type == RNF_ARG_INT) out += std::to_string(a.i);
        else if (a.type == RNF_ARG_UINT) out += std::to_string(a.u);
        else { std::snprintf(buf, sizeof buf, "%g", a.d); out += buf; }
        break;
      }
      case 'd': case 'i': case 'D': {
        long long v = !have ? 0 : a.type == RNF_ARG_INT ? a.i : a.type == RNF_ARG_UINT ? (long long)a.u
                     : a.type == RNF_ARG_DOUBLE ? (long long)a.d : 0;
        std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), v);
        out += buf;
        break;
      }
      case 'u': case 'U': case 'x': case 'X': case 'o': case 'O': {
        unsigned long long v = !have ? 0 : a.type == RNF_ARG_UINT ? a.u : a.type == RNF_ARG_INT ? (unsigned long long)a.i
                              : a.type == RNF_ARG_DOUBLE ? (unsigned long long)a.d : 0;
        char c = conv == 'U' ? 'u' : conv == 'O' ? 'o' : conv;
        std::snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), v);
        out += buf;
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double v = !have ? 0 : a.type == RNF_ARG_DOUBLE ? a.d : a.type == RNF_ARG_INT ? (double)a.i
                  : a.type == RNF_ARG_UINT ? (double)a.u : 0;
        std::snprintf(buf, sizeof buf, (spec + conv).c_str(), v);
        out += buf;
        break;
      }
      case 'c': {
        if (have) out += static_cast<char>(a.type == RNF_ARG_UINT ? a.u : a.i);
        break;
      }
      default:
        out.append(start, p);
        break;
    }
  }
  return out;
}

std::string trf(const char* key, const std::vector<rnf_arg>& args) { return format(tr(key), args); }

bool hasPrefix(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool hasSuffix(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
std::string lower(const std::string& s) {
  std::string o = s;
  for (char& c : o) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return o;
}

rnf_list* makeList(const Pairs& p) {
  auto* l = new rnf_list;
  for (auto& e : p) l->items.push_back({e.first, e.second, 0});
  return l;
}

Pairs toPairs(const rnf_binding* b, size_t n) {
  Pairs out;
  if (!b) return out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) out.emplace_back(b[i].input ? b[i].input : "", b[i].action ? b[i].action : "");
  return out;
}

}  // namespace rnf

using namespace rnf;

extern "C" {

uint32_t rnf_api_version(void) { return RNF_API_VERSION; }
const char* rnf_last_error(void) { return tlsError.c_str(); }
void rnf_string_free(char* s) { std::free(s); }
double rnf_frame_period(void) { return double(RN_FPS_DEN) / double(RN_FPS_NUM); }

const char* rnf_ui_language_choose(const char* const* preferred, size_t count) {
  if (!preferred || count == 0 || !preferred[0]) return "en";
  return hasPrefix(lower(preferred[0]), "ja") ? "ja" : "en";
}

void rnf_l10n_set_language(const char* lang) { gLanguage.store(langIndex(lang)); }
const char* rnf_l10n_language(void) { return gLanguage.load() == 1 ? "ja" : "en"; }
const char* rnf_l10n_lookup(const char* key) { return tr(key); }
const char* rnf_l10n_lookup_in(const char* lang, const char* key) { return lookup(langIndex(lang), key); }
size_t rnf_l10n_count(void) { return kTableSize; }
int rnf_l10n_entry(size_t index, const char** key, const char** en, const char** ja) {
  if (index >= kTableSize) return 0;
  if (key) *key = kL10nTable[index].key;
  if (en) *en = kL10nTable[index].en;
  if (ja) *ja = kL10nTable[index].ja;
  return 1;
}

char* rnf_format(const char* fmt, const rnf_arg* args, size_t count) {
  RNF_GUARD_BEGIN
  return dup(format(fmt, std::vector<rnf_arg>(args, args + (args ? count : 0))));
  RNF_GUARD_END(nullptr)
}
char* rnf_l10n_format(const char* key, const rnf_arg* args, size_t count) {
  RNF_GUARD_BEGIN
  return dup(format(tr(key), std::vector<rnf_arg>(args, args + (args ? count : 0))));
  RNF_GUARD_END(nullptr)
}

size_t rnf_list_count(const rnf_list* l) { return l ? l->items.size() : 0; }
const char* rnf_list_a(const rnf_list* l, size_t i) { return l && i < l->items.size() ? l->items[i].a.c_str() : ""; }
const char* rnf_list_b(const rnf_list* l, size_t i) { return l && i < l->items.size() ? l->items[i].b.c_str() : ""; }
int64_t rnf_list_value(const rnf_list* l, size_t i) { return l && i < l->items.size() ? l->items[i].value : 0; }
void rnf_list_free(rnf_list* l) { delete l; }

}  // extern "C"
