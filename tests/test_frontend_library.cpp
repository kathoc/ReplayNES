// Frontend core: ROM library folders, scanning, SHA-256 matching of projects, naming, and the
// "Reset Project" backup name. Ported from the macOS LibraryTests / ProjectResetTests.
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

void writeBytes(const std::string& path, const std::string& bytes) { std::ofstream(path, std::ios::binary) << bytes; }

std::vector<rnf_rom_entry> roms(rnf_rom_list* l) {
  std::vector<rnf_rom_entry> v(rnf_rom_list_count(l));
  for (size_t i = 0; i < v.size(); ++i) rnf_rom_list_get(l, i, &v[i]);
  return v;
}

double localDate(int y, int mo, int d, int h, int mi) {
  struct tm t{};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_isdst = -1;
  return double(std::mktime(&t));
}

}  // namespace

TEST_CASE("ensure creates the folders and reports conflicts") {
  std::string tmp = rntest::tempDir("frontend-lib");
  std::string root = rn::fs::join(tmp, "ReplayNES");
  REQUIRE_EQ(rnf_library_ensure(root.c_str(), nullptr), RN_OK);
  CHECK(rn::fs::isDir(rn::fs::join(root, RNF_LIBRARY_ROM_DIR)));
  CHECK(rn::fs::isDir(rn::fs::join(root, RNF_LIBRARY_PROJECTS_DIR)));
  CHECK_EQ(rnf_library_ensure(root.c_str(), nullptr), RN_OK);
  std::string bad = rn::fs::join(tmp, "Bad");
  rn::fs::createDirs(bad);
  writeBytes(rn::fs::join(bad, "ROM"), "");
  char* failed = nullptr;
  CHECK_EQ(rnf_library_ensure(bad.c_str(), &failed), RN_ERR_ALREADY_EXISTS);
  CHECK_EQ(take(failed), rn::fs::join(bad, "ROM"));
}

TEST_CASE("scan finds .nes files one level deep, sorted by name") {
  std::string tmp = rntest::tempDir("frontend-lib");
  REQUIRE_EQ(rnf_library_ensure(tmp.c_str(), nullptr), RN_OK);
  std::string romDir = rn::fs::join(tmp, "ROM");
  rn::fs::createDirs(rn::fs::join(romDir, "Sub/Deeper"));
  for (auto name : {"b game.NES", "A Game.nes", "Sub/c game.Nes", "Sub/Deeper/too deep.nes", "readme.txt",
                    "notes.nes.txt", "game10.nes", "game9.nes", ".hidden.nes"})
    writeBytes(rn::fs::join(romDir, name), "NES\x1a");
  rnf_rom_list* l = nullptr;
  REQUIRE_EQ(rnf_library_scan_roms(romDir.c_str(), nullptr, nullptr, &l), RN_OK);
  auto r = roms(l);
  std::vector<std::string> names;
  for (auto& e : r) names.push_back(e.name);
  CHECK((names == std::vector<std::string>{"A Game", "b game", "c game", "game9", "game10"}));
  for (auto& e : r)
    if (std::string(e.name) == "c game") CHECK_EQ(std::string(e.relative_path), "Sub/c game.Nes");
  CHECK_EQ(r[0].size, int64_t(4));
  CHECK(r[0].modified > 1.5e9);
  rnf_rom_list_free(l);
  CHECK_EQ(rnf_library_scan_roms(rn::fs::join(tmp, "missing").c_str(), nullptr, nullptr, &l), RN_ERR_IO);
  CHECK(l == nullptr);

  // A frontend-supplied collation (here: reverse byte order) decides the order.
  REQUIRE_EQ(rnf_library_scan_roms(romDir.c_str(), [](const char* a, const char* b, void*) { return -std::strcmp(a, b); },
                                   nullptr, &l), RN_OK);
  CHECK_EQ(std::string(roms(l)[0].name), "game9");
  rnf_rom_list_free(l);
}

TEST_CASE("projects are matched by SHA-256, not by name") {
  std::string tmp = rntest::tempDir("frontend-lib");
  REQUIRE_EQ(rnf_library_ensure(tmp.c_str(), nullptr), RN_OK);
  std::string romDir = rn::fs::join(tmp, "ROM"), projDir = rn::fs::join(tmp, "Projects");
  std::string rom = rntest::writeTestRom(romDir, "Test ROM.nes");
  const double date = 1790000000;
  std::string stamp = take(rnf_library_timestamp(date));
  CHECK_EQ(stamp.size(), size_t(15));
  std::string dir = take(rnf_library_new_project_path(projDir.c_str(), "Test ROM", date));
  CHECK_EQ(dir, rn::fs::join(projDir, "Test ROM " + stamp + ".nesrec"));
  {
    rn_session_options o;
    rn_session_options_init(&o);
    rn_session* s = nullptr;
    REQUIRE_EQ(rn_session_new(rom.c_str(), dir.c_str(), &o, &s), RN_OK);
    rn_set_mode(s, RN_MODE_RECORD);
    for (int i = 0; i < 30; ++i) rn_step(s, 0, 0, 0, nullptr);
    REQUIRE_EQ(rn_session_save(s), RN_OK);
    rn_session_close(s);
  }
  std::string dir2 = take(rnf_library_new_project_path(projDir.c_str(), "Test ROM", date));
  CHECK_EQ(dir2, rn::fs::join(projDir, "Test ROM " + stamp + " 2.nesrec"));

  std::string renamed = rn::fs::join(romDir, "Renamed.nes");
  REQUIRE(std::rename(rom.c_str(), renamed.c_str()) == 0);
  std::vector<uint8_t> bytes;
  rn::fs::readFile(renamed, bytes);
  bytes.back() ^= 0xFF;
  rn::fs::writeFileAtomic(rn::fs::join(romDir, "Other.nes"), bytes.data(), bytes.size());
  writeBytes(rn::fs::join(projDir, "not a project.txt"), "junk");

  rnf_project_list* p = nullptr;
  REQUIRE_EQ(rnf_library_scan_projects(projDir.c_str(), &p), RN_OK);
  REQUIRE_EQ(rnf_project_list_count(p), size_t(1));
  rnf_project_entry pe;
  rnf_project_list_get(p, 0, &pe);
  CHECK_EQ(std::string(pe.name), "Test ROM " + stamp);
  CHECK_EQ(std::string(pe.rom_name), "Test ROM.nes");
  std::string sha = pe.rom_sha256;

  rnf_rom_hash_cache* cache = rnf_rom_hash_cache_new();
  rnf_rom_list* l = nullptr;
  REQUIRE_EQ(rnf_library_scan_roms(romDir.c_str(), nullptr, nullptr, &l), RN_OK);
  std::map<std::string, std::string> byName;
  for (auto& e : roms(l)) {
    char hex[65];
    REQUIRE_EQ(rnf_rom_hash_cache_sha256(cache, e.path, e.size, e.modified, hex), RN_OK);
    byName[e.name] = hex;
    char again[65];
    rnf_rom_hash_cache_sha256(cache, e.path, e.size, e.modified, again);
    CHECK_EQ(std::string(again), std::string(hex));
  }
  CHECK_EQ(byName["Renamed"], sha);
  CHECK(byName["Other"] != sha);
  rnf_rom_list_free(l);
  rnf_project_list_free(p);
  rnf_rom_hash_cache_free(cache);
}

TEST_CASE("sanitize") {
  CHECK_EQ(take(rnf_library_sanitize("Mario/Luigi: Deluxe")), "Mario_Luigi_ Deluxe");
  CHECK_EQ(take(rnf_library_sanitize("  ")), "ROM");
  CHECK_EQ(take(rnf_library_sanitize(".hidden")), "_hidden");
  CHECK_EQ(take(rnf_library_sanitize("\xE3\x80\x80Zelda\tII\xC2\xA0")), "Zelda_II");  // ideographic / no-break spaces trimmed, tab is Cc
  CHECK_EQ(take(rnf_library_sanitize("a\x01" "b\xE2\x80\x8B" "c")), "a_b_c");          // Cc and Cf (zero-width space)
  CHECK_EQ(take(rnf_library_sanitize("日本語")), "日本語");
}

TEST_CASE("backup name next to the project") {
  const double d = localDate(2026, 10, 6, 20, 5);
  std::string first = take(rnf_backup_path("/x/Games/Mario.nesrec", d, [](const char*, void*) { return 0; }, nullptr));
  CHECK_EQ(first, "/x/Games/Mario (Before Reset 2026-10-06 20.05).nesrec");
  std::string taken = take(rnf_backup_path(
      "/x/Games/Mario.nesrec", d,
      [](const char* p, void* ctx) { return int(std::string(p) == *static_cast<std::string*>(ctx)); }, &first));
  CHECK_EQ(taken, "/x/Games/Mario (Before Reset 2026-10-06 20.05) 2.nesrec");
  CHECK_EQ(take(rnf_backup_path("/x/Games/NoExt/", d, [](const char*, void*) { return 0; }, nullptr)),
           "/x/Games/NoExt (Before Reset 2026-10-06 20.05).nesrec");
  rnf_l10n_set_language("ja");
  CHECK_EQ(take(rnf_backup_path("/x/Mario.nesrec", d, [](const char*, void*) { return 0; }, nullptr)),
           "/x/Mario（リセット前 2026-10-06 20.05）.nesrec");
  rnf_l10n_set_language("en");
}

// UTF-8 paths reach the file system intact (Windows: wide APIs, not the ANSI code page), and a
// file's modification time is reported identically on every scan (it keys the ROM hash cache).
TEST_CASE("non-ASCII folder and file names") {
  std::string tmp = rntest::tempDir("frontend-lib-utf8");
  std::string root = rn::fs::join(tmp, "リプレイ Äö");
  REQUIRE_EQ(rnf_library_ensure(root.c_str(), nullptr), RN_OK);
  REQUIRE_EQ(rnf_library_ensure(root.c_str(), nullptr), RN_OK);
  std::string romDir = rn::fs::join(root, RNF_LIBRARY_ROM_DIR);
  std::string sub = rn::fs::join(romDir, "ゲーム");
  REQUIRE(rn::fs::createDirs(sub).ok());
  REQUIRE(rn::fs::writeFileAtomic(rn::fs::join(sub, "マリオ.nes"), "NES\x1a", 4).ok());
  double modified[2] = {0, 0};
  for (double& m : modified) {
    rnf_rom_list* l = nullptr;
    REQUIRE_EQ(rnf_library_scan_roms(romDir.c_str(), nullptr, nullptr, &l), RN_OK);
    auto r = roms(l);
    REQUIRE_EQ(r.size(), size_t(1));
    CHECK_EQ(std::string(r[0].name), "マリオ");
    CHECK_EQ(std::string(r[0].relative_path), "ゲーム/マリオ.nes");
    CHECK_EQ(std::string(r[0].path), rn::fs::join(sub, "マリオ.nes"));
    CHECK_EQ(r[0].size, int64_t(4));
    m = r[0].modified;
    rnf_rom_list_free(l);
  }
  CHECK(modified[0] > 1.5e9);
  CHECK_EQ(modified[0], modified[1]);
}

#ifdef _WIN32
TEST_CASE("backup name keeps Windows separators") {
  const double d = localDate(2026, 10, 6, 20, 5);
  auto none = [](const char*, void*) { return 0; };
  CHECK_EQ(take(rnf_backup_path("C:\\x\\Mario.nesrec", d, none, nullptr)),
           "C:\\x\\Mario (Before Reset 2026-10-06 20.05).nesrec");
  CHECK_EQ(take(rnf_backup_path("C:\\Mario.nesrec", d, none, nullptr)), "C:\\Mario (Before Reset 2026-10-06 20.05).nesrec");
  CHECK_EQ(take(rnf_backup_path("C:\\x\\NoExt\\", d, none, nullptr)), "C:\\x\\NoExt (Before Reset 2026-10-06 20.05).nesrec");
}
#endif
