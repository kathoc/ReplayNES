// Quick menu model (docs/design/UI_REDESIGN.md): the static menu tree, filtered by the platform's
// features, and the navigation state (stack of pages with their focus, L / R switching, breadcrumb).
// Every frontend draws the same tree; values / enabled states stay in the frontends.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "common.hpp"

using namespace rnf;

namespace {

struct ItemDef {
  const char* page;
  const char* id;
  const char* label;
  const char* description;
  const char* icon;
  rnf_menu_item_kind kind;
  const char* target;
  uint32_t features;  // all required (0: always); any-of for the Share tile (see below)
  std::vector<const char*> choices;
};

struct PageDef {
  const char* id;
  const char* title;
  const char* icon;
  rnf_menu_page_kind kind;
  const char* group;
  uint32_t features;
};

struct GroupDef {
  const char* id;
  const char* title;
  const char* icon;
};

constexpr uint32_t F_EXPORT = RNF_MENU_FEATURE_EXPORT, F_STREAM = RNF_MENU_FEATURE_STREAM, F_CRT = RNF_MENU_FEATURE_CRT,
                   F_STEAM = RNF_MENU_FEATURE_STEAM, F_OSK = RNF_MENU_FEATURE_OSK, F_UPDATES = RNF_MENU_FEATURE_UPDATES,
                   F_SLOW_AUDIO = RNF_MENU_FEATURE_SLOW_AUDIO, F_LOW_LATENCY = RNF_MENU_FEATURE_LOW_LATENCY,
                   F_QUIT = RNF_MENU_FEATURE_QUIT, F_FULLSCREEN = RNF_MENU_FEATURE_FULLSCREEN,
                   F_UI_SCALE = RNF_MENU_FEATURE_UI_SCALE;
// The Share page needs one of these.
constexpr uint32_t F_SHARE_ANY = F_EXPORT | F_STREAM;

const GroupDef kGroups[] = {
    {"settings", RNF_L("Settings"), "settings"},
};

const PageDef kPages[] = {
    {"quick", RNF_L("Quick Menu"), "menu-2", RNF_MENU_PAGE_TILES, "", 0},
    {"retry", RNF_L("Retry (menu)"), "history", RNF_MENU_PAGE_TILES, "", 0},
    {"takes", RNF_L("Takes"), "git-branch", RNF_MENU_PAGE_LIST, "", 0},
    {"bookmarks", RNF_L("Bookmarks"), "bookmark", RNF_MENU_PAGE_LIST, "", 0},
    {"practice", RNF_L("Practice"), "target", RNF_MENU_PAGE_CARDS, "", 0},
    {"share", RNF_L("Share"), "share", RNF_MENU_PAGE_TILES, "", F_SHARE_ANY},
    {"settings.display", RNF_L("Display (settings page)"), "device-desktop", RNF_MENU_PAGE_SETTINGS, "settings", 0},
    {"settings.controls", RNF_L("Controls"), "device-gamepad", RNF_MENU_PAGE_SETTINGS, "settings", 0},
    {"settings.sound", RNF_L("Sound"), "volume", RNF_MENU_PAGE_SETTINGS, "settings", 0},
    {"settings.system", RNF_L("System"), "adjustments-horizontal", RNF_MENU_PAGE_SETTINGS, "settings", 0},
    {"display.crt", RNF_L("CRT Details"), "device-tv-old", RNF_MENU_PAGE_SETTINGS, "", F_CRT},
    {"display.shape", RNF_L("Picture Shape"), "aspect-ratio", RNF_MENU_PAGE_SETTINGS, "", 0},
    {"controls.controller", RNF_L("Controller"), "device-gamepad", RNF_MENU_PAGE_CUSTOM, "", 0},
    {"controls.assign", RNF_L("Button Action"), "list", RNF_MENU_PAGE_LIST, "", 0},
    {"controls.keyboard", RNF_L("Keyboard"), "keyboard", RNF_MENU_PAGE_LIST, "", 0},
    {"controls.detail", RNF_L("More Controls"), "adjustments", RNF_MENU_PAGE_SETTINGS, "", 0},
    {"system.updates", RNF_L("Updates"), "download", RNF_MENU_PAGE_SETTINGS, "", F_UPDATES},
    {"system.about", RNF_L("About"), "info-circle", RNF_MENU_PAGE_SETTINGS, "", 0},
    {"system.detail", RNF_L("More Settings"), "adjustments", RNF_MENU_PAGE_SETTINGS, "", 0},
    {"game", RNF_L("Game"), "device-gamepad-2", RNF_MENU_PAGE_TILES, "", 0},
    {"library.projects", RNF_L("Projects"), "folder", RNF_MENU_PAGE_LIST, "", 0},
};

using K = rnf_menu_item_kind;
constexpr K RESUME = RNF_MENU_ITEM_RESUME, ACTION = RNF_MENU_ITEM_ACTION, PAGE = RNF_MENU_ITEM_PAGE,
            TOGGLE = RNF_MENU_ITEM_TOGGLE, CHOICE = RNF_MENU_ITEM_CHOICE, SLIDER = RNF_MENU_ITEM_SLIDER,
            INFO = RNF_MENU_ITEM_INFO;

// "Japanese" written in Japanese (shown as is, never translated).
#define RNF_JAPANESE_NAME "=\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"

const std::vector<ItemDef>& items() {
  static const std::vector<ItemDef> v = {
      // Quick Menu: six tiles, one row.
      {"quick", "resume", RNF_L("Resume"), RNF_L("Back to the game"), "player-play", RESUME, "", 0, {}},
      {"quick", "retry", RNF_L("Retry (menu)"), RNF_L("Record again from here, go back to an earlier try"), "history", PAGE, "retry", 0, {}},
      {"quick", "practice", RNF_L("Practice"), RNF_L("Repeat a section from A to B (nothing is recorded)"), "target", PAGE,
       "practice", 0, {}},
      {"quick", "share", RNF_L("Share"), RNF_L("Export a video or send the picture to other apps"), "share", PAGE, "share",
       F_SHARE_ANY, {}},
      {"quick", "settings", RNF_L("Settings"), RNF_L("Display, controls, sound and system"), "settings", PAGE,
       "settings.display", 0, {}},
      {"quick", "game", RNF_L("Game"), RNF_L("Choose a game, save, reset"), "device-gamepad-2", PAGE, "game", 0, {}},
      // Retry.
      {"retry", "retry.rerecord", RNF_L("Record from Here"), RNF_L("Continue recording from this point (the old continuation is kept)"),
       "player-record", ACTION, "", 0, {}},
      {"retry", "retry.undo", RNF_L("Previous Try"), RNF_L("Back to the take before the last re-recording"), "arrow-back-up",
       ACTION, "", 0, {}},
      {"retry", "retry.takes", RNF_L("Takes"), RNF_L("Every try of this recording"), "git-branch", PAGE, "takes", 0, {}},
      {"retry", "retry.bookmarks", RNF_L("Bookmarks"), RNF_L("Jump to a moment you marked"), "bookmark", PAGE, "bookmarks", 0, {}},
      {"retry", "retry.playback", RNF_L("Watch"), RNF_L("Playback mode: plays the recording without changing it"), "eye",
       TOGGLE, "", 0, {}},
      // The Reset Project confirmation (same as Game > Reset > Reset Project...): keep A/B, backup.
      {"retry", "retry.restart", RNF_L("Restart Recording"), RNF_L("Delete every take and record again from power-on"),
       "player-skip-back", ACTION, "", 0, {}},
      // Share.
      {"share", "share.export", RNF_L("Export MP4"), RNF_L("Save the take as one continuous video"), "movie", ACTION, "",
       F_EXPORT, {}},
      {"share", "share.stream", RNF_L("Stream Output"), RNF_L("Send the picture to OBS and other apps"), "broadcast", TOGGLE,
       "", F_STREAM, {}},
      // Settings: Display.
      {"settings.display", "display.size", RNF_L("Size"), RNF_L("Integer: sharp pixels · FILL: as large as the screen"),
       "maximize", CHOICE, "", 0, {RNF_L("Integer"), RNF_L("FILL")}},
      {"settings.display", "display.crt", RNF_L("CRT"), RNF_L("Looks like a CRT TV (display only)"), "device-tv-old", TOGGLE,
       "", F_CRT, {}},
      {"settings.display", "display.flash", RNF_L("Reduce Flashing"), RNF_L("Dims hard full-screen flashes (photosensitivity)"),
       "bolt-off", CHOICE, "", 0, {RNF_L("Off"), RNF_L("Low"), RNF_L("Standard"), RNF_L("High")}},
      {"settings.display", "display.vtr", RNF_L("VTR Effect"), RNF_L("Videotape-style noise while rewinding (display only)"),
       "rewind-backward-5", TOGGLE, "", 0, {}},
      {"settings.display", "display.shape", RNF_L("Picture Shape"), RNF_L("8:7 pixels, hide the edges"), "aspect-ratio", PAGE,
       "display.shape", 0, {}},
      {"settings.display", "display.crt_detail", RNF_L("CRT Details"), RNF_L("Scanlines, afterglow, signal"), "adjustments",
       PAGE, "display.crt", F_CRT, {}},
      // Display > Picture Shape.
      {"display.shape", "display.par87", RNF_L("8:7 Pixels"), RNF_L("The pixel shape of a CRT TV"), "aspect-ratio", TOGGLE,
       "", 0, {}},
      {"display.shape", "display.overscan", RNF_L("Hide Edges"), RNF_L("Hides 8 pixels on each side, like a TV"),
       "crop", TOGGLE, "", 0, {}},
      // Display > CRT details.
      {"display.crt", "crt.lines", RNF_L("Scanlines"), RNF_L("240 = standard; fewer lines is an experiment"), "line-scan", SLIDER,
       "", F_CRT, {}},
      {"display.crt", "crt.beam", RNF_L("Beam Growth"), RNF_L("Bright lines get thicker"), "line-height", TOGGLE, "", F_CRT, {}},
      {"display.crt", "crt.persistence", RNF_L("Afterglow"), RNF_L("Phosphor persistence"), "sun-low", TOGGLE, "", F_CRT, {}},
      {"display.crt", "crt.supply", RNF_L("Power Sag"), RNF_L("Bright screens widen and dim the picture"), "bolt", TOGGLE, "",
       F_CRT, {}},
      {"display.crt", "crt.antenna", RNF_L("Signal"), RNF_L("Antenna signal strength (weaker = noisier)"), "antenna", SLIDER,
       "", F_CRT, {}},
      {"display.crt", "crt.reset", RNF_L("Defaults"), RNF_L("Back to nesterm’s defaults"), "restore", ACTION, "", F_CRT, {}},
      // Settings: Controls.
      {"settings.controls", "controls.controller", RNF_L("Controller"), RNF_L("See and change what each button does"),
       "device-gamepad", PAGE, "controls.controller", 0, {}},
      {"settings.controls", "controls.keyboard", RNF_L("Keyboard"), RNF_L("Keys for the game and the hotkeys"), "keyboard",
       PAGE, "controls.keyboard", 0, {}},
      {"settings.controls", "controls.confirm", RNF_L("Confirm Button"), RNF_L("Which button confirms in menus (the other goes back)"),
       "circle-check", CHOICE, "", 0, {RNF_L("East Confirms"), RNF_L("South Confirms")}},
      {"settings.controls", "controls.dpad_paused", RNF_L("D-pad While Paused"), RNF_L("← / → step one frame while paused"),
       "arrows-horizontal", TOGGLE, "", 0, {}},
      {"settings.controls", "controls.osk", RNF_L("On-screen Keyboard"), RNF_L("For names and search: Steam’s or the built-in one"),
       "keyboard", CHOICE, "", F_OSK, {RNF_L("Automatic"), RNF_L("Built-in"), "=Steam"}},
      {"settings.controls", "controls.detail", RNF_L("More"), RNF_L("Turbo, opposite directions, stick"),
       "adjustments", PAGE, "controls.detail", 0, {}},
      // Controls > More.
      {"controls.detail", "controls.rewind_pause", RNF_L("Pause After Rewind"), RNF_L("Pause when rewind / fast-forward is released"),
       "player-pause", TOGGLE, "", 0, {}},
      {"controls.detail", "controls.turbo", RNF_L("Turbo Speed"), RNF_L("Presses per second of Turbo A / B"), "bolt",
       SLIDER, "", 0, {}},
      {"controls.detail", "controls.turbo_duty", RNF_L("Turbo Press Length"), RNF_L("Frames each turbo press is held"), "clock",
       SLIDER, "", 0, {}},
      {"controls.detail", "controls.socd", RNF_L("Opposite Directions"), RNF_L("← and → (or ↑ and ↓) pressed together"),
       "arrows-diff", CHOICE, "", 0, {RNF_L("Neutral"), RNF_L("Last Wins"), RNF_L("Both")}},
      {"controls.detail", "controls.stick", RNF_L("Stick Threshold"), RNF_L("How far the stick moves before it counts"),
       "device-gamepad-3", SLIDER, "", 0, {}},
      {"controls.detail", "controls.reset", RNF_L("Reset Controls"), RNF_L("Every key and button back to the defaults"),
       "restore", ACTION, "", 0, {}},
      // Settings: Sound.
      {"settings.sound", "sound.volume", RNF_L("Volume"), RNF_L("Game sound volume"), "volume", SLIDER, "", 0, {}},
      {"settings.sound", "sound.slow", RNF_L("Sound in Slow Motion"), RNF_L("Play the sound at half speed in slow motion"),
       "volume-2", TOGGLE, "", F_SLOW_AUDIO, {}},
      {"settings.sound", "sound.low_latency", RNF_L("Low Latency"), RNF_L("A smaller sound buffer (may crackle)"), "bolt",
       TOGGLE, "", F_LOW_LATENCY, {}},
      // Settings: System.
      {"settings.system", "system.language", RNF_L("Language"), RNF_L("Automatic follows the system language"), "language",
       CHOICE, "", 0, {RNF_L("Automatic"), RNF_JAPANESE_NAME, "=English"}},
      {"settings.system", "system.updates", RNF_L("Updates"), RNF_L("Check for a new version"), "download", PAGE,
       "system.updates", F_UPDATES, {}},
      {"settings.system", "system.steam", RNF_L("Add to Steam"), RNF_L("Adds ReplayNES to the Steam library (close Steam first)"),
       "brand-steam", ACTION, "", F_STEAM, {}},
      {"settings.system", "system.about", RNF_L("About"), RNF_L("Version, folders and license"), "info-circle", PAGE,
       "system.about", 0, {}},
      {"settings.system", "system.detail", RNF_L("More"), RNF_L("Full screen, UI size, autosave"), "adjustments", PAGE,
       "system.detail", 0, {}},
      {"settings.system", "system.quit", RNF_L("Quit"), RNF_L("Close ReplayNES (where you are is kept)"), "power", ACTION, "",
       F_QUIT, {}},
      // System > Updates.
      {"system.updates", "updates.status", RNF_L("Status"), "", "info-circle", INFO, "", F_UPDATES, {}},
      {"system.updates", "updates.check", RNF_L("Check Now"), RNF_L("Look for a new version now"), "refresh", ACTION, "",
       F_UPDATES, {}},
      {"system.updates", "updates.apply", RNF_L("Update"), RNF_L("Install the new version"), "download", ACTION, "",
       F_UPDATES, {}},
      {"system.updates", "updates.auto", RNF_L("Check Automatically"), RNF_L("Look for updates in the background"),
       "clock", TOGGLE, "", F_UPDATES, {}},
      // System > About.
      {"system.about", "about.version", RNF_L("Version"), "", "info-circle", INFO, "", 0, {}},
      {"system.about", "about.roms", RNF_L("ROM Folder"), RNF_L("Put .nes files here"), "folder", ACTION, "", 0, {}},
      {"system.about", "about.projects", RNF_L("Projects Folder"), RNF_L("Your recordings are saved here"), "folder", ACTION,
       "", 0, {}},
      {"system.about", "about.settings", RNF_L("Settings File"), "", "file-text", INFO, "", 0, {}},
      {"system.about", "about.license", RNF_L("License"), RNF_L("Free software (GPL-2.0-or-later)"), "license", INFO, "", 0, {}},
      // System > More.
      {"system.detail", "system.fullscreen", RNF_L("Full Screen"), RNF_L("Use the whole screen"), "maximize", TOGGLE, "",
       F_FULLSCREEN, {}},
      {"system.detail", "system.ui_scale", RNF_L("UI Size"), RNF_L("Size of menus and text"), "text-size", SLIDER, "",
       F_UI_SCALE, {}},
      {"system.detail", "system.autosave", RNF_L("Autosave"), RNF_L("How often the recording is saved"), "device-floppy",
       CHOICE, "", 0, {RNF_L("2 s"), RNF_L("3 s"), RNF_L("5 s"), RNF_L("10 s"), RNF_L("30 s")}},
      // Here, not under Controls: Controls and Controls > More are full on Linux (docs/design/UI_REDESIGN.md).
      {"system.detail", "system.resume_countdown", RNF_L("Resume Countdown"), RNF_L("3, 2, 1 before play resumes after a pause"),
       "player-play", TOGGLE, "", 0, {}},
      {"system.detail", "system.flash_badge", RNF_L("Flash Badge"), RNF_L("Show a badge while flashes are reduced"), "bolt-off",
       TOGGLE, "", 0, {}},
      {"system.detail", "system.stats", RNF_L("Latency Stats"), RNF_L("Measured latency and timing (F3)"), "chart-line", TOGGLE,
       "", 0, {}},
      // Game.
      {"game", "game.library", RNF_L("Choose Game"), RNF_L("Back to the game library"), "layout-grid", ACTION, "", 0, {}},
      {"game", "game.save", RNF_L("Save"), RNF_L("Save the project now"), "device-floppy", ACTION, "", 0, {}},
      {"game", "game.save_as", RNF_L("Save As"), RNF_L("Save under a new name"), "file-plus", ACTION, "", 0, {}},
      {"game", "game.open", RNF_L("Open Project"), RNF_L("Open a saved project"), "folder-open", ACTION, "", 0, {}},
      {"game", "game.reset", RNF_L("Reset"), RNF_L("Soft reset, power cycle or start over"), "refresh", ACTION, "", 0, {}},
  };
  return v;
}

bool featureOK(uint32_t need, uint32_t have) {
  if (need == 0) return true;
  if (need == F_SHARE_ANY) return (have & F_SHARE_ANY) != 0;
  return (need & have) == need;
}

struct Item {
  const ItemDef* def;
};

struct Page {
  const PageDef* def;
  std::vector<Item> items;
  size_t count = 0;  // LIST / CARDS
};

struct Level {
  size_t page;
  size_t focus;
};

const GroupDef* groupOf(const char* id) {
  if (!id || !*id) return nullptr;
  for (const GroupDef& g : kGroups)
    if (std::strcmp(g.id, id) == 0) return &g;
  return nullptr;
}

}  // namespace

struct rnf_menu {
  std::vector<Page> pages;
  std::vector<Level> stack;

  int find(const char* id) const {
    if (!id) return -1;
    for (size_t i = 0; i < pages.size(); ++i)
      if (std::strcmp(pages[i].def->id, id) == 0) return int(i);
    return -1;
  }
  size_t count(size_t p) const {
    const Page& pg = pages[p];
    rnf_menu_page_kind k = pg.def->kind;
    return k == RNF_MENU_PAGE_LIST || k == RNF_MENU_PAGE_CARDS ? pg.count : pg.items.size();
  }
  int columns(size_t p) const {
    switch (pages[p].def->kind) {
      case RNF_MENU_PAGE_TILES: return int(std::max<size_t>(1, pages[p].items.size()));
      case RNF_MENU_PAGE_CARDS: return 4;
      default: return 1;
    }
  }
  Level* top() { return stack.empty() ? nullptr : &stack.back(); }
  const Level* top() const { return stack.empty() ? nullptr : &stack.back(); }
};

namespace {

rnf_menu_event pushPage(rnf_menu* m, int p) {
  if (p < 0 || m->stack.size() >= RNF_MENU_MAX_DEPTH) return RNF_MENU_EVENT_NONE;
  m->stack.push_back({size_t(p), 0});
  return RNF_MENU_EVENT_PUSHED;
}

}  // namespace

namespace {
struct Crumb {
  rnf_menu_crumb c;
  size_t level;       // stack level it stands for
  const char* group;  // a group's title: the group id, else nullptr
};
std::vector<Crumb> crumbsOf(const rnf_menu* m) {
  std::vector<Crumb> v;
  // The Quick Menu itself is the root nobody needs to see; another root (the library's Settings)
  // is shown.
  size_t from = 0;
  if (!m->stack.empty() && std::strcmp(m->pages[m->stack[0].page].def->id, "quick") == 0) from = 1;
  const char* lastGroup = "";
  for (size_t i = from; i < m->stack.size(); ++i) {
    const PageDef* d = m->pages[m->stack[i].page].def;
    if (const GroupDef* g = groupOf(d->group); g && std::strcmp(lastGroup, d->group) != 0)
      v.push_back({{g->title, g->icon}, i, d->group});
    lastGroup = d->group;
    v.push_back({{d->title, d->icon}, i, nullptr});
  }
  return v;
}
}  // namespace

extern "C" {

rnf_menu* rnf_menu_new(uint32_t features) {
  RNF_GUARD_BEGIN
  auto m = std::make_unique<rnf_menu>();
  for (const PageDef& d : kPages) {
    if (!featureOK(d.features, features)) continue;
    Page p;
    p.def = &d;
    if (d.kind == RNF_MENU_PAGE_CARDS) p.count = RNF_MENU_MAX_CARDS;
    for (const ItemDef& it : items())
      if (std::strcmp(it.page, d.id) == 0 && featureOK(it.features, features)) p.items.push_back({&it});
    m->pages.push_back(std::move(p));
  }
  // Items opening pages left out by the features are left out too.
  for (Page& p : m->pages)
    p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                                 [&](const Item& i) { return i.def->kind == PAGE && m->find(i.def->target) < 0; }),
                  p.items.end());
  return m.release();
  RNF_GUARD_END(nullptr)
}

void rnf_menu_free(rnf_menu* m) { delete m; }

size_t rnf_menu_page_count(const rnf_menu* m) { return m ? m->pages.size() : 0; }

int rnf_menu_page_get(const rnf_menu* m, size_t page, rnf_menu_page_info* out) {
  if (!m || page >= m->pages.size() || !out) return 0;
  const PageDef* d = m->pages[page].def;
  out->id = d->id;
  out->title = d->title;
  out->icon = d->icon;
  out->kind = d->kind;
  out->columns = m->columns(page);
  out->count = m->count(page);
  out->group = d->group;
  return 1;
}

int rnf_menu_page_find(const rnf_menu* m, const char* id) { return m ? m->find(id) : -1; }

int rnf_menu_item_get(const rnf_menu* m, size_t page, size_t item, rnf_menu_item_info* out) {
  if (!m || page >= m->pages.size() || item >= m->pages[page].items.size() || !out) return 0;
  const ItemDef* d = m->pages[page].items[item].def;
  out->id = d->id;
  out->label = d->label;
  out->description = d->description;
  out->icon = d->icon;
  out->kind = d->kind;
  out->target = d->target;
  out->choice_count = d->choices.size();
  return 1;
}

int rnf_menu_item_find(const rnf_menu* m, size_t page, const char* id) {
  if (!m || page >= m->pages.size() || !id) return -1;
  const auto& v = m->pages[page].items;
  for (size_t i = 0; i < v.size(); ++i)
    if (std::strcmp(v[i].def->id, id) == 0) return int(i);
  return -1;
}

const char* rnf_menu_item_choice(const rnf_menu* m, size_t page, size_t item, size_t choice) {
  if (!m || page >= m->pages.size() || item >= m->pages[page].items.size()) return nullptr;
  const ItemDef* d = m->pages[page].items[item].def;
  return choice < d->choices.size() ? d->choices[choice] : nullptr;
}

void rnf_menu_set_count(rnf_menu* m, size_t page, size_t count) {
  if (!m || page >= m->pages.size()) return;
  Page& p = m->pages[page];
  if (p.def->kind != RNF_MENU_PAGE_LIST && p.def->kind != RNF_MENU_PAGE_CARDS) return;
  if (p.def->kind == RNF_MENU_PAGE_CARDS) count = std::min<size_t>(count, RNF_MENU_MAX_CARDS);
  p.count = count;
  for (Level& l : m->stack)
    if (l.page == page && l.focus >= std::max<size_t>(1, count)) l.focus = count ? count - 1 : 0;
}

int rnf_menu_open(rnf_menu* m, const char* root) {
  if (!m) return 0;
  int p = m->find(root ? root : "quick");
  if (p < 0) return 0;
  m->stack.clear();
  m->stack.push_back({size_t(p), 0});
  return 1;
}

void rnf_menu_close(rnf_menu* m) {
  if (m) m->stack.clear();
}

size_t rnf_menu_depth(const rnf_menu* m) { return m ? m->stack.size() : 0; }
size_t rnf_menu_level_page(const rnf_menu* m, size_t level) {
  return m && level < m->stack.size() ? m->stack[level].page : 0;
}
size_t rnf_menu_current(const rnf_menu* m) { return m && m->top() ? m->top()->page : 0; }
size_t rnf_menu_focus(const rnf_menu* m) { return m && m->top() ? m->top()->focus : 0; }

void rnf_menu_set_focus(rnf_menu* m, size_t item) {
  if (!m || !m->top()) return;
  size_t n = m->count(m->top()->page);
  m->top()->focus = n ? std::min(item, n - 1) : 0;
}

size_t rnf_menu_sheet(const rnf_menu* m) {
  if (!m || !m->top() || m->pages[m->top()->page].def->kind != RNF_MENU_PAGE_LIST) return 0;
  return m->top()->focus / RNF_MENU_MAX_ITEMS;
}

size_t rnf_menu_sheet_count(const rnf_menu* m) {
  if (!m || !m->top() || m->pages[m->top()->page].def->kind != RNF_MENU_PAGE_LIST) return 1;
  size_t n = m->count(m->top()->page);
  return std::max<size_t>(1, (n + RNF_MENU_MAX_ITEMS - 1) / RNF_MENU_MAX_ITEMS);
}

rnf_menu_event rnf_menu_move(rnf_menu* m, int dx, int dy, int* adjust_dir) {
  if (adjust_dir) *adjust_dir = 0;
  if (!m || !m->top() || (dx == 0 && dy == 0)) return RNF_MENU_EVENT_NONE;
  Level& l = *m->top();
  const Page& p = m->pages[l.page];
  size_t n = m->count(l.page);
  auto adjust = [&](int d) {
    if (adjust_dir) *adjust_dir = d;
    return RNF_MENU_EVENT_ADJUST;
  };
  switch (p.def->kind) {
    case RNF_MENU_PAGE_CUSTOM: return RNF_MENU_EVENT_NONE;
    case RNF_MENU_PAGE_SETTINGS:
      if (dx != 0) {
        if (l.focus >= n) return RNF_MENU_EVENT_NONE;
        K k = p.items[l.focus].def->kind;
        if (k == TOGGLE || k == CHOICE || k == SLIDER) return adjust(dx < 0 ? -1 : 1);
        return RNF_MENU_EVENT_NONE;
      }
      [[fallthrough]];
    case RNF_MENU_PAGE_LIST: {
      if (dx != 0) return n ? adjust(dx < 0 ? -1 : 1) : RNF_MENU_EVENT_NONE;
      if (n == 0) return RNF_MENU_EVENT_NONE;
      if (dy < 0 && l.focus > 0) { --l.focus; return RNF_MENU_EVENT_MOVED; }
      if (dy > 0 && l.focus + 1 < n) { ++l.focus; return RNF_MENU_EVENT_MOVED; }
      return RNF_MENU_EVENT_NONE;
    }
    case RNF_MENU_PAGE_TILES:
    case RNF_MENU_PAGE_CARDS: {
      if (n == 0) return RNF_MENU_EVENT_NONE;
      size_t cols = size_t(m->columns(l.page));
      size_t row = l.focus / cols, col = l.focus % cols, rows = (n + cols - 1) / cols;
      if (dx < 0 && col > 0) --col;
      else if (dx > 0 && col + 1 < cols && row * cols + col + 1 < n) ++col;
      else if (dy < 0 && row > 0) --row;
      else if (dy > 0 && row + 1 < rows) ++row;
      else return RNF_MENU_EVENT_NONE;
      size_t f = std::min(row * cols + col, n - 1);
      if (f == l.focus) return RNF_MENU_EVENT_NONE;
      l.focus = f;
      return RNF_MENU_EVENT_MOVED;
    }
  }
  return RNF_MENU_EVENT_NONE;
}

rnf_menu_event rnf_menu_confirm(rnf_menu* m) {
  if (!m || !m->top()) return RNF_MENU_EVENT_NONE;
  Level& l = *m->top();
  const Page& p = m->pages[l.page];
  rnf_menu_page_kind k = p.def->kind;
  if (k == RNF_MENU_PAGE_LIST || k == RNF_MENU_PAGE_CARDS || k == RNF_MENU_PAGE_CUSTOM)
    return m->count(l.page) > 0 || k == RNF_MENU_PAGE_CUSTOM ? RNF_MENU_EVENT_ACTIVATE : RNF_MENU_EVENT_NONE;
  if (l.focus >= p.items.size()) return RNF_MENU_EVENT_NONE;
  const ItemDef* d = p.items[l.focus].def;
  switch (d->kind) {
    case RESUME: return RNF_MENU_EVENT_CLOSE;
    case PAGE: return pushPage(m, m->find(d->target));
    case INFO: return RNF_MENU_EVENT_NONE;
    default: return RNF_MENU_EVENT_ACTIVATE;
  }
}

rnf_menu_event rnf_menu_back(rnf_menu* m) {
  if (!m || m->stack.empty()) return RNF_MENU_EVENT_NONE;
  if (m->stack.size() == 1) return RNF_MENU_EVENT_CLOSE;
  m->stack.pop_back();
  return RNF_MENU_EVENT_POPPED;
}

rnf_menu_event rnf_menu_switch(rnf_menu* m, int dir) {
  if (!m || !m->top() || dir == 0) return RNF_MENU_EVENT_NONE;
  Level& l = *m->top();
  const PageDef* d = m->pages[l.page].def;
  if (d->group && *d->group) {
    std::vector<size_t> sib;
    for (size_t i = 0; i < m->pages.size(); ++i)
      if (std::strcmp(m->pages[i].def->group, d->group) == 0) sib.push_back(i);
    if (sib.size() < 2) return RNF_MENU_EVENT_NONE;
    size_t at = size_t(std::find(sib.begin(), sib.end(), l.page) - sib.begin());
    at = (at + sib.size() + (dir < 0 ? sib.size() - 1 : 1)) % sib.size();
    l.page = sib[at];
    l.focus = 0;
    return RNF_MENU_EVENT_SWITCHED;
  }
  if (d->kind == RNF_MENU_PAGE_LIST) {
    size_t sheets = rnf_menu_sheet_count(m);
    if (sheets < 2) return RNF_MENU_EVENT_NONE;
    size_t s = (l.focus / RNF_MENU_MAX_ITEMS + sheets + (dir < 0 ? sheets - 1 : 1)) % sheets;
    l.focus = std::min(s * RNF_MENU_MAX_ITEMS, m->count(l.page) - 1);
    return RNF_MENU_EVENT_SWITCHED;
  }
  return RNF_MENU_EVENT_NONE;
}

rnf_menu_event rnf_menu_push(rnf_menu* m, const char* id) {
  if (!m) return RNF_MENU_EVENT_NONE;
  int p = m->find(id);
  if (p < 0) return RNF_MENU_EVENT_NONE;
  if (m->stack.empty()) return rnf_menu_open(m, id) ? RNF_MENU_EVENT_PUSHED : RNF_MENU_EVENT_NONE;
  return pushPage(m, p);
}


size_t rnf_menu_breadcrumb(const rnf_menu* m, rnf_menu_crumb* out, size_t cap) {
  if (!m) return 0;
  std::vector<Crumb> v = crumbsOf(m);
  for (size_t i = 0; out && i < v.size() && i < cap; ++i) out[i] = v[i].c;
  return v.size();
}

rnf_menu_event rnf_menu_crumb_select(rnf_menu* m, size_t crumb) {
  if (!m || m->stack.empty()) return RNF_MENU_EVENT_NONE;
  std::vector<Crumb> v = crumbsOf(m);
  if (crumb + 1 >= v.size()) return RNF_MENU_EVENT_NONE;  // out of range, or the current page
  const Crumb& c = v[crumb];
  bool popped = c.level + 1 < m->stack.size();
  m->stack.resize(c.level + 1);
  if (c.group) {
    for (size_t i = 0; i < m->pages.size(); ++i) {
      if (std::strcmp(m->pages[i].def->group, c.group) != 0) continue;
      Level& l = m->stack.back();
      if (l.page == i) break;  // already the group's first page
      l.page = i;
      l.focus = 0;
      return popped ? RNF_MENU_EVENT_POPPED : RNF_MENU_EVENT_SWITCHED;
    }
  }
  return popped ? RNF_MENU_EVENT_POPPED : RNF_MENU_EVENT_NONE;
}

const char* rnf_menu_text(const char* key) {
  if (!key) return "";
  if (key[0] == '=') return key + 1;
  return rnf_l10n_lookup(key);
}

}  // extern "C"
