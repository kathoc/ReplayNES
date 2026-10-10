// The library (start screen, docs/design/UI_REDESIGN.md): a "Continue" hero card (the last
// session - also after a crash - else the latest project: its picture, game name and when it was
// played; A resumes it), a row of filter chips (All / Favorites / Recent), the sort (Name / Last
// Played / Maker / Year / Genre) and the search, and pages of large game cards, four per row
// (picture of the last session of the game, else a tile made from its name; the title from the
// game database in the UI language, "maker · year" under it, a star on favourites).
// A plays the focused game (a new project), Y marks it as a favourite, X lists its projects,
// View / Select (Tab) cycles the sort, L / R turn the pages, the Menu pill (L+R / Esc) opens
// Settings. An empty library is one card saying where the games go. "Add Test Cartridge" (the last
// item of the row; Y on the empty card) writes the ReplayNES Test Cartridge into the ROM folder (once)
// and focuses it. Nothing scrolls.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

#include "app_model.h"
#include "emulation.h"
#include "icons.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "library.h"
#include "library_thumbs.h"
#include "pad_nav.h"
#include "paths.h"
#include "platform/platform.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui.h"
#include "ui_theme.h"

namespace rnl {

using namespace theme;

namespace {
void icon(ImDrawList* dl, const char* name, ImVec2 center, float size, ImU32 col) {
  const char* g = icons::glyph(name);
  if (!*g) return;
  ImVec2 ts = measure(size, g);
  dl->AddText(ImGui::GetFont(), size, ImVec2(center.x - ts.x / 2, center.y - ts.y / 2), col, g);
}

std::string takeString(char* s) {
  std::string out = s ? s : "";
  rnf_string_free(s);
  return out;
}

std::string fileName(const std::string& path) {
  size_t i = path.find_last_of("/\\");
  return i == std::string::npos ? path : path.substr(i + 1);
}

uint32_t hashOf(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}

// A tile for a game without a picture: a dark gradient in a color of its own + its initial.
void nameTile(ImDrawList* dl, const LRect& r, const std::string& name, float radius, float fontSize) {
  uint32_t h = hashOf(name);
  float hue = float(h % 360) / 360.0f;
  ImVec4 c1, c2;
  ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.42f, c1.x, c1.y, c1.z);
  ImGui::ColorConvertHSVtoRGB(std::fmod(hue + 0.08f, 1.0f), 0.6f, 0.22f, c2.x, c2.y, c2.z);
  ImU32 a = ImGui::ColorConvertFloat4ToU32(ImVec4(c1.x, c1.y, c1.z, 1)), b = ImGui::ColorConvertFloat4ToU32(ImVec4(c2.x, c2.y, c2.z, 1));
  dl->AddRectFilled(tl(r), br(r), b, radius);
  dl->AddRectFilledMultiColor(ImVec2(r.x + radius * 0.3f, r.y + radius * 0.3f), ImVec2(r.right() - radius * 0.3f, r.bottom() - radius * 0.3f), a, a,
                              b, b);
  std::string initial;
  for (unsigned char ch : name) {
    if (ch == ' ' || ch == '(' || ch == '[') {
      if (!initial.empty()) break;
      continue;
    }
    if (initial.empty() || (ch & 0xC0) == 0x80) initial += char(ch);
    else break;
  }
  if (initial.empty()) initial = "?";
  ImVec2 ts = measure(fontSize, initial.c_str());
  dl->AddText(ImGui::GetFont(), fontSize, ImVec2(r.x + (r.w - ts.x) / 2, r.y + (r.h - ts.y) / 2), IM_COL32(255, 255, 255, 200),
              initial.c_str());
}
}  // namespace

bool UI::thumbImage(ThumbRef& img, ImTextureID* tex, ImVec2* uv0, ImVec2* uv1) {
  if (!img) return false;
  uint64_t t = 0;
  float uv[4];
  if (!d_.renderer->thumbTexture(img.get()->serial, img.get()->px, &t, uv)) return false;
  *tex = ImTextureID(t);
  *uv0 = ImVec2(uv[0], uv[1]);
  *uv1 = ImVec2(uv[2], uv[3]);
  return true;
}

namespace {
// The picture fills r (cropped to its aspect ratio), rounded.
void coverImage(ImDrawList* dl, ImTextureID tex, ImVec2 uv0, ImVec2 uv1, const LRect& r, float radius) {
  float ia = float(RNF_THUMB_WIDTH) / float(RNF_THUMB_HEIGHT), ra = r.w / std::max(1.0f, r.h);
  ImVec2 a = uv0, b = uv1;
  if (ra > ia) {  // wider box: crop top / bottom
    float keep = ia / ra, cut = (1 - keep) / 2 * (uv1.y - uv0.y);
    a.y += cut;
    b.y -= cut;
  } else {
    float keep = ra / ia, cut = (1 - keep) / 2 * (uv1.x - uv0.x);
    a.x += cut;
    b.x -= cut;
  }
  dl->AddImageRounded(ImTextureRef(tex), tl(r), br(r), a, b, IM_COL32(255, 255, 255, 255), radius);
}
}  // namespace

namespace {
constexpr int kFocusHero = -1;
constexpr int kFocusPageStart = -2;  // L / R: the first card of the new page
constexpr int kFocusBar = -3;        // the filter / sort / search row
constexpr int kBarItems = RNF_LIBRARY_FILTER_COUNT + 3;  // filters, sort, search, Add Test Cartridge
constexpr int kBarSort = RNF_LIBRARY_FILTER_COUNT, kBarSearch = RNF_LIBRARY_FILTER_COUNT + 1;
constexpr int kBarTestCart = RNF_LIBRARY_FILTER_COUNT + 2;
constexpr ImU32 kStar = IM_COL32(255, 200, 40, 255);

std::string playTime(double seconds) {
  int64_t m = int64_t(seconds / 60);
  if (m < 60) return TRF("%lld min", {m < 1 ? int64_t(1) : m});
  return TRF("%lld h %lld min", {m / 60, m % 60});
}
}  // namespace

std::vector<const LibraryROM*> UI::libraryRoms() const { return d_.library->arranged(search_); }

const LibraryROM* UI::projectsRom() const {
  for (const LibraryROM& r : d_.library->roms())
    if (r.path == projectsRomPath_) return &r;
  return nullptr;
}

void UI::libraryBarAction(int item) {
  LibraryModel* lib = d_.library;
  if (item < RNF_LIBRARY_FILTER_COUNT) {
    lib->setFilter(rnf_library_filter(item));
    libPage_ = 0;
  } else if (item == kBarSort) {
    lib->setSort(rnf_library_sort((int(lib->sort()) + 1) % RNF_LIBRARY_SORT_COUNT));
    libPage_ = 0;
  } else if (item == kBarSearch) {
    focusSearch_ = true;
    searchShown_ = true;
  } else if (item == kBarTestCart) {
    addTestCartridge(ImGui::GetTime());
  }
}

void UI::addTestCartridge(double now) {
  LibraryModel* lib = d_.library;
  std::string error;
  std::string path = lib->addTestCartridge(&error);
  if (path.empty()) {
    Dialog d;
    d.title = TR("Add Test Cartridge");
    d.message = TRF("Can’t add the Test Cartridge: %@", {error});
    showDialog(std::move(d));
    return;
  }
  // Shown in any case: all games, no search; focused as soon as the scan lists it.
  lib->setFilter(RNF_LIBRARY_FILTER_ALL);
  search_[0] = 0;
  searchShown_ = false;
  libFocusPath_ = path;
  libFocusPathUntil_ = now + 10;
}

void UI::handleLibraryInput(double now, size_t cardCount, size_t pages, bool hero) {
  if (menuOpen_ || popupOpen() || popupLastFrame_ || ImGui::GetIO().WantTextInput || focusSearch_) return;
  auto pressed = [](ImGuiKey k, bool repeat) { return ImGui::IsKeyPressed(k, repeat); };
  const int perPage = 8, cols = 4;
  int dx = 0, dy = 0;
  if (pressed(ImGuiKey_GamepadDpadLeft, true) || pressed(ImGuiKey_LeftArrow, true)) dx = -1;
  if (pressed(ImGuiKey_GamepadDpadRight, true) || pressed(ImGuiKey_RightArrow, true)) dx = 1;
  if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_UpArrow, true)) dy = -1;
  if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_DownArrow, true)) dy = 1;
  if (pressed(ImGuiKey_PageUp, false)) shoulder(-1);
  if (pressed(ImGuiKey_PageDown, false)) shoulder(1);
  int n = int(cardCount);
  if (dx || dy) libMoved_ = true;
  // Top to bottom: the filter / sort / search row (top bar), the Continue card, the game cards.
  if (libFocus_ == kFocusHero) {
    if (dy < 0) libFocus_ = kFocusBar;
    else if (dy > 0 && n > 0) libFocus_ = std::min(n - 1, libPage_ * perPage);
  } else if (libFocus_ == kFocusBar) {
    if (dx) libBar_ = std::clamp(libBar_ + dx, 0, kBarItems - 1);
    if (dy > 0) libFocus_ = hero ? kFocusHero : n > 0 ? std::min(n - 1, libPage_ * perPage) : kFocusBar;
  } else if (n > 0) {
    int page = libFocus_ / perPage, in = libFocus_ % perPage, row = in / cols, col = in % cols;
    if (dx < 0) {
      if (col > 0) --libFocus_;
      else if (page > 0) libFocus_ = std::min(n - 1, (page - 1) * perPage + row * cols + cols - 1);
    } else if (dx > 0) {
      if (col + 1 < cols && libFocus_ + 1 < n) ++libFocus_;
      else if (page + 1 < int(pages)) libFocus_ = std::min(n - 1, (page + 1) * perPage + row * cols);
    } else if (dy < 0) {
      if (row > 0) libFocus_ -= cols;
      else libFocus_ = hero ? kFocusHero : kFocusBar;
    } else if (dy > 0) {
      if (row == 0 && libFocus_ + cols < n && in + cols < perPage) libFocus_ += cols;
    }
    if (libFocus_ >= 0) libPage_ = libFocus_ / perPage;
  } else if (dy < 0) {
    libFocus_ = kFocusBar;
  }
  bool confirm = pressed(confirmKey(), false) || pressed(ImGuiKey_Enter, false) || pressed(ImGuiKey_KeypadEnter, false);
  bool keyX = pressed(kPadX, false) || pressed(ImGuiKey_Delete, false);
  bool keyY = pressed(kPadY, false) || pressed(ImGuiKey_F2, false);
  bool keySort = pressed(ImGuiKey_GamepadBack, false) || pressed(ImGuiKey_Tab, false);
  bool keySearch = (ImGui::GetIO().KeyCtrl && pressed(ImGuiKey_F, false)) || pressed(ImGuiKey_Slash, false);
  bool back = pressed(cancelKey(), false) || pressed(ImGuiKey_Backspace, false);
  const std::vector<const LibraryROM*>& roms = libraryRoms();
  const LibraryROM* focused = libFocus_ >= 0 && libFocus_ < n ? roms[size_t(libFocus_)] : nullptr;
  if (confirm) {
    if (libFocus_ == kFocusHero) {
      d_.app->continueLast();
    } else if (libFocus_ == kFocusBar) {
      libraryBarAction(libBar_);
    } else if (focused) {
      d_.app->playFromLibrary(*focused);
    } else if (n == 0 && d_.library->roms().empty()) {
      d_.library->ensureFolders();
      openFolder(d_.library->romDir());
    }
  } else if (keyX) {
    if (focused) {
      projectsRomPath_ = focused->path;
      openPage("library.projects");
    } else if (d_.library->roms().empty()) {
      d_.library->ensureFolders();
      d_.library->refresh();
    }
  } else if (keyY) {
    if (focused) d_.library->toggleFavorite(*focused);  // the focus stays (a Favorites list may shrink)
    else if (d_.library->roms().empty()) addTestCartridge(now);
  } else if (keySort) {
    libraryBarAction(kBarSort);
  } else if (keySearch) {
    libraryBarAction(kBarSearch);
  } else if (back && search_[0]) {
    search_[0] = 0;
    searchShown_ = false;
    libFocus_ = 0;
    libPage_ = 0;
  }
}

void UI::buildLibraryBar(ImDrawList* dl, const LRect& bar, double now) {
  // Filter chips (All / Favorites / Recent), the sort (cycles) and the search: one row, each a
  // button; the controller reaches it with up from the cards.
  LibraryModel* lib = d_.library;
  const UiMetrics& m = metrics_;
  ImGuiIO& io = ImGui::GetIO();
  float h = bar.h, gap = S(10), fs = m.label();
  struct Item {
    std::string text;
    bool selected;
  };
  std::vector<Item> items;
  static const char* const kFilterIcons[RNF_LIBRARY_FILTER_COUNT] = {"layout-grid", "star-filled", "history"};
  for (int i = 0; i < RNF_LIBRARY_FILTER_COUNT; ++i)
    items.push_back({std::string(icons::glyph(kFilterIcons[i])) + "  " + rnf_library_filter_name(rnf_library_filter(i)),
                     lib->filter() == rnf_library_filter(i)});
  items.push_back({std::string(icons::glyph("list")) + "  " + rnf_library_sort_name(lib->sort()), false});
  std::string searchText = search_[0] ? std::string(search_) : std::string(TR("Search"));
  items.push_back({std::string(icons::glyph("search")) + "  " + searchText, search_[0] != 0});
  items.push_back({std::string(icons::glyph("plus")) + "  " + TR("Add Test Cartridge"), false});
  // Natural widths, shrunk together when the row is narrow (labels are then cut).
  std::vector<float> widths;
  float total = gap * 4 + gap * float(items.size() - 1);
  for (int i = 0; i < int(items.size()); ++i) {
    float w = measure(fs, items[size_t(i)].text.c_str()).x + h * 0.9f;
    if (i == kBarSearch) w = std::max(w, searchShown_ ? S(260) : S(150));
    widths.push_back(w);
    total += w;
  }
  const float gaps = gap * 4 + gap * float(items.size() - 1);
  float k = total > bar.w && total > gaps ? std::max(0.3f, (bar.w - gaps) / (total - gaps)) : 1.0f;
  float x = bar.x;
  for (int i = 0; i < int(items.size()); ++i) {
    float w = widths[size_t(i)] * k;
    if (i == kBarSort || i == kBarTestCart) x += gap * 2;  // a little apart from the filters / the search
    LRect r{x, bar.y, w, h};
    if (i == kBarSearch && searchShown_) {
      buildSearchField(r);
      x += w + gap;
      continue;
    }
    ImGui::SetCursorScreenPos(tl(r));
    ImGui::PushID(1000 + i);
    bool clicked = ImGui::InvisibleButton("##bar", ImVec2(r.w, r.h));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered && (io.MouseDelta.x || io.MouseDelta.y)) libFocus_ = kFocusBar, libBar_ = i, libMoved_ = true;
    bool f = libFocus_ == kFocusBar && libBar_ == i;
    bool sel = items[size_t(i)].selected;
    dl->AddRectFilled(tl(r), br(r), sel ? kBrand : f ? kTileFocus : kTile, h / 2);
    textFit(dl, fs, ImVec2(r.x + h * 0.45f, r.y + (h - fs) / 2), r.w - h * 0.9f, sel || f ? kText : kTextDim, items[size_t(i)].text);
    if (f) drawFocus(r, h / 2, now);
    if (clicked) {
      libFocus_ = kFocusBar;
      libBar_ = i;
      libraryBarAction(i);
    }
    x += w + gap;
  }
}

void UI::buildSearchField(const LRect& r) {
  // The search chip becomes the field (an on-screen keyboard comes with it, ui_osk.cpp).
  ImGui::PopItemFlag();
  ImGui::SetCursorScreenPos(ImVec2(r.x, r.y + (r.h - ImGui::GetFrameHeight()) / 2));
  ImGui::SetNextItemWidth(r.w);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() / 2);
  std::string hint = std::string(icons::glyph("search")) + "  " + TR("Search ROMs");
  ImGui::InputTextWithHint("##search", hint.c_str(), search_, sizeof search_);
  ImGui::PopStyleVar();
  ImGuiID sid = ImGui::GetItemID();
  bool held = ImGui::IsKeyDown(confirmKey()) || ImGui::IsKeyDown(cancelKey()) ||
              ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) || ImGui::IsKeyDown(ImGuiKey_Slash);
  if (focusSearch_ && !held) {
    // On the release of the button that asked for it: the press must not reach the keyboard.
    focusSearch_ = false;
    ImGui::SetFocusID(sid, ImGui::GetCurrentWindow());
    ImGui::ActivateItemByID(sid);
    ImGui::GetCurrentContext()->NavNextActivateFlags |= ImGuiActivateFlags_PreferInput;
  }
  if (ImGui::IsItemDeactivated()) {
    if (!search_[0]) searchShown_ = false;
    libFocus_ = search_[0] ? 0 : kFocusBar;
    libMoved_ = true;
    libPage_ = 0;
  }
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
}

void UI::buildLibrary(double now) {
  LibraryModel* lib = d_.library;
  lib->watch(now);
  ImGuiIO& io = ImGui::GetIO();
  const UiMetrics& m = metrics_;
  const std::vector<const LibraryROM*> roms = libraryRoms();  // a copy: actions below may re-sort
  std::optional<AppModel::ContinueTarget> cont = d_.app->continueTarget();
  bool hero = cont.has_value() && !search_[0];
  LibraryGeometry g = layoutLibrary(m, hero);
  const int perPage = int(g.cards.size());
  int n = int(roms.size());
  int pages = std::max(1, (n + perPage - 1) / perPage);
  // L / R (shoulder) moved the page: focus its first card.
  libPage_ = std::clamp(libPage_, 0, pages - 1);
  if (libFocus_ == kFocusPageStart) libFocus_ = n > 0 ? std::min(n - 1, libPage_ * perPage) : kFocusBar;
  if (!libMoved_) libFocus_ = hero ? kFocusHero : (n > 0 ? 0 : kFocusBar);  // until the user moves (the scan may finish late)
  if (libFocus_ == kFocusHero && !hero) libFocus_ = n > 0 ? 0 : kFocusBar;
  if (libFocus_ >= n) libFocus_ = n > 0 ? n - 1 : kFocusBar;
  if (!libFocusPath_.empty()) {  // Add Test Cartridge: focus it once a scan lists it
    for (int i = 0; i < n; ++i)
      if (LibraryModel::samePath(roms[size_t(i)]->path, libFocusPath_)) {
        libFocus_ = i;
        libMoved_ = true;
        libFocusPath_.clear();
        break;
      }
    if (now > libFocusPathUntil_) libFocusPath_.clear();
  }
  if (libFocus_ >= 0 && n > 0) libPage_ = libFocus_ / perPage;
  handleLibraryInput(now, size_t(n), size_t(pages), hero);
  if (!hasSession() && menuOpen_) return;  // Settings over the library draw everything

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##library", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar();
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(16, 16, 19, 255));
  // A soft brand glow from the top.
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(io.DisplaySize.x, io.DisplaySize.y * 0.5f), IM_COL32(58, 20, 24, 150),
                              IM_COL32(58, 20, 24, 150), IM_COL32(16, 16, 19, 0), IM_COL32(16, 16, 19, 0));
  // Top bar: the name, an update notice, the search field (while searching).
  LRect bar = m.topBar();
  float cy = bar.y + bar.h / 2;
  float x = bar.x;
  dl->AddText(ImGui::GetFont(), m.title(), ImVec2(x, cy - m.title() / 2), kText, "Replay");
  x += measure(m.title(), "Replay").x;
  dl->AddText(ImGui::GetFont(), m.title(), ImVec2(x, cy - m.title() / 2), kBrand, "NES");
  x += measure(m.title(), "NES").x + S(24);
  if (update_.noticeVisible()) {
    std::string t = updateStatusText();
    float w = std::min(measure(m.hint(), t.c_str()).x + S(48), std::max(0.0f, pill_.x - x - S(300)));
    if (w > S(80)) {
      LRect r{x, cy - m.hint() * 0.9f, w, m.hint() * 1.8f};
      buildUpdateNotice(r);
      x += w + S(16);
    }
  }
  // Filter chips, sort and search between the name and the Menu pill.
  if (!lib->roms().empty()) {
    float h = m.label() + S(16);
    buildLibraryBar(dl, LRect{x, cy - h / 2, std::max(0.0f, pill_.x - S(16) - x), h}, now);
  }
  // Empty library: one card saying where the games go.
  if (lib->roms().empty()) {
    const LRect& r = g.emptyCard;
    dl->AddRectFilled(tl(r), br(r), kPanel, m.panelRadius());
    layoutRecord(r);
    icon(dl, "folder-plus", ImVec2(r.x + r.w / 2, r.y + r.h * 0.22f), m.title() * 2.0f, kBrand);
    bool loading = lib->scanning();
    textCentered(dl, m.title(), inset(r, S(24)), r.y + r.h * 0.40f, kText, loading ? TR("Loading…") : TR("Put your games here"));
    textCentered(dl, m.label(), inset(r, S(24)), r.y + r.h * 0.40f + m.title() * 1.5f, kTextDim, Paths::display(lib->romDir()));
    textCentered(dl, m.hint(), inset(r, S(24)), r.y + r.h * 0.40f + m.title() * 1.5f + m.label() * 1.6f, kTextFaint,
                 TR(".nes files; they appear here by themselves"));
    // Buttons: Open Folder (A), Reload (X), Add Test Cartridge (Y).
    float gap = S(16), bw = std::min(S(220), (r.w - S(48) - gap * 2) / 3), bh = m.label() * 2.2f;
    float bx = r.x + (r.w - bw * 3 - gap * 2) / 2;
    static const char* const kIcon[3] = {"folder-open", "refresh", "plus"};
    for (int i = 0; i < 3; ++i) {
      const LRect br_{bx + float(i) * (bw + gap), r.bottom() - bh - S(24), bw, bh};
      ImGui::SetCursorScreenPos(tl(br_));
      ImGui::PushID(i);
      bool clicked = ImGui::InvisibleButton("##emptybtn", ImVec2(br_.w, br_.h));
      ImGui::PopID();
      dl->AddRectFilled(tl(br_), br(br_), i == 0 ? kBrand : kTile, br_.h / 2);
      const char* label = i == 0 ? TR("Open Folder") : i == 1 ? TR("Reload") : TR("Add Test Cartridge");
      std::string t = std::string(icons::glyph(kIcon[i])) + "  " + label;
      textFit(dl, m.label(), ImVec2(br_.x + S(12) + std::max(0.0f, (br_.w - S(24) - measure(m.label(), t.c_str()).x) / 2), br_.y + (br_.h - m.label()) / 2),
              br_.w - S(24), kText, t);
      if (clicked) {
        if (i == 2) {
          addTestCartridge(now);
          continue;
        }
        lib->ensureFolders();
        if (i == 0) openFolder(lib->romDir());
        else lib->refresh();
      }
    }
    if (!lib->folderError().empty()) description_ = lib->folderError();
    prompt({"ui.confirm"}, TR("Open Folder"));
    prompt({"face.west"}, TR("Reload"));
    prompt({"face.north"}, TR("Add Test Cartridge"));
  } else {
    // Continue: the last session (also after a crash), else the latest project.
    if (hero) {
      const LRect& r = g.hero;
      bool f = libFocus_ == kFocusHero;
      ImGui::SetCursorScreenPos(tl(r));
      if (ImGui::InvisibleButton("##hero", ImVec2(r.w, r.h))) d_.app->continueLast();
      if (ImGui::IsItemHovered() && (io.MouseDelta.x || io.MouseDelta.y)) libFocus_ = kFocusHero, libMoved_ = true;
      dl->AddRectFilled(tl(r), br(r), f ? kTileFocus : kTile, m.panelRadius());
      LRect pic{r.x + S(12), r.y + S(12), (r.h - S(24)) * 16.0f / 15.0f, r.h - S(24)};
      const LibraryROM* rom = lib->romWithSHA(cont->romSHA256);
      ThumbRef img = d_.libraryThumbs ? d_.libraryThumbs->forProject(cont->projectPath) : ThumbRef();
      if (!img && d_.libraryThumbs && !cont->romPath.empty()) img = d_.libraryThumbs->forRom(cont->romPath);
      std::string game = rom ? rom->title()
                         : !cont->romPath.empty() ? takeString(rnf_game_title(nullptr, fileName(cont->romPath).c_str()))
                                                  : takeString(rnf_game_title(nullptr, cont->projectName.c_str()));
      ImTextureID tex{};
      ImVec2 uv0, uv1;
      if (img && thumbImage(img, &tex, &uv0, &uv1)) coverImage(dl, tex, uv0, uv1, pic, m.tileRadius());
      else nameTile(dl, pic, game, m.tileRadius(), pic.h * 0.4f);
      float tx = pic.right() + S(24), tw = r.right() - S(24) - tx;
      float ty = r.y + (r.h - (m.hint() + m.title() + m.hint() + S(16))) / 2;
      std::string label = std::string(icons::glyph("player-play-filled")) + "  " + TR("Continue");
      textFit(dl, m.hint() * 1.1f, ImVec2(tx, ty), tw, kBrand, label);
      textFit(dl, m.title(), ImVec2(tx, ty + m.hint() + S(8)), tw, kText, game);
      std::string when = cont->date > 0 ? localDateTime(cont->date) : std::string();
      if (cont->isTemp) when = when.empty() ? std::string(TR("Not saved as a project")) : when + TR(" · ") + TR("Not saved as a project");
      textFit(dl, m.hint() * 1.1f, ImVec2(tx, ty + m.hint() + m.title() + S(16)), tw, kTextDim, when);
      if (f) {
        drawFocus(r, m.panelRadius(), now);
        description_ = TR("Pick up where you left off");
      }
    }
    // Game cards.
    int first = libPage_ * perPage;
    float textH = libraryCardTextHeight(m);
    bool recentView = lib->filter() == RNF_LIBRARY_FILTER_RECENT || lib->sort() == RNF_LIBRARY_SORT_RECENT;
    for (int k = 0; k < perPage && first + k < n; ++k) {
      const LibraryROM& rom = *roms[size_t(first + k)];
      const LRect& r = g.cards[size_t(k)];
      bool f = libFocus_ == first + k;
      ImGui::SetCursorScreenPos(tl(r));
      ImGui::PushID(first + k);
      bool clicked = ImGui::InvisibleButton("##card", ImVec2(r.w, r.h));
      bool hovered = ImGui::IsItemHovered();
      ImGui::PopID();
      if (hovered && (io.MouseDelta.x || io.MouseDelta.y)) libFocus_ = first + k, libMoved_ = true;
      if (clicked) {
        libFocus_ = first + k;
        d_.app->playFromLibrary(rom);
      }
      dl->AddRectFilled(tl(r), br(r), f ? kTileFocus : kTile, m.tileRadius());
      LRect pic{r.x + S(8), r.y + S(8), r.w - S(16), r.h - textH - S(8)};
      std::vector<LibraryProject> projects = lib->projectsFor(rom);
      ThumbRef img = d_.libraryThumbs ? d_.libraryThumbs->forRom(rom.path) : ThumbRef();
      if (!img && d_.libraryThumbs && !projects.empty()) {
        const LibraryProject* lp = &projects.front();
        for (const LibraryProject& p : projects)
          if (p.modified > lp->modified) lp = &p;
        img = d_.libraryThumbs->forProject(lp->path);
      }
      std::string title = rom.title();
      ImTextureID tex{};
      ImVec2 uv0, uv1;
      if (img && thumbImage(img, &tex, &uv0, &uv1)) coverImage(dl, tex, uv0, uv1, pic, m.tileRadius() - S(4));
      else nameTile(dl, pic, title, m.tileRadius() - S(4), pic.h * 0.45f);
      if (lib->isFavorite(rom)) {
        float d = m.label() * 1.7f;
        ImVec2 c(pic.right() - d * 0.5f - S(6), pic.y + d * 0.5f + S(6));
        dl->AddCircleFilled(c, d * 0.5f, IM_COL32(0, 0, 0, 170));
        icon(dl, "star-filled", c, m.label() * 1.05f, kStar);
      }
      float ty = pic.bottom() + S(8);
      textFit(dl, m.label(), ImVec2(r.x + S(12), ty), r.w - S(24), kText, title);
      // Maker · year (the last play while looking at the history).
      std::string sub = rom.byline();
      rnf_library_history_entry h{};
      if (recentView && lib->history(rom, &h)) sub = localDateTime(h.last_played) + TR(" · ") + playTime(h.play_seconds);
      if (sub.empty()) sub = projects.empty() ? std::string(TR("New")) : std::string();
      textFit(dl, m.hint(), ImVec2(r.x + S(12), ty + m.label() * 1.2f), r.w - S(24), kTextDim, sub);
      if (f) {
        drawFocus(r, m.tileRadius(), now);
        std::string d = rom.details();
        description_ = d.empty() ? rom.relativePath : d;
      }
    }
    if (n == 0) {
      std::string empty = search_[0] ? TRF("No ROMs match “%@”", {std::string(search_)})
                           : lib->filter() == RNF_LIBRARY_FILTER_FAVORITES ? std::string(TR("No favorites yet"))
                                                                           : std::string(TR("Nothing played yet"));
      textCentered(dl, m.label(), g.content, g.cards.front().y + S(40), kTextDim, empty);
    }
    // Pages: dots under the grid.
    if (pages > 1) {
      float dot = S(7), gap = S(10), total = float(pages) * dot + float(pages - 1) * gap;
      float dx0 = g.content.x + (g.content.w - total) / 2, dy0 = g.cards.back().bottom() + S(10);
      if (dy0 + dot < m.bottomBar().y)
        for (int p = 0; p < pages; ++p)
          dl->AddCircleFilled(ImVec2(dx0 + float(p) * (dot + gap) + dot / 2, dy0 + dot / 2), dot / 2, p == libPage_ ? kText : kTextFaint);
    }
    if (libFocus_ == kFocusHero) {
      prompt({"ui.confirm"}, TR("Continue"));
    } else if (libFocus_ == kFocusBar) {
      prompt({"ui.confirm"}, TR("Select"));
      if (libBar_ < RNF_LIBRARY_FILTER_COUNT) description_ = rnf_library_filter_name(rnf_library_filter(libBar_));
      else if (libBar_ == kBarSort) description_ = TRF("Sort by %@", {std::string(rnf_library_sort_name(lib->sort()))});
      else if (libBar_ == kBarTestCart)
        description_ = TR("Adds the ReplayNES Test Cartridge (palette, sprites, controllers, rapid-fire meter, sound) to the ROM folder");
      else description_ = TR("Search ROMs");
    } else if (n > 0) {
      const LibraryROM* fr = libFocus_ >= 0 && libFocus_ < n ? roms[size_t(libFocus_)] : nullptr;
      prompt({"ui.confirm"}, TR("Play"));
      if (fr) prompt({"face.north"}, lib->isFavorite(*fr) ? TR("Unfavorite") : TR("Favorite"));
      prompt({"face.west"}, TR("Projects"));
    }
    prompt({"options"}, rnf_library_sort_name(lib->sort()));
    if (pages > 1) prompt({"leftShoulder", "rightShoulder"}, TR("Page"));
    if (!lib->scanError().empty()) description_ = lib->scanError();
  }
  layoutRecord(g.content);
  buildHintBar(m.bottomBar());
  ImGui::PopItemFlag();
  ImGui::End();
}

}  // namespace rnl
