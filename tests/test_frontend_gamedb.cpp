// Frontend core: game database (lookup by PRG+CHR hash and by file name), Japanese / English title
// collation, and the library catalog (favourites, play history, sort / filter / search, persistence).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

struct LangScope {
  std::string saved;
  explicit LangScope(const char* lang) : saved(rnf_l10n_language()) { rnf_l10n_set_language(lang); }
  ~LangScope() { rnf_l10n_set_language(saved.c_str()); }
};

int cmpJa(const char* a, const char* b) { return rnf_collate(a, b, "ja"); }
int cmpEn(const char* a, const char* b) { return rnf_collate(a, b, "en"); }

rnf_game_info game(const char* en, const char* ja, const char* reading, const char* pubEn, const char* pubJa, int year, rnf_genre g) {
  rnf_game_info i{};
  i.id = en;
  i.title_en = en;
  i.title_ja = ja;
  i.reading = reading;
  i.publisher_en = pubEn;
  i.publisher_ja = pubJa;
  i.year = year;
  i.genre = g;
  i.region = "JP";
  return i;
}

std::vector<size_t> arrange(const std::vector<rnf_library_item>& items, rnf_library_sort sort, rnf_library_filter filter,
                            const char* query, const rnf_library_prefs* prefs) {
  size_t n = rnf_library_arrange(items.data(), items.size(), sort, filter, query, prefs, nullptr, 0);
  std::vector<size_t> out(n);
  rnf_library_arrange(items.data(), items.size(), sort, filter, query, prefs, out.data(), out.size());
  return out;
}

std::vector<std::string> titles(const std::vector<rnf_library_item>& items, const std::vector<size_t>& order) {
  std::vector<std::string> out;
  for (size_t i : order) out.push_back(take(rnf_game_title(items[i].game, items[i].file_name)));
  return out;
}

}  // namespace

// ------------------------------------------------------------------ collation

TEST_CASE("kana sort keys fold katakana, small kana, marks and long vowels") {
  CHECK_EQ(take(rnf_kana_sort_key("フロントライン")), std::string("ふろんとらいん"));
  CHECK_EQ(take(rnf_kana_sort_key("スーパー")), std::string("すうはあ"));
  CHECK_EQ(take(rnf_kana_sort_key("ガッツ")), std::string("かつつ"));
  CHECK_EQ(take(rnf_kana_sort_key("キャッスル")), std::string("きやつする"));
  CHECK_EQ(take(rnf_kana_sort_key("ヴォルガード")), std::string("うおるかあと"));
  CHECK_EQ(take(rnf_kana_sort_key("ﾌﾛﾝﾄﾗｲﾝ")), std::string("ふろんとらいん"));  // half-width
  CHECK_EQ(take(rnf_kana_sort_key("ドラゴンクエスト・Ⅱ")), std::string("とらこんくえすと2"));
}

TEST_CASE("Japanese collation: gojuon order with dakuten and long vowels") {
  // Dakuten / handakuten are folded at the first level: コナミ < ゴルフ (な < る).
  CHECK(cmpJa("コナミ", "ゴルフ") < 0);
  CHECK(cmpJa("ゴルフ", "コロコロ") < 0);
  // Ties: plain < voiced < semi-voiced.
  CHECK(cmpJa("はは", "ばば") < 0);
  CHECK(cmpJa("ばば", "ぱぱ") < 0);
  CHECK(cmpJa("かき", "がき") < 0);
  // Hiragana and katakana are the same letters (ties: hiragana first).
  CHECK(cmpJa("まりお", "マリオ") < 0);
  CHECK(cmpJa("マリオ", "まりおか") < 0);
  // The long vowel mark reads as the preceding vowel: スーパー = すうはあ, so before スパイ.
  CHECK(cmpJa("スーパーマリオブラザーズ", "スパイ") < 0);
  CHECK(cmpJa("スーパーマリオブラザーズ", "スーパーマリオブラザーズ3") < 0);
  CHECK(cmpJa("スウ", "スー") < 0);  // explicit vowel first
  // Small kana fold to their full size: きゃ = きや.
  CHECK(cmpJa("キャッスル", "キヤ") > 0);
  CHECK(cmpJa("キャ", "キヤ") > 0);  // small after full size on a tie
  // FRONTLINE read as フロントライン: between ファミスタ (はみすた) and ボンバーマン (ほんはあまん).
  CHECK(cmpJa("ファミスタ", "フロントライン") < 0);
  CHECK(cmpJa("フロントライン", "ボンバーマン") < 0);
  // Kana < digits < Latin < kanji.
  CHECK(cmpJa("ん", "1942") < 0);
  CHECK(cmpJa("1942", "Abadox") < 0);
  CHECK(cmpJa("Abadox", "魂斗羅") < 0);
  // Digit runs by value.
  CHECK(cmpJa("グラディウス2", "グラディウス10") < 0);
  // Spaces and punctuation are ignored.
  CHECK_EQ(cmpJa("ドラゴン・クエスト", "ドラゴンクエスト") != 0, true);  // raw tie-break only
  CHECK(cmpJa("ドラゴン・クエスト", "ドラゴンクエストII") < 0);
}

TEST_CASE("English collation ignores case and a leading The") {
  CHECK(cmpEn("abadox", "Balloon Fight") < 0);
  CHECK(cmpEn("The Legend of Zelda", "Mario Bros.") < 0);
  CHECK(cmpEn("Kid Icarus", "The Legend of Zelda") < 0);
  CHECK(cmpEn("Gradius 2", "Gradius 10") < 0);
  CHECK(cmpEn("1942", "Abadox") < 0);
  CHECK(cmpEn("Zanac", "グラディウス") < 0);
}

// ------------------------------------------------------------------ database

TEST_CASE("ROM data hashes skip the iNES header and trainer") {
  // SHA-1("abc") (FIPS 180-1 test vector) from a headerless image.
  char hex[41];
  const uint8_t abc[] = {'a', 'b', 'c'};
  rnf_rom_data_sha1(abc, 3, hex);
  CHECK_EQ(std::string(hex), std::string("A9993E364706816ABA3E25717850C26C9CD0D89D"));
  // A headered image hashes like its PRG+CHR body, with or without a trainer.
  std::vector<uint8_t> body(16384 + 8192);
  for (size_t i = 0; i < body.size(); ++i) body[i] = uint8_t(i * 7 + 3);
  std::vector<uint8_t> headered = {'N', 'E', 'S', 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  headered.insert(headered.end(), body.begin(), body.end());
  std::vector<uint8_t> trainer = headered;
  trainer[6] |= 0x04;
  trainer.insert(trainer.begin() + 16, 512, 0xEE);
  uint32_t crc = rnf_rom_data_crc32(body.data(), body.size());
  CHECK_EQ(rnf_rom_data_crc32(headered.data(), headered.size()), crc);
  CHECK_EQ(rnf_rom_data_crc32(trainer.data(), trainer.size()), crc);
  char a[41], b[41];
  rnf_rom_data_sha1(body.data(), body.size(), a);
  rnf_rom_data_sha1(trainer.data(), trainer.size(), b);
  CHECK_EQ(std::string(a), std::string(b));
}

TEST_CASE("database lookups by hash, id and file name") {
  REQUIRE(rnf_gamedb_count() > 500);
  rnf_game_info byCrc{}, bySha{}, byName{};
  // Super Mario Bros. (World): CRC32 / SHA-1 of PRG+CHR as in Nestopia's NstDatabase.xml.
  REQUIRE(rnf_gamedb_find_crc32(0xD445F698u, &byCrc));
  REQUIRE(rnf_gamedb_find_sha1("facee9c577a5262dbe33ac4930bb0b58c8c037f7", &bySha));
  CHECK_EQ(std::string(byCrc.id), std::string(bySha.id));
  CHECK_EQ(std::string(byCrc.title_ja), std::string("スーパーマリオブラザーズ"));
  CHECK_EQ(std::string(byCrc.reading), std::string("スーパーマリオブラザーズ"));
  CHECK_EQ(byCrc.year, 1985);
  CHECK_EQ(int(byCrc.genre), int(RNF_GENRE_ACTION));
  CHECK_EQ(std::string(byCrc.publisher_ja), std::string("任天堂"));
  REQUIRE(rnf_gamedb_find_name("Super Mario Bros. (World).nes", &byName));
  CHECK_EQ(std::string(byName.id), std::string(byCrc.id));
  rnf_game_info again{};
  REQUIRE(rnf_gamedb_find_id(byCrc.id, &again));
  CHECK_EQ(std::string(again.title_en), std::string(byCrc.title_en));
  // A Nestopia hash entry with a different SHA-1 is not this game.
  CHECK_FALSE(rnf_gamedb_find_sha1("0000000000000000000000000000000000000000", nullptr));

  // File names: region / revision / dump tags ignored, ", The" and subtitles handled.
  rnf_game_info g{};
  REQUIRE(rnf_gamedb_find_name("Gradius (Japan).NES", &g));
  CHECK_EQ(std::string(g.title_ja), std::string("グラディウス"));
  REQUIRE(rnf_gamedb_find_name("Abadox - The Deadly Inner War (USA).nes", &g));
  CHECK_EQ(std::string(g.reading), std::string("アバドックス"));
  REQUIRE(rnf_gamedb_find_name("Tetris (Bulletproof) (Japan) (Rev A).nes", &g));
  CHECK_EQ(std::string(g.publisher_ja).empty(), false);
  CHECK_FALSE(rnf_gamedb_find_name("My Homebrew Thing (PD).nes", nullptr));
  // Romanized long vowels: "Akumajou" (No-Intro) = "Akumaj\u014D" (macron).
  rnf_game_info a{}, b{};
  REQUIRE(rnf_gamedb_find_name("Akumajou Dracula (Japan).nes", &a));
  CHECK_EQ(std::string(a.reading), std::string("アクマジョウドラキュラ"));
  if (rnf_gamedb_find_name("Akumaj\u014D Dracula", &b)) CHECK_EQ(std::string(a.id), std::string(b.id));
}

TEST_CASE("FRONT LINE reads フロントライン") {
  rnf_game_info g{};
  REQUIRE(rnf_gamedb_find_name("Front Line (Japan).nes", &g));
  CHECK_EQ(std::string(g.reading), std::string("フロントライン"));
}

TEST_CASE("display strings follow the language") {
  rnf_game_info g = game("Gradius", "グラディウス", "グラディウス", "Konami", "コナミ", 1986, RNF_GENRE_SHOOTER);
  {
    LangScope ja("ja");
    CHECK_EQ(take(rnf_game_title(&g, "Gradius (Japan).nes")), std::string("グラディウス"));
    CHECK_EQ(take(rnf_game_byline(&g)), std::string("コナミ・1986"));
    CHECK_EQ(take(rnf_game_details(&g)), std::string("コナミ・1986・シューティング"));
  }
  {
    LangScope en("en");
    CHECK_EQ(take(rnf_game_title(&g, "x.nes")), std::string("Gradius"));
    CHECK_EQ(take(rnf_game_byline(&g)), std::string("Konami · 1986"));
    CHECK_EQ(take(rnf_game_title(nullptr, "Legend of Zelda, The (USA) [!].nes")), std::string("The Legend of Zelda"));
    CHECK_EQ(take(rnf_game_title(nullptr, "Sub/Tetris (Bulletproof) (Japan) (Rev A).NES")), std::string("Tetris"));
    CHECK_EQ(take(rnf_game_byline(nullptr)), std::string(""));
  }
  CHECK_EQ(int(rnf_genre_from_code("puzzle")), int(RNF_GENRE_PUZZLE));
  CHECK_EQ(std::string(rnf_genre_code(RNF_GENRE_RPG)), std::string("rpg"));
  CHECK_EQ(int(rnf_genre_from_code("utility")), int(RNF_GENRE_UTILITY));
}

// The hash key in frontend/data/nesdb-overrides.json must follow the cartridge (engine/src/testrom):
// rebuild nesdb.tsv with the new CRC32 / SHA-1 whenever tools/testcart changes its image.
TEST_CASE("the ReplayNES Test Cartridge is in the database by its hash") {
  const std::string path = (std::filesystem::temp_directory_path() / "rn-gamedb-testcart.nes").string();
  REQUIRE(rn_write_test_cartridge(path.c_str()) == RN_OK);
  rnf_game_info g{};
  CHECK_EQ(int(rnf_gamedb_identify_file(path.c_str(), &g)), int(RNF_GAME_MATCH_HASH));  // not by its name: "x.nes" below
  std::remove(path.c_str());
  std::ifstream probe(path);
  CHECK_FALSE(probe.good());
  REQUIRE(g.id != nullptr);
  CHECK_EQ(std::string(g.id), std::string(RNF_TEST_CARTRIDGE_GAME_ID));
  CHECK_EQ(int(g.genre), int(RNF_GENRE_UTILITY));
  CHECK_EQ(g.year, 2026);
  {
    LangScope ja("ja");
    CHECK_EQ(take(rnf_game_title(&g, "x.nes")), std::string("ReplayNES テストカートリッジ"));
    CHECK_EQ(take(rnf_game_details(&g)), std::string("ReplayNES・2026・ツール"));
  }
  {
    LangScope en("en");
    CHECK_EQ(take(rnf_game_title(&g, "x.nes")), std::string("ReplayNES Test Cartridge"));
    CHECK_EQ(take(rnf_game_details(&g)), std::string("ReplayNES · 2026 · Utility"));
  }
  // Also by the file name "Add Test Cartridge" gives it.
  rnf_game_info n{};
  REQUIRE(rnf_gamedb_find_name(RNF_TEST_CARTRIDGE_FILE, &n));
  CHECK_EQ(std::string(n.id), std::string(RNF_TEST_CARTRIDGE_GAME_ID));
}

// ------------------------------------------------------------------ catalog

namespace {
struct Fixture {
  rnf_game_info smb = game("Super Mario Bros.", "スーパーマリオブラザーズ", "スーパーマリオブラザーズ", "Nintendo", "任天堂", 1985, RNF_GENRE_ACTION);
  rnf_game_info front = game("Front Line", "FRONTLINE", "フロントライン", "Taito", "タイトー", 1985, RNF_GENRE_SHOOTER);
  rnf_game_info famista = game("Pro Yakyuu Family Stadium", "プロ野球ファミリースタジアム", "ファミスタ", "Namco", "ナムコ", 1986, RNF_GENRE_SPORTS);
  rnf_game_info gradius = game("Gradius", "グラディウス", "グラディウス", "Konami", "コナミ", 1986, RNF_GENRE_SHOOTER);
  rnf_game_info zelda = game("The Legend of Zelda", "ゼルダの伝説", "ゼルダノデンセツ", "Nintendo", "任天堂", 1986, RNF_GENRE_ADVENTURE);
  rnf_game_info bomber = game("Bomberman", "ボンバーマン", "", "Hudson Soft", "ハドソン", 1985, RNF_GENRE_ACTION);  // reading from the kana title
  std::vector<rnf_library_item> items;
  Fixture() {
    items = {{"k-smb", "Super Mario Bros. (World).nes", "Super Mario Bros. (World).nes", &smb},
             {"k-front", "Front Line (Japan).nes", "Front Line (Japan).nes", &front},
             {"k-homebrew", "zz homebrew.nes", "zz homebrew.nes", nullptr},
             {"k-famista", "Famista.nes", "Famista.nes", &famista},
             {"k-gradius", "Gradius (Japan).nes", "Gradius (Japan).nes", &gradius},
             {"k-zelda", "Zelda.nes", "Zelda.nes", &zelda},
             {"k-bomber", "Bomberman.nes", "Bomberman.nes", &bomber},
             {"", "Another Homebrew.nes", "Another Homebrew.nes", nullptr}};
  }
};
}  // namespace

TEST_CASE("library order: Japanese by reading, English by title, unknown ROMs after") {
  Fixture f;
  {
    LangScope ja("ja");
    auto t = titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_NAME, RNF_LIBRARY_FILTER_ALL, nullptr, nullptr));
    std::vector<std::string> want = {"グラディウス", "スーパーマリオブラザーズ", "ゼルダの伝説", "プロ野球ファミリースタジアム",
                                     "FRONTLINE",   "ボンバーマン",             "Another Homebrew", "zz homebrew"};
    CHECK(t == want);
  }
  {
    LangScope en("en");
    auto t = titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_NAME, RNF_LIBRARY_FILTER_ALL, nullptr, nullptr));
    std::vector<std::string> want = {"Another Homebrew", "Bomberman", "Front Line", "Gradius", "The Legend of Zelda",
                                     "Pro Yakyuu Family Stadium", "Super Mario Bros.", "zz homebrew"};
    CHECK(t == want);
  }
}

TEST_CASE("library sorts by maker, year and genre with unknowns last") {
  Fixture f;
  LangScope en("en");
  auto year = titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_YEAR, RNF_LIBRARY_FILTER_ALL, nullptr, nullptr));
  CHECK_EQ(year.front(), std::string("Bomberman"));  // 1985, by title
  CHECK_EQ(year[3], std::string("Gradius"));          // 1986
  CHECK_EQ(year.back(), std::string("zz homebrew"));
  auto maker = titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_PUBLISHER, RNF_LIBRARY_FILTER_ALL, nullptr, nullptr));
  CHECK_EQ(maker.front(), std::string("Bomberman"));  // Hudson Soft
  CHECK_EQ(maker[1], std::string("Gradius"));         // Konami
  CHECK_EQ(maker[6], std::string("Another Homebrew"));
  auto genre = titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_GENRE, RNF_LIBRARY_FILTER_ALL, nullptr, nullptr));
  CHECK_EQ(genre.front(), std::string("Bomberman"));  // action
  CHECK_EQ(genre[1], std::string("Super Mario Bros."));
  CHECK_EQ(genre.back(), std::string("zz homebrew"));
}

TEST_CASE("favourites, history, filters and search; persisted as JSON") {
  Fixture f;
  LangScope ja("ja");
  rnf_library_prefs* p = rnf_library_prefs_new();
  CHECK_EQ(rnf_library_prefs_toggle_favorite(p, "k-zelda"), 1);
  CHECK_EQ(rnf_library_prefs_toggle_favorite(p, "k-front"), 1);
  CHECK_EQ(rnf_library_prefs_toggle_favorite(p, "k-front"), 0);
  CHECK_EQ(rnf_library_prefs_toggle_favorite(p, ""), 0);  // unknown hash: never a favourite
  CHECK_EQ(rnf_library_prefs_toggle_favorite(p, "k-gradius"), 1);
  auto fav = titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_NAME, RNF_LIBRARY_FILTER_FAVORITES, nullptr, p));
  CHECK(fav == std::vector<std::string>({"グラディウス", "ゼルダの伝説"}));

  rnf_library_prefs_record_play(p, "k-gradius", 1000, 0);
  rnf_library_prefs_record_play(p, "k-gradius", 1000, 60);  // same session continues
  rnf_library_prefs_record_play(p, "k-smb", 2000, 30);
  rnf_library_prefs_record_play(p, "k-front", 3000, 5);
  rnf_library_prefs_record_play(p, "k-gradius", 4000, 10);  // a new session
  rnf_library_history_entry h{};
  REQUIRE(rnf_library_prefs_history_find(p, "k-gradius", &h));
  CHECK_EQ(h.plays, int64_t(2));
  CHECK(near(h.play_seconds, 70, 1e-9));
  CHECK(near(h.last_played, 4000, 1e-9));
  auto recent = titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_NAME, RNF_LIBRARY_FILTER_RECENT, nullptr, p));
  CHECK(recent == std::vector<std::string>({"グラディウス", "FRONTLINE", "スーパーマリオブラザーズ"}));
  auto last = titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_RECENT, RNF_LIBRARY_FILTER_ALL, nullptr, p));
  CHECK_EQ(last[0], std::string("グラディウス"));
  CHECK_EQ(last[3], std::string("ゼルダの伝説"));  // never played: by name

  // Search: kana-insensitive, either language, the reading, the file name.
  CHECK(titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_NAME, RNF_LIBRARY_FILTER_ALL, "まりお", p)) ==
        std::vector<std::string>({"スーパーマリオブラザーズ"}));
  CHECK(titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_NAME, RNF_LIBRARY_FILTER_ALL, "ふろんと", p)) ==
        std::vector<std::string>({"FRONTLINE"}));
  CHECK(titles(f.items, arrange(f.items, RNF_LIBRARY_SORT_NAME, RNF_LIBRARY_FILTER_ALL, "ＺＥＬＤＡ", p)) ==
        std::vector<std::string>({"ゼルダの伝説"}));
  CHECK_EQ(arrange(f.items, RNF_LIBRARY_SORT_NAME, RNF_LIBRARY_FILTER_ALL, "homebrew", p).size(), size_t(2));

  rnf_library_prefs_set_sort(p, RNF_LIBRARY_SORT_YEAR);
  rnf_library_prefs_set_filter(p, RNF_LIBRARY_FILTER_FAVORITES);
  std::string dir = rntest::tempDir("frontend-gamedb");
  std::string path = dir + "/settings/" + RNF_LIBRARY_PREFS_FILE;
  REQUIRE_EQ(rnf_library_prefs_save(p, path.c_str()), RN_OK);
  rnf_library_prefs* q = rnf_library_prefs_new();
  REQUIRE_EQ(rnf_library_prefs_load(q, path.c_str()), RN_OK);
  CHECK(rnf_library_prefs_is_favorite(q, "k-zelda"));
  CHECK(rnf_library_prefs_is_favorite(q, "k-gradius"));
  CHECK_FALSE(rnf_library_prefs_is_favorite(q, "k-front"));
  CHECK_EQ(int(rnf_library_prefs_sort(q)), int(RNF_LIBRARY_SORT_YEAR));
  CHECK_EQ(int(rnf_library_prefs_filter(q)), int(RNF_LIBRARY_FILTER_FAVORITES));
  REQUIRE_EQ(rnf_library_prefs_history_count(q), size_t(3));
  REQUIRE(rnf_library_prefs_history_get(q, 0, &h));
  CHECK_EQ(std::string(h.key), std::string("k-gradius"));
  CHECK(near(h.play_seconds, 70, 1e-9));
  rnf_library_prefs_forget(q, "k-gradius");
  CHECK_EQ(rnf_library_prefs_history_count(q), size_t(2));

  // Missing file: empty prefs; damaged file: RN_ERR_CORRUPT and empty prefs.
  REQUIRE_EQ(rnf_library_prefs_load(q, (dir + "/missing.json").c_str()), RN_OK);
  CHECK_EQ(rnf_library_prefs_history_count(q), size_t(0));
  std::ofstream(dir + "/bad.json") << "{ not json";
  CHECK_EQ(rnf_library_prefs_load(q, (dir + "/bad.json").c_str()), RN_ERR_CORRUPT);
  CHECK_FALSE(rnf_library_prefs_is_favorite(q, "k-zelda"));
  rnf_library_prefs_free(q);
  rnf_library_prefs_free(p);
}

// The developer's own ROMs (gitignored <repo>/roms, or RN_LOCAL_ROMS_DIR): each one is recognised by
// its hash with a Japanese title and reading. Skipped when there are none.
TEST_CASE("local ROMs are identified by hash") {
  std::string dir = std::getenv("RN_LOCAL_ROMS_DIR") ? std::getenv("RN_LOCAL_ROMS_DIR") : RN_LOCAL_ROMS;
  std::error_code ec;
  int seen = 0;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::string p = it->path().string(), ext = it->path().extension().string();
    if (ext != ".nes" && ext != ".NES") continue;
    rnf_game_info g{};
    rnf_game_match m = rnf_gamedb_identify_file(p.c_str(), &g);
    MESSAGE(it->path().filename().string() + " -> " + (m ? std::string(g.title_ja) + " / " + g.reading : std::string("(none)")));
    CHECK_EQ(int(m), int(RNF_GAME_MATCH_HASH));
    if (m) {
      CHECK(*g.reading);
      CHECK(g.year > 0);
    }
    ++seen;
  }
  if (!seen) MESSAGE("no local ROMs in " + dir + "; skipped");
}
