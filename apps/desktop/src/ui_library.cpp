// The library (start screen, docs/design/UI_REDESIGN.md): a "Continue" hero card (the latest
// project: its picture, game name and when it was played - A resumes it) and pages of large game
// cards, four per row (picture of the last session of the game, else a tile made from its name).
// A plays the focused game (a new project), X lists its projects, Y searches (on-screen keyboard),
// L / R turn the pages, the Menu pill (L+R / Esc) opens Settings. An empty library is one card
// saying where the games go. Nothing scrolls.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstring>

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

std::vector<const LibraryROM*> UI::libraryRoms() const {
  std::vector<const LibraryROM*> out;
  for (const LibraryROM& r : d_.library->roms())
    if (librarySearchMatches(search_, r)) out.push_back(&r);
  return out;
}

const LibraryROM* UI::projectsRom() const {
  for (const LibraryROM& r : d_.library->roms())
    if (r.path == projectsRomPath_) return &r;
  return nullptr;
}

void UI::handleLibraryInput(double now, size_t cardCount, size_t pages, bool hero) {
  (void)now;
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
  if (libFocus_ == -1) {
    if (dy > 0 && n > 0) libFocus_ = libPage_ * perPage;
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
      else if (hero) libFocus_ = -1;
    } else if (dy > 0) {
      if (row == 0 && libFocus_ + cols < n && in + cols < perPage) libFocus_ += cols;
    }
    libPage_ = libFocus_ / perPage;
  }
  bool confirm = pressed(ImGuiKey_GamepadFaceDown, false) || pressed(ImGuiKey_Enter, false) || pressed(ImGuiKey_KeypadEnter, false);
  bool keyX = pressed(kPadX, false) || pressed(ImGuiKey_Delete, false);
  bool keyY = pressed(kPadY, false) || pressed(ImGuiKey_F2, false) || (ImGui::GetIO().KeyCtrl && pressed(ImGuiKey_F, false));
  bool back = pressed(ImGuiKey_GamepadFaceRight, false) || pressed(ImGuiKey_Backspace, false);
  std::vector<const LibraryROM*> roms = libraryRoms();
  if (confirm) {
    if (libFocus_ == -1) {
      const LibraryProject* latest = nullptr;
      for (const LibraryProject& p : d_.library->allProjects())
        if (!latest || p.modified > latest->modified) latest = &p;
      if (latest) d_.app->continueProject(latest->path);
    } else if (libFocus_ >= 0 && libFocus_ < n) {
      d_.app->playFromLibrary(*roms[size_t(libFocus_)]);
    } else if (n == 0 && d_.library->roms().empty()) {
      d_.library->ensureFolders();
      openFolder(d_.library->romDir());
    }
  } else if (keyX) {
    if (libFocus_ >= 0 && libFocus_ < n) {
      projectsRomPath_ = roms[size_t(libFocus_)]->path;
      openPage("library.projects");
    } else if (d_.library->roms().empty()) {
      d_.library->ensureFolders();
      d_.library->refresh();
    }
  } else if (keyY) {
    focusSearch_ = true;
    searchShown_ = true;
  } else if (back && search_[0]) {
    search_[0] = 0;
    searchShown_ = false;
    libFocus_ = 0;
    libPage_ = 0;
  }
}

void UI::buildLibrary(double now) {
  LibraryModel* lib = d_.library;
  lib->watch(now);
  ImGuiIO& io = ImGui::GetIO();
  const UiMetrics& m = metrics_;
  std::vector<const LibraryROM*> roms = libraryRoms();
  const LibraryProject* latest = nullptr;
  for (const LibraryProject& p : lib->allProjects())
    if (!latest || p.modified > latest->modified) latest = &p;
  bool hero = latest != nullptr && !search_[0];
  LibraryGeometry g = layoutLibrary(m, hero);
  const int perPage = int(g.cards.size());
  int n = int(roms.size());
  int pages = std::max(1, (n + perPage - 1) / perPage);
  // L / R (shoulder) moved the page: focus its first card.
  libPage_ = std::clamp(libPage_, 0, pages - 1);
  if (libFocus_ == -2) libFocus_ = n > 0 ? std::min(n - 1, libPage_ * perPage) : (hero ? -1 : 0);
  if (!libMoved_ && hero) libFocus_ = -1;  // the Continue card until the user moves (the scan may finish late)
  if (libFocus_ == -1 && !hero) libFocus_ = 0;
  if (libFocus_ >= n) libFocus_ = n > 0 ? n - 1 : (hero ? -1 : 0);
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
  // Top bar: the name, an update notice, the search.
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
  // Search: Y opens it (an on-screen keyboard comes with the field, ui_osk.cpp).
  if (searchShown_ || search_[0]) {
    float sw = std::min(S(360), std::max(S(160), pill_.x - S(24) - x));
    ImGui::SetCursorScreenPos(ImVec2(pill_.x - S(24) - sw, cy - ImGui::GetFrameHeight() / 2));
    ImGui::PopItemFlag();
    ImGui::SetNextItemWidth(sw);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() / 2);
    std::string hint = std::string(icons::glyph("search")) + "  " + TR("Search ROMs");
    ImGui::InputTextWithHint("##search", hint.c_str(), search_, sizeof search_);
    ImGui::PopStyleVar();
    ImGuiID sid = ImGui::GetItemID();
    if (focusSearch_ && !ImGui::IsKeyDown(kPadY)) {
      // On the Y release: the press must not reach the keyboard.
      focusSearch_ = false;
      ImGui::SetFocusID(sid, ImGui::GetCurrentWindow());
      ImGui::ActivateItemByID(sid);
      ImGui::GetCurrentContext()->NavNextActivateFlags |= ImGuiActivateFlags_PreferInput;
    }
    if (ImGui::IsItemDeactivated()) {
      if (!search_[0]) searchShown_ = false;
      libFocus_ = 0;
      libPage_ = 0;
    }
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
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
    // Buttons: Open Folder (A), Reload (X).
    float bw = S(220), bh = m.label() * 2.2f, gap = S(16);
    LRect b1{r.x + r.w / 2 - bw - gap / 2, r.bottom() - bh - S(24), bw, bh}, b2{r.x + r.w / 2 + gap / 2, b1.y, bw, bh};
    for (int i = 0; i < 2; ++i) {
      const LRect& br_ = i == 0 ? b1 : b2;
      ImGui::SetCursorScreenPos(tl(br_));
      ImGui::PushID(i);
      bool clicked = ImGui::InvisibleButton("##emptybtn", ImVec2(br_.w, br_.h));
      ImGui::PopID();
      dl->AddRectFilled(tl(br_), br(br_), i == 0 ? kBrand : kTile, br_.h / 2);
      std::string t = std::string(icons::glyph(i == 0 ? "folder-open" : "refresh")) + "  " + (i == 0 ? TR("Open Folder") : TR("Reload"));
      textCentered(dl, m.label(), br_, br_.y + (br_.h - m.label()) / 2, kText, t);
      if (clicked) {
        lib->ensureFolders();
        if (i == 0) openFolder(lib->romDir());
        else lib->refresh();
      }
    }
    if (!lib->folderError().empty()) description_ = lib->folderError();
    prompt({"face.south"}, TR("Open Folder"));
    prompt({"face.west"}, TR("Reload"));
  } else {
    // Continue.
    if (hero) {
      const LRect& r = g.hero;
      bool f = libFocus_ == -1;
      ImGui::SetCursorScreenPos(tl(r));
      if (ImGui::InvisibleButton("##hero", ImVec2(r.w, r.h))) d_.app->continueProject(latest->path);
      if (ImGui::IsItemHovered() && (io.MouseDelta.x || io.MouseDelta.y)) libFocus_ = -1;
      dl->AddRectFilled(tl(r), br(r), f ? kTileFocus : kTile, m.panelRadius());
      LRect pic{r.x + S(12), r.y + S(12), (r.h - S(24)) * 16.0f / 15.0f, r.h - S(24)};
      ThumbRef img = d_.libraryThumbs ? d_.libraryThumbs->forProject(latest->path) : ThumbRef();
      ImTextureID tex{};
      ImVec2 uv0, uv1;
      if (img && thumbImage(img, &tex, &uv0, &uv1)) coverImage(dl, tex, uv0, uv1, pic, m.tileRadius());
      else nameTile(dl, pic, latest->romName.empty() ? latest->name : latest->romName, m.tileRadius(), pic.h * 0.4f);
      float tx = pic.right() + S(24), tw = r.right() - S(24) - tx;
      float ty = r.y + (r.h - (m.hint() + m.title() + m.hint() + S(16))) / 2;
      std::string cont = std::string(icons::glyph("player-play-filled")) + "  " + TR("Continue");
      textFit(dl, m.hint() * 1.1f, ImVec2(tx, ty), tw, kBrand, cont);
      std::string game = latest->romName.empty() ? latest->name : latest->romName;
      if (game.size() > 4 && (game.compare(game.size() - 4, 4, ".nes") == 0 || game.compare(game.size() - 4, 4, ".NES") == 0))
        game.resize(game.size() - 4);
      textFit(dl, m.title(), ImVec2(tx, ty + m.hint() + S(8)), tw, kText, game);
      textFit(dl, m.hint() * 1.1f, ImVec2(tx, ty + m.hint() + m.title() + S(16)), tw, kTextDim, localDateTime(latest->modified));
      if (f) {
        drawFocus(r, m.panelRadius(), now);
        description_ = TR("Pick up where you left off");
      }
    }
    // Game cards.
    int first = libPage_ * perPage;
    float textH = libraryCardTextHeight(m);
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
      ImTextureID tex{};
      ImVec2 uv0, uv1;
      if (img && thumbImage(img, &tex, &uv0, &uv1)) coverImage(dl, tex, uv0, uv1, pic, m.tileRadius() - S(4));
      else nameTile(dl, pic, rom.name, m.tileRadius() - S(4), pic.h * 0.45f);
      float ty = pic.bottom() + S(8);
      textFit(dl, m.label(), ImVec2(r.x + S(12), ty), r.w - S(24), kText, rom.name);
      // When it was played last (its latest project), or "New".
      double last = 0;
      for (const LibraryProject& p : projects) last = std::max(last, p.modified);
      std::string sub = projects.empty() ? std::string(TR("New")) : localDateTime(last);
      textFit(dl, m.hint(), ImVec2(r.x + S(12), ty + m.label() * 1.2f), r.w - S(24), kTextDim, sub);
      if (f) {
        drawFocus(r, m.tileRadius(), now);
        description_ = rom.relativePath;
      }
    }
    if (n == 0) textCentered(dl, m.label(), g.content, g.cards.front().y + S(40), kTextDim, TRF("No ROMs match “%@”", {std::string(search_)}));
    // Pages: dots under the grid.
    if (pages > 1) {
      float dot = S(7), gap = S(10), total = float(pages) * dot + float(pages - 1) * gap;
      float dx0 = g.content.x + (g.content.w - total) / 2, dy0 = g.cards.back().bottom() + S(10);
      if (dy0 + dot < m.bottomBar().y)
        for (int p = 0; p < pages; ++p)
          dl->AddCircleFilled(ImVec2(dx0 + float(p) * (dot + gap) + dot / 2, dy0 + dot / 2), dot / 2, p == libPage_ ? kText : kTextFaint);
    }
    if (libFocus_ == -1) prompt({"face.south"}, TR("Continue"));
    else if (n > 0) {
      prompt({"face.south"}, TR("Play"));
      prompt({"face.west"}, TR("Projects"));
    }
    prompt({"face.north"}, TR("Search"));
    if (pages > 1) prompt({"leftShoulder", "rightShoulder"}, TR("Page"));
    if (!lib->scanError().empty()) description_ = lib->scanError();
  }
  for (const LRect& c : g.cards) (void)c;
  layoutRecord(g.content);
  buildHintBar(m.bottomBar());
  ImGui::PopItemFlag();
  ImGui::End();
}

}  // namespace rnl
