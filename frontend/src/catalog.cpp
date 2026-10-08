// Library catalog (rnf_library_prefs / rnf_library_arrange): favourites, play history, the chosen
// sort / filter (persisted as library.json in the settings folder), the order of the library
// screen, and the localized display strings of a game (rnf_game_*).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "collate.hpp"
#include "common.hpp"
#include "util/Fs.h"
#include "util/Json.h"

using namespace rnf;

namespace rnf {
std::string titleFromFileName(const std::string& fileName);
}

namespace {

bool japanese() { return std::strcmp(rnf_l10n_language(), "ja") == 0; }

std::string s(const char* p) { return p ? p : ""; }

std::string title(const rnf_game_info* g, const std::string& fileName, bool ja) {
  if (g) {
    std::string t = ja ? s(g->title_ja) : s(g->title_en);
    if (t.empty()) t = ja ? s(g->title_en) : s(g->title_ja);
    if (!t.empty()) return t;
  }
  return titleFromFileName(fileName);
}

std::string publisher(const rnf_game_info* g, bool ja) {
  if (!g) return "";
  std::string p = ja ? s(g->publisher_ja) : s(g->publisher_en);
  return p.empty() ? s(g->publisher_en) : p;
}

std::string joinDot(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& p : parts) {
    if (p.empty()) continue;
    if (!out.empty()) out += tr(RNF_L(" · "));
    out += p;
  }
  return out;
}

const char* const kSortCodes[RNF_LIBRARY_SORT_COUNT] = {"name", "recent", "publisher", "year", "genre"};
const char* const kFilterCodes[RNF_LIBRARY_FILTER_COUNT] = {"all", "favorites", "recent"};

struct History {
  std::string key;
  double last = 0, seconds = 0;
  int64_t plays = 0;
};

}  // namespace

struct rnf_library_prefs {
  std::set<std::string> favorites;
  std::vector<History> history;  // newest first
  rnf_library_sort sort = RNF_LIBRARY_SORT_NAME;
  rnf_library_filter filter = RNF_LIBRARY_FILTER_ALL;
};

namespace {

const History* findHistory(const rnf_library_prefs* p, const std::string& key) {
  if (!p || key.empty()) return nullptr;
  for (const History& h : p->history)
    if (h.key == key) return &h;
  return nullptr;
}

// Search: width-, case- and kana-insensitive (katakana folded to hiragana), spaces ignored.
std::string searchFold(const std::string& text) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    uint32_t cp = foldWidth(nextCodePoint(text, i));
    if (cp == ' ' || cp == 0x30FB) continue;
    if (cp < 0x80) out += char(std::tolower(int(cp)));
    else appendUtf8(out, katakanaToHiragana(cp));
  }
  return out;
}

}  // namespace

extern "C" {

// ------------------------------------------------------------------ display strings

char* rnf_game_title(const rnf_game_info* info, const char* file_name) {
  try {
    return dup(title(info, s(file_name), japanese()));
  } catch (...) {
    return nullptr;
  }
}

char* rnf_game_publisher(const rnf_game_info* info) {
  try {
    return dup(publisher(info, japanese()));
  } catch (...) {
    return nullptr;
  }
}

char* rnf_game_byline(const rnf_game_info* info) {
  try {
    if (!info) return dup("");
    return dup(joinDot({publisher(info, japanese()), info->year > 0 ? std::to_string(info->year) : ""}));
  } catch (...) {
    return nullptr;
  }
}

char* rnf_game_details(const rnf_game_info* info) {
  try {
    if (!info) return dup("");
    return dup(joinDot({publisher(info, japanese()), info->year > 0 ? std::to_string(info->year) : "", rnf_genre_name(info->genre)}));
  } catch (...) {
    return nullptr;
  }
}

const char* rnf_library_sort_name(rnf_library_sort v) {
  switch (v) {
    case RNF_LIBRARY_SORT_NAME: return tr(RNF_L("Name"));
    case RNF_LIBRARY_SORT_RECENT: return tr(RNF_L("Last Played"));
    case RNF_LIBRARY_SORT_PUBLISHER: return tr(RNF_L("Maker"));
    case RNF_LIBRARY_SORT_YEAR: return tr(RNF_L("Year"));
    case RNF_LIBRARY_SORT_GENRE: return tr(RNF_L("Genre"));
    default: return "";
  }
}

const char* rnf_library_filter_name(rnf_library_filter v) {
  switch (v) {
    case RNF_LIBRARY_FILTER_ALL: return tr(RNF_L("All (games)"));
    case RNF_LIBRARY_FILTER_FAVORITES: return tr(RNF_L("Favorites"));
    case RNF_LIBRARY_FILTER_RECENT: return tr(RNF_L("Recent"));
    default: return "";
  }
}

// ------------------------------------------------------------------ prefs

rnf_library_prefs* rnf_library_prefs_new(void) {
  try {
    return new rnf_library_prefs;
  } catch (...) {
    return nullptr;
  }
}

void rnf_library_prefs_free(rnf_library_prefs* p) { delete p; }

rn_status rnf_library_prefs_load(rnf_library_prefs* p, const char* path) {
  if (!p || !path) return fail(RN_ERR_INVALID_ARG, "null argument");
  RNF_GUARD_BEGIN
  *p = rnf_library_prefs{};
  if (!rn::fs::exists(path)) return RN_OK;
  std::string text;
  if (!rn::fs::readText(path, text).ok()) return fail(RN_ERR_IO, std::string("cannot read ") + path);
  rn::Json j;
  std::string err;
  if (!rn::Json::parse(text, j, &err) || !j.isObject()) return fail(RN_ERR_CORRUPT, path + std::string(": ") + err);
  const rn::Json& sort = j["sort"];
  if (sort.isString())
    for (int i = 0; i < RNF_LIBRARY_SORT_COUNT; ++i)
      if (sort.asString() == kSortCodes[i]) p->sort = rnf_library_sort(i);
  const rn::Json& filter = j["filter"];
  if (filter.isString())
    for (int i = 0; i < RNF_LIBRARY_FILTER_COUNT; ++i)
      if (filter.asString() == kFilterCodes[i]) p->filter = rnf_library_filter(i);
  for (const rn::Json& f : j["favorites"].items())
    if (f.isString() && !f.asString().empty()) p->favorites.insert(f.asString());
  for (const rn::Json& h : j["history"].items()) {
    if (!h["rom"].isString() || h["rom"].asString().empty() || findHistory(p, h["rom"].asString())) continue;
    History e;
    e.key = h["rom"].asString();
    e.last = h["last"].isNumber() ? h["last"].asDouble() : 0;
    e.seconds = h["seconds"].isNumber() ? std::max(0.0, h["seconds"].asDouble()) : 0;
    e.plays = h["plays"].isNumber() ? std::max<int64_t>(0, h["plays"].asInt()) : 0;
    p->history.push_back(e);
  }
  std::stable_sort(p->history.begin(), p->history.end(), [](const History& a, const History& b) { return a.last > b.last; });
  if (p->history.size() > RNF_LIBRARY_HISTORY_MAX) p->history.resize(RNF_LIBRARY_HISTORY_MAX);
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

rn_status rnf_library_prefs_save(const rnf_library_prefs* p, const char* path) {
  if (!p || !path) return fail(RN_ERR_INVALID_ARG, "null argument");
  RNF_GUARD_BEGIN
  rn::Json j = rn::Json::object();
  j.set("version", 1);
  j.set("sort", kSortCodes[p->sort]);
  j.set("filter", kFilterCodes[p->filter]);
  rn::Json fav = rn::Json::array();
  for (const std::string& f : p->favorites) fav.push(f);
  j.set("favorites", fav);
  rn::Json hist = rn::Json::array();
  for (const History& h : p->history) {
    rn::Json e = rn::Json::object();
    e.set("rom", h.key);
    e.set("last", std::floor(h.last));
    e.set("seconds", std::floor(h.seconds));
    e.set("plays", h.plays);
    hist.push(e);
  }
  j.set("history", hist);
  std::string dir = path;
  while (!dir.empty() && !isPathSep(dir.back())) dir.pop_back();
  if (!dir.empty()) rn::fs::createDirs(dir);
  if (!rn::fs::writeFileAtomic(path, j.dump(2) + "\n").ok()) return fail(RN_ERR_IO, std::string("cannot write ") + path);
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

int rnf_library_prefs_is_favorite(const rnf_library_prefs* p, const char* key) {
  return p && key && *key && p->favorites.count(key) ? 1 : 0;
}

void rnf_library_prefs_set_favorite(rnf_library_prefs* p, const char* key, int favorite) {
  if (!p || !key || !*key) return;
  try {
    if (favorite) p->favorites.insert(key);
    else p->favorites.erase(key);
  } catch (...) {
  }
}

int rnf_library_prefs_toggle_favorite(rnf_library_prefs* p, const char* key) {
  if (!p || !key || !*key) return 0;
  int on = rnf_library_prefs_is_favorite(p, key) ? 0 : 1;
  rnf_library_prefs_set_favorite(p, key, on);
  return on;
}

void rnf_library_prefs_record_play(rnf_library_prefs* p, const char* key, double date, double seconds) {
  if (!p || !key || !*key) return;
  try {
    History e;
    e.key = key;
    for (auto it = p->history.begin(); it != p->history.end(); ++it) {
      if (it->key == e.key) {
        e = *it;
        p->history.erase(it);
        break;
      }
    }
    if (e.last != date) e.plays += 1;  // a new session (the same start date continues one)
    e.last = date;
    e.seconds += std::max(0.0, seconds);
    p->history.insert(p->history.begin(), e);
    if (p->history.size() > RNF_LIBRARY_HISTORY_MAX) p->history.resize(RNF_LIBRARY_HISTORY_MAX);
  } catch (...) {
  }
}

size_t rnf_library_prefs_history_count(const rnf_library_prefs* p) { return p ? p->history.size() : 0; }

static void fillHistory(const History& h, rnf_library_history_entry* out) {
  if (out) *out = rnf_library_history_entry{h.key.c_str(), h.last, h.seconds, h.plays};
}

int rnf_library_prefs_history_get(const rnf_library_prefs* p, size_t index, rnf_library_history_entry* out) {
  if (!p || index >= p->history.size()) return 0;
  fillHistory(p->history[index], out);
  return 1;
}

int rnf_library_prefs_history_find(const rnf_library_prefs* p, const char* key, rnf_library_history_entry* out) {
  const History* h = findHistory(p, s(key));
  if (!h) return 0;
  fillHistory(*h, out);
  return 1;
}

void rnf_library_prefs_forget(rnf_library_prefs* p, const char* key) {
  if (!p || !key) return;
  p->history.erase(std::remove_if(p->history.begin(), p->history.end(), [&](const History& h) { return h.key == key; }), p->history.end());
}

rnf_library_sort rnf_library_prefs_sort(const rnf_library_prefs* p) { return p ? p->sort : RNF_LIBRARY_SORT_NAME; }
void rnf_library_prefs_set_sort(rnf_library_prefs* p, rnf_library_sort v) {
  if (p && int(v) >= 0 && v < RNF_LIBRARY_SORT_COUNT) p->sort = v;
}
rnf_library_filter rnf_library_prefs_filter(const rnf_library_prefs* p) { return p ? p->filter : RNF_LIBRARY_FILTER_ALL; }
void rnf_library_prefs_set_filter(rnf_library_prefs* p, rnf_library_filter v) {
  if (p && int(v) >= 0 && v < RNF_LIBRARY_FILTER_COUNT) p->filter = v;
}

// ------------------------------------------------------------------ arrange

size_t rnf_library_arrange(const rnf_library_item* items, size_t count, rnf_library_sort sort, rnf_library_filter filter,
                           const char* query, const rnf_library_prefs* prefs, size_t* out, size_t cap) {
  if (!items && count) return 0;
  try {
    bool ja = japanese();
    struct Row {
      size_t index;
      std::string title, publisher;
      std::vector<CollationElement> titleKey, publisherKey;
      int year;
      int genre;
      double last;  // 0: never played
      size_t recentRank;
      std::string rel;
    };
    std::string q = searchFold(s(query));
    // History rank of each key (newest first).
    std::vector<std::pair<std::string, size_t>> ranks;
    std::vector<Row> rows;
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const rnf_library_item& it = items[i];
      std::string key = s(it.key), file = s(it.file_name);
      const History* h = findHistory(prefs, key);
      size_t rank = SIZE_MAX;
      if (h && prefs) rank = size_t(h - prefs->history.data());
      if (filter == RNF_LIBRARY_FILTER_FAVORITES && !rnf_library_prefs_is_favorite(prefs, key.c_str())) continue;
      if (filter == RNF_LIBRARY_FILTER_RECENT && rank >= RNF_LIBRARY_RECENT_COUNT) continue;
      Row r;
      r.index = i;
      r.title = title(it.game, file, ja);
      if (!q.empty()) {
        bool hit = searchFold(r.title).find(q) != std::string::npos || searchFold(file).find(q) != std::string::npos;
        if (it.game) {
          for (const char* f : {it.game->title_en, it.game->title_ja, it.game->reading})
            hit = hit || searchFold(s(f)).find(q) != std::string::npos;
        }
        if (!hit) continue;
      }
      r.publisher = publisher(it.game, ja);
      // Japanese: by the kana reading when there is one.
      std::string sortText = r.title;
      if (ja && it.game && it.game->reading && *it.game->reading) sortText = it.game->reading;
      r.titleKey = collationElements(sortText, ja);
      r.publisherKey = collationElements(r.publisher, ja);
      r.year = it.game ? it.game->year : 0;
      r.genre = it.game ? int(it.game->genre) : 0;
      r.last = h ? h->last : 0;
      r.recentRank = rank;
      r.rel = s(it.relative_path);
      rows.push_back(std::move(r));
    }
    auto byTitle = [](const Row& a, const Row& b) {
      int c = compareElements(a.titleKey, b.titleKey);
      if (c) return c < 0;
      if (a.title != b.title) return a.title < b.title;
      if (a.rel != b.rel) return a.rel < b.rel;
      return a.index < b.index;
    };
    rnf_library_sort mode = filter == RNF_LIBRARY_FILTER_RECENT ? RNF_LIBRARY_SORT_RECENT : sort;
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
      switch (mode) {
        case RNF_LIBRARY_SORT_RECENT:
          if (a.recentRank != b.recentRank) return a.recentRank < b.recentRank;
          break;
        case RNF_LIBRARY_SORT_PUBLISHER: {
          bool ea = a.publisher.empty(), eb = b.publisher.empty();
          if (ea != eb) return eb;  // unknown makers last
          int c = compareElements(a.publisherKey, b.publisherKey);
          if (c) return c < 0;
          break;
        }
        case RNF_LIBRARY_SORT_YEAR:
          if ((a.year == 0) != (b.year == 0)) return b.year == 0;
          if (a.year != b.year) return a.year < b.year;
          break;
        case RNF_LIBRARY_SORT_GENRE:
          if ((a.genre == 0) != (b.genre == 0)) return b.genre == 0;
          if (a.genre != b.genre) return a.genre < b.genre;
          break;
        default: break;
      }
      return byTitle(a, b);
    });
    for (size_t i = 0; i < rows.size() && i < cap; ++i) out[i] = rows[i].index;
    return rows.size();
  } catch (...) {
    return 0;
  }
}

}  // extern "C"
