// "Add to Steam" (src/steam_shortcut.*): binary shortcuts.vdf round trip, append / update /
// idempotence with untouched entries kept byte for byte, the shortcut app id, grid artwork names,
// the Steam-running guard and the failure paths. Fixtures are synthetic shortcuts.vdf files built
// byte by byte here (independently of the serializer under test).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "steam_shortcut.h"
#include "support/rn_test.h"

namespace fs = std::filesystem;

namespace {

std::string tmpDir(const std::string& name) {
  std::string d = std::string(RNL_TEST_TMP) + "/steam-" + name + "-" + std::to_string(::getpid());
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void writeFile(const std::string& p, const std::string& data) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream f(p, std::ios::binary);
  f << data;
}

// --- hand-rolled binary VDF -----------------------------------------------------------------
std::string S(const std::string& k, const std::string& v) { return std::string(1, '\x01') + k + '\0' + v + '\0'; }
std::string I(const std::string& k, uint32_t v) {
  std::string out = std::string(1, '\x02') + k + '\0';
  for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xFF));
  return out;
}
std::string U64(const std::string& k, uint64_t v) {
  std::string out = std::string(1, '\x07') + k + '\0';
  for (int i = 0; i < 8; ++i) out.push_back(char((v >> (8 * i)) & 0xFF));
  return out;
}
std::string M(const std::string& k, const std::string& body) { return std::string(1, '\0') + k + '\0' + body + '\x08'; }

/// A Steam-written entry (modern field order).
std::string entry(const std::string& key, uint32_t appid, const std::string& name, const std::string& exe,
                  const std::string& launch, const std::string& icon, uint32_t lastPlay, const std::string& tags) {
  return M(key, I("appid", appid) + S("AppName", name) + S("Exe", exe) + S("StartDir", "/usr/bin/") + S("icon", icon) +
                    S("ShortcutPath", "") + S("LaunchOptions", launch) + I("IsHidden", 0) +
                    I("AllowDesktopConfig", 1) + I("AllowOverlay", 1) + I("OpenVR", 0) + I("Devkit", 0) +
                    S("DevkitGameID", "") + I("DevkitOverrideAppID", 0) + I("LastPlayTime", lastPlay) +
                    S("FlatpakAppID", "") + S("sortas", "") + M("tags", tags));
}

std::string fileOf(const std::string& entries) { return M("shortcuts", entries) + '\x08'; }

std::string otherEntries() {
  return entry("0", 4148668625u, "Browser", "\"/usr/bin/flatpak\"",
               "\"run\" \"--branch=stable\" \"org.example.Browser\" \"@@u\" \"@@\"",
               "/home/u/.local/share/Steam/userdata/1/config/grid/4148668625_icon.ico", 1759000000u,
               S("0", "favorite") + S("1", "Tools")) +
         // older Steam: lowercase keys, no appid, a 64-bit field, UTF-8 name, empty tags
         M("1", S("appname", "Ëmülätör ✓") + S("exe", "\"/opt/emu/run.sh\"") + S("StartDir", "\"/opt/emu/\"") +
                    S("icon", "") + U64("LastPlayTime64", 0x0123456789ABCDEFull) + M("tags", "")) +
         entry("2", 3700377527u, "Konsole", "\"konsole\"", "", "", 0, "");
}

std::string fakePng(const std::string& tag) { return "\x89PNG\r\n\x1a\n" + tag; }

std::string makeArtwork(const std::string& dir) {
  std::string a = dir + "/artwork";
  for (const char* n : {"capsule", "wide", "hero", "logo", "icon"}) writeFile(a + "/" + n + ".png", fakePng(n));
  return a;
}

struct Env {
  std::string root, userdata, user, vdf, grid, art;
  explicit Env(const std::string& name, const std::string& accounts = "12345") {
    root = tmpDir(name);
    userdata = root + "/Steam/userdata";
    user = userdata + "/" + accounts;
    vdf = user + "/config/shortcuts.vdf";
    grid = user + "/config/grid";
    art = makeArtwork(root);
    fs::create_directories(user + "/config");
  }
  steam::AddOptions opts() const {
    steam::AddOptions o;
    o.userdataRoots = {userdata};
    o.artworkDir = art;
    o.home = root + "/home";
    return o;
  }
};

const steam::vdf::Node* findEntryNamed(const steam::ShortcutsFile& f, const std::string& name) {
  for (const auto& e : f.entries) {
    const auto* n = e.node.find("AppName");
    if (n && n->str == name) return &e.node;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("steam: crc32 and shortcut app ids") {
  CHECK_EQ(steam::crc32("123456789"), 0xCBF43926u);  // the CRC-32 (IEEE) check value
  CHECK_EQ(steam::crc32(""), 0u);
  // crc32(Exe + AppName) | 0x80000000, as computed by zlib.crc32 / Steam ROM Manager / BoilR.
  CHECK_EQ(steam::shortcutAppId("\"/usr/bin/flatpak\"", "ReplayNES"), 2746019953u);
  CHECK_EQ(steam::shortcutAppId("\"C:\\Games\\game.exe\"", "My Game"), 3017974625u);
  CHECK_EQ(steam::shortcutAppId("", ""), 0x80000000u);
  CHECK_EQ(steam::shortcutGameId64(2746019953u), 11794065892332011520ull);
}

TEST_CASE("steam: shortcuts.vdf parses and serialises byte for byte") {
  std::string bytes = fileOf(otherEntries());
  steam::ShortcutsFile f;
  std::string err;
  REQUIRE(steam::ShortcutsFile::parse(bytes, &f, &err));
  REQUIRE_EQ(f.entries.size(), size_t(3));
  CHECK(f.serialize() == bytes);
  CHECK_EQ(f.nextKey(), std::string("3"));
  // the parsed nodes re-serialise identically too (an updated entry keeps unknown fields)
  for (auto& e : f.entries) e.dirty = true;
  CHECK(f.serialize() == bytes);
  const auto* legacy = findEntryNamed(f, "Ëmülätör ✓");
  REQUIRE(legacy != nullptr);
  CHECK_EQ(legacy->find("LastPlayTime64")->raw.size(), size_t(8));

  steam::ShortcutsFile empty;
  REQUIRE(steam::ShortcutsFile::parse("", &empty, &err));
  CHECK(empty.serialize() == std::string("\0shortcuts\0\x08\x08", 13));
  CHECK_EQ(empty.nextKey(), std::string("0"));

  CHECK_FALSE(steam::ShortcutsFile::parse(bytes.substr(0, bytes.size() - 5), &f, &err));
  CHECK_FALSE(steam::ShortcutsFile::parse(M("other", "") + '\x08', &f, &err));
  CHECK_FALSE(steam::ShortcutsFile::parse(std::string("\x05weird\0", 7), &f, &err));
}

TEST_CASE("steam: appends a new entry, keeps the others, backs up, installs artwork") {
  Env env("append");
  std::string original = fileOf(otherEntries());
  writeFile(env.vdf, original);
  ::chmod(env.vdf.c_str(), 0755);

  steam::Report r;
  REQUIRE(steam::addToSteam(env.opts(), r));
  REQUIRE_EQ(r.users.size(), size_t(1));
  const auto& u = r.users[0];
  CHECK(u.action == steam::UserAction::Added);
  CHECK_EQ(u.accountId, std::string("12345"));
  CHECK_EQ(u.appId, 2746019953u);
  CHECK_EQ(u.entryKey, std::string("3"));
  CHECK(u.shortcutsWritten);

  std::string now = readFile(env.vdf);
  // old head + entries byte-identical, the new entry in front of the list terminator
  std::string oldBody = original.substr(0, original.size() - 2);
  REQUIRE(now.size() > original.size());
  CHECK(now.compare(0, oldBody.size(), oldBody) == 0);
  CHECK(now.substr(now.size() - 2) == std::string("\x08\x08", 2));
  CHECK(readFile(env.vdf + ".replaynes-backup") == original);
  CHECK_EQ(fs::status(env.vdf).permissions() & fs::perms::all, fs::perms(0755));

  steam::ShortcutsFile f;
  std::string err;
  REQUIRE(steam::ShortcutsFile::parse(now, &f, &err));
  REQUIRE_EQ(f.entries.size(), size_t(4));
  const auto* ours = findEntryNamed(f, "ReplayNES");
  REQUIRE(ours != nullptr);
  CHECK_EQ(ours->find("appid")->u32(), 2746019953u);
  CHECK_EQ(ours->find("Exe")->str, std::string("\"/usr/bin/flatpak\""));
  CHECK_EQ(ours->find("LaunchOptions")->str, std::string("run io.github.replaynes.ReplayNES"));
  CHECK_EQ(ours->find("icon")->str, env.grid + "/2746019953_icon.png");
  CHECK_EQ(ours->find("tags")->find("0")->str, std::string("Emulator"));

  // grid names: <id>p capsule, <id> wide, _hero, _logo, _icon (+ logo position json)
  CHECK(readFile(env.grid + "/2746019953p.png") == fakePng("capsule"));
  CHECK(readFile(env.grid + "/2746019953.png") == fakePng("wide"));
  CHECK(readFile(env.grid + "/2746019953_hero.png") == fakePng("hero"));
  CHECK(readFile(env.grid + "/2746019953_logo.png") == fakePng("logo"));
  CHECK(readFile(env.grid + "/2746019953_icon.png") == fakePng("icon"));
  CHECK(readFile(env.grid + "/2746019953.json").find("BottomLeft") != std::string::npos);
  CHECK(!fs::exists(env.vdf + ".replaynes-tmp"));

  // idempotent: a second run changes nothing
  std::string afterFirst = readFile(env.vdf);
  steam::Report r2;
  REQUIRE(steam::addToSteam(env.opts(), r2));
  CHECK(r2.users[0].action == steam::UserAction::Unchanged);
  CHECK(r2.users[0].existingEntry);
  CHECK_FALSE(r2.users[0].shortcutsWritten);
  CHECK(r2.users[0].artworkWritten.empty());
  CHECK(readFile(env.vdf) == afterFirst);
  CHECK(readFile(env.vdf + ".replaynes-backup") == original);
}

TEST_CASE("steam: updates the entry the user added through Steam in place") {
  Env env("update");
  std::string mine = entry("3", 2957107289u, "ReplayNES", "\"/usr/bin/flatpak\"",
                           "\"run\" \"--branch=master\" \"--arch=x86_64\" \"--command=replaynes-linux\" "
                           "\"io.github.replaynes.ReplayNES\"",
                           "", 1781000000u, "");
  std::string original = fileOf(otherEntries() + mine);
  writeFile(env.vdf, original);
  // the user's own hero art for this id stays (as a backup); a user JPEG capsule wins
  writeFile(env.grid + "/2957107289_hero.png", fakePng("user hero"));
  writeFile(env.grid + "/2957107289p.jpg", "user jpeg");
  writeFile(env.grid + "/2957107289.json", "{\"custom\":1}");

  steam::Report r;
  REQUIRE(steam::addToSteam(env.opts(), r));
  const auto& u = r.users[0];
  CHECK(u.action == steam::UserAction::Updated);
  CHECK(u.existingEntry);
  CHECK_EQ(u.appId, 2957107289u);
  CHECK_EQ(u.entryKey, std::string("3"));

  std::string now = readFile(env.vdf);
  std::string others = fileOf(otherEntries());
  std::string prefix = others.substr(0, others.size() - 2);
  CHECK(now.compare(0, prefix.size(), prefix) == 0);   // entries 0-2 untouched
  std::string iconPath = env.grid + "/2957107289_icon.png";
  CHECK_EQ(now.size(), original.size() + iconPath.size());  // only the icon string grew

  steam::ShortcutsFile f;
  std::string err;
  REQUIRE(steam::ShortcutsFile::parse(now, &f, &err));
  REQUIRE_EQ(f.entries.size(), size_t(4));
  const auto* ours = findEntryNamed(f, "ReplayNES");
  REQUIRE(ours != nullptr);
  CHECK_EQ(ours->find("appid")->u32(), 2957107289u);
  CHECK_EQ(ours->find("LastPlayTime")->u32(), 1781000000u);
  CHECK(ours->find("LaunchOptions")->str.find("--command=replaynes-linux") != std::string::npos);
  CHECK_EQ(ours->find("icon")->str, iconPath);

  CHECK(readFile(env.grid + "/2957107289_hero.png") == fakePng("hero"));
  CHECK(readFile(env.grid + "/2957107289_hero.png.replaynes-backup") == fakePng("user hero"));
  CHECK(!fs::exists(env.grid + "/2957107289p.png"));
  CHECK(readFile(env.grid + "/2957107289.json") == "{\"custom\":1}");
  CHECK(readFile(env.vdf + ".replaynes-backup") == original);
}

TEST_CASE("steam: dry run, missing file, several accounts") {
  Env env("dry");
  fs::create_directories(env.userdata + "/0/config");       // anonymous: skipped
  fs::create_directories(env.userdata + "/777/config");     // no shortcuts.vdf yet
  fs::create_directories(env.userdata + "/notanid");
  writeFile(env.vdf, fileOf(otherEntries()));
  std::string before = readFile(env.vdf);

  auto o = env.opts();
  o.dryRun = true;
  steam::Report r;
  REQUIRE(steam::addToSteam(o, r));
  REQUIRE_EQ(r.users.size(), size_t(2));
  CHECK(r.users[0].action == steam::UserAction::Added);
  CHECK(r.users[0].shortcutsWritten);
  CHECK_EQ(r.users[0].artworkWritten.size(), size_t(6));
  CHECK(readFile(env.vdf) == before);
  CHECK(!fs::exists(env.grid));
  CHECK(!fs::exists(env.userdata + "/777/config/shortcuts.vdf"));
  CHECK(steam::formatReport(r).find("dry run") != std::string::npos);
  CHECK(r.summary.find("Would add") != std::string::npos);

  steam::Report r2;
  REQUIRE(steam::addToSteam(env.opts(), r2));
  std::string created = readFile(env.userdata + "/777/config/shortcuts.vdf");
  steam::ShortcutsFile f;
  std::string err;
  REQUIRE(steam::ShortcutsFile::parse(created, &f, &err));
  REQUIRE_EQ(f.entries.size(), size_t(1));
  CHECK_EQ(f.entries[0].node.key, std::string("0"));
  CHECK(!fs::exists(env.userdata + "/777/config/shortcuts.vdf.replaynes-backup"));
  CHECK(fs::exists(env.userdata + "/777/config/grid/2746019953p.png"));
}

TEST_CASE("steam: Steam running (log file lock) blocks shortcuts.vdf, --force overrides") {
  Env env("running");
  std::string original = fileOf(otherEntries());
  writeFile(env.vdf, original);
  std::string log = env.root + "/Steam/logs/console_log.txt";
  writeFile(log, "log");
  CHECK(steam::detectSteamRunning(env.root + "/Steam", env.root + "/home") == steam::SteamState::NotRunning);

  int fd = ::open(log.c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  REQUIRE(::flock(fd, LOCK_SH) == 0);  // what the Steam client holds while it runs
  CHECK(steam::detectSteamRunning(env.root + "/Steam", env.root + "/home") == steam::SteamState::Running);

  steam::Report r;
  CHECK_FALSE(steam::addToSteam(env.opts(), r));
  CHECK(r.steam == steam::SteamState::Running);
  CHECK(r.users[0].action == steam::UserAction::SteamRunning);
  CHECK(readFile(env.vdf) == original);
  CHECK(!fs::exists(env.grid));  // no entry yet: nothing to attach artwork to
  CHECK(r.summary.find("Close Steam") != std::string::npos);

  auto o = env.opts();
  o.force = true;
  steam::Report r2;
  CHECK(steam::addToSteam(o, r2));
  CHECK(r2.users[0].action == steam::UserAction::Added);
  CHECK(r2.summary.find("restart Steam") != std::string::npos);
  ::close(fd);
  CHECK(steam::detectSteamRunning(env.root + "/Steam", env.root + "/home") == steam::SteamState::NotRunning);
}

TEST_CASE("steam: existing entry + Steam running still gets its artwork") {
  Env env("running-existing");
  std::string mine = entry("0", 2957107289u, "ReplayNES", "\"/usr/bin/flatpak\"", "run io.github.replaynes.ReplayNES", "",
                           0, "");
  writeFile(env.vdf, fileOf(mine));
  std::string before = readFile(env.vdf);
  std::string log = env.root + "/Steam/logs/console_log.txt";
  writeFile(log, "log");
  int fd = ::open(log.c_str(), O_RDONLY);
  REQUIRE(::flock(fd, LOCK_SH) == 0);
  steam::Report r;
  CHECK_FALSE(steam::addToSteam(env.opts(), r));
  ::close(fd);
  CHECK(r.users[0].action == steam::UserAction::SteamRunning);
  CHECK(readFile(env.vdf) == before);  // the icon field waits for Steam to close
  CHECK(readFile(env.grid + "/2957107289p.png") == fakePng("capsule"));
  CHECK(r.restartSteam);
  CHECK(r.summary.find("Artwork installed") != std::string::npos);
}

TEST_CASE("steam: unreadable shortcuts.vdf is never overwritten; no Steam found") {
  Env env("corrupt");
  std::string junk = "this is not a vdf";
  writeFile(env.vdf, junk);
  steam::Report r;
  CHECK_FALSE(steam::addToSteam(env.opts(), r));
  CHECK(r.users[0].action == steam::UserAction::Failed);
  CHECK(readFile(env.vdf) == junk);
  CHECK(!fs::exists(env.vdf + ".replaynes-backup"));

  auto o = env.opts();
  o.userdataRoots.clear();
  o.home = env.root + "/nohome";
  ::unsetenv("REPLAYNES_STEAM_USERDATA");
  steam::Report r2;
  CHECK_FALSE(steam::addToSteam(o, r2));
  CHECK(r2.users.empty());
  CHECK(r2.summary.find("Steam not found") != std::string::npos);

  // discovery dedupes ~/.steam/steam -> ~/.local/share/Steam
  std::string home = env.root + "/h";
  fs::create_directories(home + "/.local/share/Steam/userdata/42");
  fs::create_directories(home + "/.steam");
  fs::create_directory_symlink(home + "/.local/share/Steam", home + "/.steam/steam");
  auto roots = steam::findUserdataRoots(home);
  REQUIRE_EQ(roots.size(), size_t(1));
  CHECK_EQ(roots[0], home + "/.local/share/Steam/userdata");
}
