// SPDX-License-Identifier: GPL-2.0-or-later
#include "manifest.h"

#include <cstdlib>
#include <map>

#include "replaynes/replaynes.h"

namespace rnl {

namespace {
// Values of the object members we care about, by dotted path ("rom.name").
struct Reader {
  const std::string& s;
  size_t i = 0;
  std::map<std::string, std::string> strings;
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
  }
  bool str(std::string* out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    std::string v;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') {
        v += c;
        continue;
      }
      if (i >= s.size()) return false;
      char e = s[i++];
      switch (e) {
        case 'n': v += '\n'; break;
        case 't': v += '\t'; break;
        case 'r': v += '\r'; break;
        case 'b': v += '\b'; break;
        case 'f': v += '\f'; break;
        case 'u': {
          if (i + 4 > s.size()) return false;
          unsigned cp = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
          i += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            unsigned lo = unsigned(std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16));
            i += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          if (cp < 0x80) v += char(cp);
          else if (cp < 0x800) { v += char(0xC0 | (cp >> 6)); v += char(0x80 | (cp & 0x3F)); }
          else if (cp < 0x10000) { v += char(0xE0 | (cp >> 12)); v += char(0x80 | ((cp >> 6) & 0x3F)); v += char(0x80 | (cp & 0x3F)); }
          else { v += char(0xF0 | (cp >> 18)); v += char(0x80 | ((cp >> 12) & 0x3F)); v += char(0x80 | ((cp >> 6) & 0x3F)); v += char(0x80 | (cp & 0x3F)); }
          break;
        }
        default: v += e; break;
      }
    }
    if (i >= s.size()) return false;
    ++i;
    *out = v;
    return true;
  }
  bool value(const std::string& path, int depth) {
    if (depth > 32) return false;
    ws();
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '{') {
      ++i;
      ws();
      if (i < s.size() && s[i] == '}') { ++i; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!str(&k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        if (!value(path.empty() ? k : path + "." + k, depth + 1)) return false;
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++i;
      ws();
      if (i < s.size() && s[i] == ']') { ++i; return true; }
      for (;;) {
        if (!value(path + "[]", depth + 1)) return false;
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        return false;
      }
    }
    if (c == '"') {
      std::string v;
      if (!str(&v)) return false;
      strings[path] = v;
      return true;
    }
    // number / true / false / null
    size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') ++i;
    return i > start;
  }
};
}  // namespace

ManifestInfo parseManifest(const std::string& json) {
  ManifestInfo m;
  Reader r{json};
  r.value("", 0);  // partial results are kept even if the text is damaged further on
  auto get = [&](const char* k, std::string* out) {
    auto it = r.strings.find(k);
    if (it != r.strings.end() && !it->second.empty()) *out = it->second;
  };
  get("rom.name", &m.romName);
  get("rom.lastPath", &m.romPath);
  get("rom.sha256", &m.romSHA256);
  get("coreCompatId", &m.coreCompatID);
  return m;
}

ManifestInfo readManifest(const std::string& projectDir) {
  char* json = nullptr;
  if (rn_project_manifest_json(projectDir.c_str(), &json) != RN_OK || !json) return ManifestInfo();
  ManifestInfo m = parseManifest(json);
  rn_string_free(json);
  return m;
}

}  // namespace rnl
