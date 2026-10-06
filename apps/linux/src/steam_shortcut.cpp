// SPDX-License-Identifier: GPL-2.0-or-later
#include "steam_shortcut.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace steam {

// ------------------------------------------------------------------ binary VDF
namespace vdf {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

int valueSize(Type t) {
  switch (t) {
    case Type::Int32: case Type::Float32: case Type::Pointer: case Type::Color: return 4;
    case Type::UInt64: case Type::Int64: return 8;
    default: return -1;
  }
}

bool readCString(const std::string& b, size_t& pos, std::string* out) {
  size_t z = b.find('\0', pos);
  if (z == std::string::npos) return false;
  out->assign(b, pos, z - pos);
  pos = z + 1;
  return true;
}

}  // namespace

uint32_t Node::u32() const {
  if (raw.size() < 4) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(raw.data());
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void Node::setU32(uint32_t v) {
  raw.assign(4, '\0');
  for (int i = 0; i < 4; ++i) raw[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

const Node* Node::find(const std::string& k) const {
  std::string lk = lower(k);
  for (const Node& c : children)
    if (lower(c.key) == lk) return &c;
  return nullptr;
}

Node* Node::find(const std::string& k) { return const_cast<Node*>(static_cast<const Node*>(this)->find(k)); }

Node Node::string(const std::string& k, const std::string& v) {
  Node n;
  n.type = Type::String;
  n.key = k;
  n.str = v;
  return n;
}

Node Node::int32(const std::string& k, uint32_t v) {
  Node n;
  n.type = Type::Int32;
  n.key = k;
  n.setU32(v);
  return n;
}

Node Node::map(const std::string& k) {
  Node n;
  n.type = Type::Map;
  n.key = k;
  return n;
}

bool parseMapBody(const std::string& b, size_t& pos, Node& map, std::string* err, int depth) {
  auto fail = [&](const std::string& what) {
    if (err) *err = what + " at byte " + std::to_string(pos);
    return false;
  };
  if (depth > 32) return fail("nesting too deep");
  for (;;) {
    if (pos >= b.size()) return fail("unexpected end of file");
    size_t begin = pos;
    auto t = static_cast<Type>(static_cast<uint8_t>(b[pos++]));
    if (t == Type::End) {
      map.bodyEnd = begin;
      return true;
    }
    Node n;
    n.type = t;
    n.begin = begin;
    if (!readCString(b, pos, &n.key)) return fail("unterminated key");
    if (t == Type::Map) {
      if (!parseMapBody(b, pos, n, err, depth + 1)) return false;
    } else if (t == Type::String) {
      if (!readCString(b, pos, &n.str)) return fail("unterminated string");
    } else {
      int sz = valueSize(t);
      if (sz < 0) return fail("unsupported value type " + std::to_string(int(t)));
      if (pos + size_t(sz) > b.size()) return fail("truncated value");
      n.raw.assign(b, pos, sz);
      pos += sz;
    }
    n.end = pos;
    map.children.push_back(std::move(n));
  }
}

void write(const Node& n, std::string& out) {
  out.push_back(static_cast<char>(n.type));
  out += n.key;
  out.push_back('\0');
  if (n.type == Type::Map) {
    for (const Node& c : n.children) write(c, out);
    out.push_back(static_cast<char>(Type::End));
  } else if (n.type == Type::String) {
    out += n.str;
    out.push_back('\0');
  } else {
    out += n.raw;
  }
}

}  // namespace vdf

// ------------------------------------------------------------------ shortcuts.vdf
bool ShortcutsFile::parse(const std::string& bytes, ShortcutsFile* out, std::string* err) {
  *out = ShortcutsFile();
  if (bytes.empty()) return true;
  vdf::Node root;
  size_t pos = 0;
  if (!vdf::parseMapBody(bytes, pos, root, err)) return false;
  const vdf::Node* list = root.find("shortcuts");
  if (!list || list->type != vdf::Type::Map) {
    if (err) *err = "no \"shortcuts\" list";
    return false;
  }
  size_t first = list->children.empty() ? list->bodyEnd : list->children.front().begin;
  out->head = bytes.substr(0, first);
  out->tail = bytes.substr(list->bodyEnd);
  for (const vdf::Node& c : list->children) {
    Entry e;
    e.raw = bytes.substr(c.begin, c.end - c.begin);
    e.node = c;
    out->entries.push_back(std::move(e));
  }
  return true;
}

std::string ShortcutsFile::serialize() const {
  std::string out = head;
  for (const Entry& e : entries) {
    if (e.dirty) vdf::write(e.node, out);
    else out += e.raw;
  }
  out += tail;
  return out;
}

std::string ShortcutsFile::nextKey() const {
  long next = 0;
  for (const Entry& e : entries) {
    char* end = nullptr;
    long k = std::strtol(e.node.key.c_str(), &end, 10);
    if (end && *end == '\0' && !e.node.key.empty() && k >= next) next = k + 1;
  }
  return std::to_string(next);
}

uint32_t crc32(const std::string& data) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : data) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t shortcutAppId(const std::string& exe, const std::string& appName) {
  return crc32(exe + appName) | 0x80000000u;
}

uint64_t shortcutGameId64(uint32_t appId) { return (uint64_t(appId) << 32) | 0x02000000u; }

// ------------------------------------------------------------------ helpers
namespace {

bool readFile(const std::string& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return !f.bad();
}

/// Writes via a temporary file in the same folder + rename (atomic replace), keeping `mode`.
bool writeFileAtomic(const std::string& path, const std::string& data, mode_t mode, std::string* err) {
  std::string tmp = path + ".replaynes-tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) {
    *err = tmp + ": " + std::strerror(errno);
    return false;
  }
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      *err = tmp + ": " + std::strerror(errno);
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }
    off += size_t(n);
  }
  ::fchmod(fd, mode);
  ::fsync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    *err = path + ": " + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool isDigits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string getStr(const vdf::Node& e, const char* k) {
  const vdf::Node* n = e.find(k);
  return n && n->type == vdf::Type::String ? n->str : std::string();
}

bool contains(const std::string& h, const std::string& n) { return !n.empty() && h.find(n) != std::string::npos; }

bool iequals(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

/// 2: launches our Flatpak (any launch syntax), 1: same name, 0: something else.
int matchScore(const vdf::Node& e, const AddOptions& o) {
  if (getStr(e, "FlatpakAppID") == o.flatpakId || contains(getStr(e, "LaunchOptions"), o.flatpakId) ||
      contains(getStr(e, "Exe"), o.flatpakId))
    return 2;
  if (iequals(getStr(e, "AppName"), o.appName)) return 1;
  return 0;
}

vdf::Node newEntry(const std::string& key, uint32_t appId, const std::string& iconPath, const AddOptions& o) {
  using vdf::Node;
  Node e = Node::map(key);
  e.children = {Node::int32("appid", appId),
                Node::string("AppName", o.appName),
                Node::string("Exe", o.exe),
                Node::string("StartDir", o.startDir),
                Node::string("icon", iconPath),
                Node::string("ShortcutPath", ""),
                Node::string("LaunchOptions", o.launchOptions),
                Node::int32("IsHidden", 0),
                Node::int32("AllowDesktopConfig", 1),
                Node::int32("AllowOverlay", 1),
                Node::int32("OpenVR", 0),
                Node::int32("Devkit", 0),
                Node::string("DevkitGameID", ""),
                Node::int32("DevkitOverrideAppID", 0),
                Node::int32("LastPlayTime", 0),
                Node::string("FlatpakAppID", ""),
                Node::string("sortas", "")};
  Node tags = Node::map("tags");
  for (size_t i = 0; i < o.tags.size(); ++i) tags.children.push_back(Node::string(std::to_string(i), o.tags[i]));
  e.children.push_back(tags);
  return e;
}

struct Art { const char* src; std::string suffix; };

const Art kArt[] = {{"capsule.png", "p.png"}, {"wide.png", ".png"}, {"hero.png", "_hero.png"},
                    {"logo.png", "_logo.png"}, {"icon.png", "_icon.png"}};

/// Steam's logo placement for the hero (written once; the user may move the logo in Steam).
const char* kLogoJson =
    "{\"nVersion\":1,\"logoPosition\":{\"pinnedPosition\":\"BottomLeft\",\"nWidthPct\":50,\"nHeightPct\":50}}";

/// Installs the grid artwork for `appId`. Files with identical content are left alone; a
/// different file (the user's own art) is kept once as <name>.replaynes-backup; a JPEG the user
/// put in the same slot wins (ours is not written).
void installArtwork(const std::string& gridDir, uint32_t appId, const AddOptions& o, const std::string& artDir,
                    UserResult& u, bool* failed) {
  std::error_code ec;
  if (!o.dryRun) fs::create_directories(gridDir, ec);
  std::string id = std::to_string(appId);
  for (const Art& a : kArt) {
    std::string src = artDir + "/" + a.src;
    std::string dst = gridDir + "/" + id + a.suffix;
    std::string data;
    if (!readFile(src, &data) || data.empty()) {
      u.notes.push_back(std::string("artwork missing: ") + src);
      *failed = true;
      continue;
    }
    std::string jpg = dst.substr(0, dst.size() - 4) + ".jpg";
    if (fs::exists(jpg, ec)) {
      u.notes.push_back("kept the user's " + fs::path(jpg).filename().string());
      continue;
    }
    std::string cur;
    if (readFile(dst, &cur)) {
      if (cur == data) continue;
      std::string bak = dst + ".replaynes-backup";
      if (!fs::exists(bak, ec) && !o.dryRun) {
        fs::copy_file(dst, bak, fs::copy_options::overwrite_existing, ec);
        if (!ec) u.notes.push_back("previous " + fs::path(dst).filename().string() + " kept as " +
                                   fs::path(bak).filename().string());
      }
    }
    u.artworkWritten.push_back(dst);
    if (o.dryRun) continue;
    std::string err;
    if (!writeFileAtomic(dst, data, 0644, &err)) {
      u.notes.push_back("could not write " + err);
      *failed = true;
    }
  }
  std::string json = gridDir + "/" + id + ".json";
  if (!fs::exists(json, ec)) {
    u.artworkWritten.push_back(json);
    std::string err;
    if (!o.dryRun && !writeFileAtomic(json, kLogoJson, 0644, &err)) u.notes.push_back("could not write " + err);
  }
}

void processUser(const std::string& userDir, const std::string& accountId, const AddOptions& o,
                 const std::string& artDir, SteamState steam, UserResult& u) {
  u.accountId = accountId;
  u.userDir = userDir;
  std::string configDir = userDir + "/config";
  std::string gridDir = configDir + "/grid";
  u.shortcutsPath = configDir + "/shortcuts.vdf";

  std::string bytes;
  bool exists = readFile(u.shortcutsPath, &bytes);
  ShortcutsFile file;
  std::string err;
  if (!ShortcutsFile::parse(bytes, &file, &err)) {
    u.action = UserAction::Failed;
    u.error = "shortcuts.vdf not readable as a Steam shortcut list (" + err + "); left unchanged";
    return;
  }

  // Our entry: one that launches our Flatpak, else one with our name.
  int best = 0;
  size_t idx = 0;
  int strongCount = 0;
  for (size_t i = 0; i < file.entries.size(); ++i) {
    int s = matchScore(file.entries[i].node, o);
    if (s == 2) ++strongCount;
    if (s > best) best = s, idx = i;
  }
  if (strongCount > 1)
    u.notes.push_back(std::to_string(strongCount) + " ReplayNES entries in Steam; updating the first one");

  bool steamBlocks = steam == SteamState::Running && !o.force;
  std::string before = file.serialize();
  if (best > 0) {
    ShortcutsFile::Entry& e = file.entries[idx];
    u.existingEntry = true;
    u.entryKey = e.node.key;
    const vdf::Node* aid = e.node.find("appid");
    u.appId = aid && aid->type == vdf::Type::Int32
                  ? aid->u32()
                  : shortcutAppId(getStr(e.node, "Exe"), getStr(e.node, "AppName"));
    std::string iconPath = gridDir + "/" + std::to_string(u.appId) + "_icon.png";
    vdf::Node* icon = e.node.find("icon");
    std::error_code ec;
    if (icon && icon->type == vdf::Type::String) {
      if (icon->str.empty() || (!fs::exists(icon->str, ec) && icon->str != iconPath)) {
        icon->str = iconPath;
        e.dirty = true;
      }
    } else if (!icon) {
      e.node.children.push_back(vdf::Node::string("icon", iconPath));
      e.dirty = true;
    }
  } else {
    u.appId = shortcutAppId(o.exe, o.appName);
    ShortcutsFile::Entry e;
    e.node = newEntry(file.nextKey(), u.appId, gridDir + "/" + std::to_string(u.appId) + "_icon.png", o);
    e.dirty = true;
    u.entryKey = e.node.key;
    file.entries.push_back(std::move(e));
  }
  std::string after = file.serialize();
  bool vdfChanges = after != before || !exists;

  bool artFailed = false;
  if (u.existingEntry || !steamBlocks) installArtwork(gridDir, u.appId, o, artDir, u, &artFailed);

  if (vdfChanges && steamBlocks) {
    u.action = UserAction::SteamRunning;
    u.notes.push_back(u.existingEntry ? "shortcut icon not updated: Steam is running"
                                      : "not added: Steam is running");
    return;
  }
  if (vdfChanges && !o.dryRun) {
    std::error_code ec;
    fs::create_directories(configDir, ec);
    mode_t mode = 0644;
    struct stat st{};
    if (exists && ::stat(u.shortcutsPath.c_str(), &st) == 0) mode = st.st_mode & 07777;
    if (exists) {
      u.backupPath = u.shortcutsPath + ".replaynes-backup";
      if (!writeFileAtomic(u.backupPath, bytes, mode, &err)) {
        u.action = UserAction::Failed;
        u.error = "backup failed: " + err;
        u.backupPath.clear();
        return;
      }
    }
    if (!writeFileAtomic(u.shortcutsPath, after, mode, &err)) {
      u.action = UserAction::Failed;
      u.error = err;
      return;
    }
    u.shortcutsWritten = true;
  } else if (vdfChanges) {
    u.shortcutsWritten = true;  // dry run: would be written
    if (exists) u.backupPath = u.shortcutsPath + ".replaynes-backup";
  }
  if (artFailed) {
    u.action = UserAction::Failed;
    if (u.error.empty()) u.error = "artwork incomplete";
  } else if (!u.existingEntry) {
    u.action = UserAction::Added;
  } else {
    u.action = vdfChanges || !u.artworkWritten.empty() ? UserAction::Updated : UserAction::Unchanged;
  }
}

std::string realOr(const std::string& p) {
  std::error_code ec;
  fs::path c = fs::canonical(p, ec);
  return ec ? p : c.string();
}

const char* actionName(UserAction a) {
  switch (a) {
    case UserAction::Added: return "added";
    case UserAction::Updated: return "updated";
    case UserAction::Unchanged: return "already up to date";
    case UserAction::SteamRunning: return "waiting for Steam to close";
    case UserAction::Failed: return "failed";
  }
  return "?";
}

}  // namespace

// ------------------------------------------------------------------ discovery
std::vector<std::string> findUserdataRoots(const std::string& home) {
  std::vector<std::string> out, seen;
  for (const char* rel : {"/.local/share/Steam/userdata", "/.steam/steam/userdata", "/.steam/root/userdata",
                          "/.var/app/com.valvesoftware.Steam/.local/share/Steam/userdata"}) {
    std::string p = home + rel;
    std::error_code ec;
    if (!fs::is_directory(p, ec)) continue;
    std::string c = realOr(p);
    if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
    seen.push_back(c);
    out.push_back(p);
  }
  return out;
}

SteamState detectSteamRunning(const std::string& steamRoot, const std::string& home) {
  // 1. flock probe on the client's log files (works through the Flatpak sandbox: locks are
  //    per inode, the pid namespace does not matter).
  bool sawLog = false;
  for (const char* f : {"console_log.txt", "bootstrap_log.txt", "connection_log.txt", "configstore_log.txt"}) {
    std::string p = steamRoot + "/logs/" + f;
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) continue;
    sawLog = true;
    int r = ::flock(fd, LOCK_EX | LOCK_NB);
    int e = errno;
    if (r == 0) ::flock(fd, LOCK_UN);
    ::close(fd);
    if (r != 0 && e == EWOULDBLOCK) return SteamState::Running;
  }
  // 2. Outside a sandbox: ~/.steam/steam.pid + /proc/<pid>/comm.
  bool sandboxed = ::access("/.flatpak-info", F_OK) == 0;
  std::string pidText;
  if (!sandboxed && readFile(home + "/.steam/steam.pid", &pidText)) {
    long pid = std::atol(pidText.c_str());
    std::string comm;
    if (pid > 0 && readFile("/proc/" + std::to_string(pid) + "/comm", &comm)) {
      if (comm.rfind("steam", 0) == 0) return SteamState::Running;
    }
    return SteamState::NotRunning;
  }
  return sawLog ? SteamState::NotRunning : SteamState::Unknown;
}

std::string defaultArtworkDir() {
  if (const char* e = std::getenv("REPLAYNES_STEAM_ARTWORK"); e && *e) return e;
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    fs::path d = exe.parent_path().parent_path() / "share/replaynes/steam-artwork";
    if (fs::is_directory(d, ec)) return d.string();
  }
  return "/app/share/replaynes/steam-artwork";
}

// ------------------------------------------------------------------ add
bool addToSteam(const AddOptions& optIn, Report& r) {
  AddOptions o = optIn;
  r = Report();
  r.dryRun = o.dryRun;
  if (o.home.empty()) {
    const char* h = std::getenv("HOME");
    o.home = h ? h : "";
  }
  r.artworkDir = o.artworkDir.empty() ? defaultArtworkDir() : o.artworkDir;
  if (o.userdataRoots.empty()) {
    if (const char* e = std::getenv("REPLAYNES_STEAM_USERDATA"); e && *e) o.userdataRoots.push_back(e);
    else o.userdataRoots = findUserdataRoots(o.home);
  }
  r.roots = o.userdataRoots;
  if (r.roots.empty()) {
    r.summary = "Steam not found (no ~/.local/share/Steam/userdata). Install Steam and sign in once.";
    return false;
  }

  r.steam = SteamState::NotRunning;
  for (const std::string& root : r.roots) {
    SteamState s = detectSteamRunning(fs::path(root).parent_path().string(), o.home);
    if (s == SteamState::Running) r.steam = SteamState::Running;
    else if (s == SteamState::Unknown && r.steam != SteamState::Running) r.steam = SteamState::Unknown;
  }

  for (const std::string& root : r.roots) {
    std::error_code ec;
    std::vector<std::string> ids;
    for (const auto& de : fs::directory_iterator(root, ec)) {
      std::string name = de.path().filename().string();
      if (isDigits(name) && name != "0" && de.is_directory(ec)) ids.push_back(name);
    }
    std::sort(ids.begin(), ids.end());
    for (const std::string& id : ids) {
      UserResult u;
      processUser(root + "/" + id, id, o, r.artworkDir, r.steam, u);
      r.users.push_back(std::move(u));
    }
  }

  bool ok = !r.users.empty();
  int added = 0, updated = 0, waiting = 0, failed = 0;
  for (const UserResult& u : r.users) {
    if (u.action == UserAction::Added) ++added;
    if (u.action == UserAction::Updated) ++updated;
    if (u.action == UserAction::SteamRunning) ++waiting;
    if (u.action == UserAction::Failed) ++failed;
    if (u.action == UserAction::Failed || u.action == UserAction::SteamRunning) ok = false;
  }
  bool artOnly = false;
  for (const UserResult& u : r.users)
    if (u.action == UserAction::SteamRunning && !u.artworkWritten.empty()) artOnly = true;
  r.restartSteam = added + updated > 0 || artOnly;

  bool dry = o.dryRun;
  std::string pre = dry ? "[dry run] " : "";
  std::string did = added ? (dry ? "Would add ReplayNES to Steam" : "Added to Steam")
                          : (dry ? "Would update the Steam entry" : "Steam entry updated");
  if (r.users.empty()) r.summary = "No Steam account found. Start Steam and sign in once, then try again.";
  else if (failed) r.summary = pre + "Add to Steam failed - see details.";
  else if (waiting)
    r.summary = pre + std::string(artOnly ? (dry ? "Artwork would be installed. " : "Artwork installed. ") : "") +
                "Close Steam first (Desktop Mode: Steam menu > Exit), then try again.";
  else if (added + updated == 0) r.summary = pre + "ReplayNES is already in Steam with its artwork.";
  else if (r.steam == SteamState::Running) r.summary = pre + did + " - restart Steam to see the changes.";
  else r.summary = pre + did + " - start (or restart) Steam to see it.";
  return ok;
}

std::string formatReport(const Report& r) {
  std::ostringstream s;
  s << r.summary << "\n";
  s << "Steam: "
    << (r.steam == SteamState::Running ? "running" : r.steam == SteamState::NotRunning ? "not running" : "unknown")
    << (r.dryRun ? "   (dry run: nothing written)" : "") << "\n";
  s << "Artwork: " << r.artworkDir << "\n";
  for (const std::string& root : r.roots) s << "Userdata: " << root << "\n";
  for (const UserResult& u : r.users) {
    s << "Account " << u.accountId << ": " << actionName(u.action) << "\n";
    s << "  app id " << u.appId << " (rungameid " << shortcutGameId64(u.appId) << "), entry \"" << u.entryKey << "\""
      << (u.existingEntry ? " (existing)" : " (new)") << "\n";
    s << "  shortcuts.vdf: " << u.shortcutsPath << (u.shortcutsWritten ? (r.dryRun ? " - would be written" : " - written") : " - unchanged")
      << "\n";
    if (!u.backupPath.empty()) s << "  backup: " << u.backupPath << "\n";
    for (const std::string& a : u.artworkWritten) s << "  artwork: " << a << "\n";
    for (const std::string& n : u.notes) s << "  note: " << n << "\n";
    if (!u.error.empty()) s << "  error: " << u.error << "\n";
  }
  return s.str();
}

bool runCli(int argc, char** argv, int* exitCode) {
  bool want = false;
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--add-to-steam")) want = true;
  if (!want) return false;
  AddOptions o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--add-to-steam") continue;
    if (a == "--dry-run") o.dryRun = true;
    else if (a == "--force") o.force = true;
    else if (a == "--steam-userdata" && i + 1 < argc) o.userdataRoots.push_back(argv[++i]);
    else if (a == "--steam-artwork" && i + 1 < argc) o.artworkDir = argv[++i];
    else {
      std::fprintf(stderr,
                   "unknown option %s\nusage: --add-to-steam [--dry-run] [--force] [--steam-userdata DIR] "
                   "[--steam-artwork DIR]\n",
                   a.c_str());
      *exitCode = 2;
      return true;
    }
  }
  Report r;
  bool ok = addToSteam(o, r);
  std::fputs(formatReport(r).c_str(), stdout);
  bool waiting = false;
  for (const UserResult& u : r.users) waiting |= u.action == UserAction::SteamRunning;
  *exitCode = ok ? 0 : waiting ? 3 : 1;
  return true;
}

}  // namespace steam
