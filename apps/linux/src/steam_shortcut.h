// "Add to Steam": a non-Steam shortcut for ReplayNES in every local Steam account, with the
// library artwork (capsule, wide capsule, hero, logo, icon) from the Flatpak
// (/app/share/replaynes/steam-artwork, apps/linux/steam/artwork/).
//
//   userdata roots   ~/.local/share/Steam/userdata, ~/.steam/steam/userdata, ~/.steam/root/userdata
//                    (deduplicated; REPLAYNES_STEAM_USERDATA or AddOptions::userdataRoots override)
//   per account      <root>/<account id>/config/shortcuts.vdf   (binary VDF)
//                    <root>/<account id>/config/grid/<app id>{p,,_hero,_logo,_icon}.png, <app id>.json
//
// shortcuts.vdf is rewritten only when our entry changes: every other entry is copied back
// byte for byte, the previous file is kept as shortcuts.vdf.replaynes-backup, and the new file
// replaces it atomically. An existing ReplayNES entry (one the user added through Steam, or ours
// from an earlier run) is updated in place and keeps its app id, launch fields and play time;
// otherwise an entry is appended with the app id Steam derives for shortcuts:
// crc32(Exe + AppName) | 0x80000000 (grid files use it as an unsigned 32-bit number).
//
// Steam keeps the shortcut list in memory and writes it back, so shortcuts.vdf is only modified
// while Steam is closed (AddOptions::force overrides). Running detection works inside the
// Flatpak sandbox: the Steam client holds flock() locks on its log files
// (<Steam>/logs/*.txt, finish-arg --filesystem=~/.local/share/Steam/logs:ro); outside a sandbox
// ~/.steam/steam.pid + /proc is checked as well. Grid artwork for an existing entry is installed
// even while Steam runs (Steam picks it up after a restart).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace steam {

// ------------------------------------------------------------------ binary VDF
namespace vdf {

enum class Type : uint8_t {
  Map = 0, String = 1, Int32 = 2, Float32 = 3, Pointer = 4, Color = 6, UInt64 = 7, End = 8, Int64 = 10
};

struct Node {
  Type type = Type::Map;
  std::string key;
  std::string str;            ///< String
  std::string raw;            ///< the 4 / 8 value bytes of the numeric types, as stored
  std::vector<Node> children; ///< Map
  // Byte offsets in the parsed buffer: [begin, end) covers the type byte .. end of the value
  // (a map's terminating 0x08 included); bodyEnd: offset of a map's terminating 0x08.
  size_t begin = 0, end = 0, bodyEnd = 0;

  uint32_t u32() const;
  void setU32(uint32_t v);
  /// Case-insensitive key lookup (older Steam versions wrote lowercase keys).
  const Node* find(const std::string& k) const;
  Node* find(const std::string& k);
  static Node string(const std::string& k, const std::string& v);
  static Node int32(const std::string& k, uint32_t v);
  static Node map(const std::string& k);
};

/// Parses the children of a map body starting at `pos` up to and including its 0x08.
bool parseMapBody(const std::string& b, size_t& pos, Node& map, std::string* err, int depth = 0);
/// Serialises one node (type byte, key, value / children + 0x08).
void write(const Node& n, std::string& out);

}  // namespace vdf

// ------------------------------------------------------------------ shortcuts.vdf
/// shortcuts.vdf split so that untouched entries are written back exactly as read.
struct ShortcutsFile {
  struct Entry {
    std::string raw;   ///< the entry's bytes (type byte .. its 0x08)
    vdf::Node node;    ///< parsed copy (key = "0", "1", ...)
    bool dirty = false;
  };
  std::string head = std::string("\0shortcuts\0", 11);  ///< bytes before the first entry
  std::vector<Entry> entries;
  std::string tail = std::string("\x08\x08", 2);       ///< from the list's 0x08 to the end

  /// An empty input is an empty list. False (and *err) for anything that is not a binary VDF
  /// with a "shortcuts" map - such a file is never overwritten.
  static bool parse(const std::string& bytes, ShortcutsFile* out, std::string* err);
  std::string serialize() const;
  /// Next free numeric key ("0" for an empty list).
  std::string nextKey() const;
};

/// Steam's id for a non-Steam shortcut: crc32(exe + appName) | 0x80000000 (exe as stored,
/// quotes included). Grid file names use this value as an unsigned decimal number.
uint32_t shortcutAppId(const std::string& exe, const std::string& appName);
/// The 64-bit id of Steam's legacy grid names / steam://rungameid/ URLs.
uint64_t shortcutGameId64(uint32_t appId);
uint32_t crc32(const std::string& data);

// ------------------------------------------------------------------ add to Steam
enum class SteamState { NotRunning, Running, Unknown };

struct AddOptions {
  std::string appName = "ReplayNES";
  std::string exe = "\"/usr/bin/flatpak\"";  ///< Steam's own format for Flatpak shortcuts
  std::string startDir = "/usr/bin/";
  std::string launchOptions = "run io.github.replaynes.ReplayNES";
  std::string flatpakId = "io.github.replaynes.ReplayNES";  ///< matches existing entries
  std::vector<std::string> tags = {"Emulator"};             ///< new entries only
  /// capsule.png, wide.png, hero.png, logo.png, icon.png. Empty: defaultArtworkDir().
  std::string artworkDir;
  /// Steam "userdata" folders. Empty: $REPLAYNES_STEAM_USERDATA, else findUserdataRoots(home).
  std::vector<std::string> userdataRoots;
  std::string home;   ///< empty: $HOME
  bool dryRun = false;
  /// Modify shortcuts.vdf even though Steam seems to be running.
  bool force = false;
};

enum class UserAction {
  Added,        ///< new entry appended
  Updated,      ///< existing entry changed (icon) and / or artwork installed
  Unchanged,    ///< everything already in place
  SteamRunning, ///< shortcuts.vdf left alone because Steam runs (artwork may be installed)
  Failed
};

struct UserResult {
  std::string accountId;
  std::string userDir;        ///< <root>/<account id>
  std::string shortcutsPath;
  UserAction action = UserAction::Unchanged;
  uint32_t appId = 0;         ///< grid id (unsigned)
  bool existingEntry = false; ///< an entry for ReplayNES was already there
  std::string entryKey;
  bool shortcutsWritten = false;
  std::string backupPath;     ///< set when shortcuts.vdf was backed up
  std::vector<std::string> artworkWritten;  ///< grid files written (or to be written: dry run)
  std::vector<std::string> notes;
  std::string error;
};

struct Report {
  SteamState steam = SteamState::Unknown;
  bool dryRun = false;
  std::string artworkDir;
  std::vector<std::string> roots;
  std::vector<UserResult> users;
  /// One line for the UI ("Added to Steam - restart Steam to see it", "Close Steam first", ...).
  std::string summary;
  bool restartSteam = false;  ///< changes take effect after a Steam restart
};

/// Adds / updates the shortcut + artwork for every Steam account found. True when every
/// account succeeded (SteamRunning counts as not done). Never throws.
bool addToSteam(const AddOptions& opt, Report& report);
/// Multi-line, human-readable report (CLI, logs).
std::string formatReport(const Report& r);

std::vector<std::string> findUserdataRoots(const std::string& home);
/// Steam root = the userdata root's parent.
SteamState detectSteamRunning(const std::string& steamRoot, const std::string& home);
std::string defaultArtworkDir();

/// `--add-to-steam [--dry-run] [--force] [--steam-userdata DIR] [--steam-artwork DIR]`:
/// returns false when argv has no --add-to-steam; otherwise runs it, prints the report and sets
/// *exitCode (0 ok, 1 failure, 3 Steam running).
bool runCli(int argc, char** argv, int* exitCode);

}  // namespace steam
