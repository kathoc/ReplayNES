// The Quick Menu and its pages (docs/design/UI_REDESIGN.md), drawn from the shared core's menu
// model (rnf_menu: tree, focus, back stack, L / R, breadcrumb). Every page is laid out by
// ui_layout.h to fit the screen (nothing scrolls); controller, keyboard, mouse and touch all go
// through the model: D-pad / arrows move, A / Enter confirm, B / Backspace back, X / Y (Delete /
// F2) the hint bar's extras, L / R (PageUp / PageDown) pages.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "app_model.h"
#include "emulation.h"
#include "icons.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "library.h"
#include "pad_nav.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui.h"
#include "ui_theme.h"

namespace rnl {

using namespace theme;

namespace {
const ImU32 kSlotColors[8] = {IM_COL32(255, 149, 0, 255), IM_COL32(10, 132, 255, 255), IM_COL32(48, 209, 88, 255),
                              IM_COL32(255, 55, 95, 255),  IM_COL32(191, 90, 242, 255), IM_COL32(64, 200, 224, 255),
                              IM_COL32(255, 214, 10, 255), IM_COL32(255, 69, 58, 255)};

const char* txt(const char* key) { return !key || !*key ? "" : key[0] == '=' ? key + 1 : TR(key); }

std::string str(char* p) {
  std::string s = p ? p : "";
  rnf_string_free(p);
  return s;
}

void icon(ImDrawList* dl, const char* name, ImVec2 center, float size, ImU32 col) {
  const char* g = icons::glyph(name);
  if (!*g) return;
  ImVec2 ts = measure(size, g);
  dl->AddText(ImGui::GetFont(), size, ImVec2(center.x - ts.x / 2, center.y - ts.y / 2), col, g);
}

// Vertex range [from, end) faded / shifted (open, page change animations).
void animateVertices(ImDrawList* dl, int from, float a, float dy) {
  for (int i = from; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    v.col = alpha(v.col, a);
    v.pos.y += dy;
  }
}
}  // namespace

// ------------------------------------------------------------------ model glue

std::string UI::menuPageId() const {
  if (!rnf_menu_depth(menu_)) return {};
  rnf_menu_page_info pi{};
  rnf_menu_page_get(menu_, rnf_menu_current(menu_), &pi);
  return pi.id;
}

std::string UI::focusedItemId() const {
  rnf_menu_item_info it{};
  if (!rnf_menu_depth(menu_) || !rnf_menu_item_get(menu_, rnf_menu_current(menu_), rnf_menu_focus(menu_), &it)) return {};
  return it.id;
}

size_t UI::listCount(const std::string& page) const {
  if (page == "takes") return hasSession() ? d_.emu->structure().takes.size() : 0;
  if (page == "bookmarks") return hasSession() ? d_.emu->structure().bookmarks.size() + 1 : 1;  // + "Add Bookmark"
  if (page == "controls.keyboard") return rnf_input_action_count();
  if (page == "controls.assign") return rnf_input_assign_choices(0, nullptr, 0);
  if (page == "library.projects") {
    const LibraryROM* r = projectsRom();
    return r ? d_.library->projectsFor(*r).size() + 2 : 0;  // + New Game, Try Without Saving
  }
  return 0;
}

void UI::syncMenuCounts() {
  for (const char* p : {"takes", "bookmarks", "controls.keyboard", "controls.assign", "library.projects"}) {
    int i = rnf_menu_page_find(menu_, p);
    if (i >= 0) rnf_menu_set_count(menu_, size_t(i), listCount(p));
  }
}

bool UI::openPage(const std::string& pageId) {
  int target = rnf_menu_page_find(menu_, pageId.c_str());
  if (target < 0) return false;
  // The natural path: parents through the items that open each page (settings pages hang off
  // the Settings tile; the library's roots are Settings and Projects).
  std::vector<std::string> chain = {pageId};
  for (int guard = 0; guard < 8; ++guard) {
    const std::string& cur = chain.front();
    rnf_menu_page_info cpi{};
    rnf_menu_page_get(menu_, size_t(rnf_menu_page_find(menu_, cur.c_str())), &cpi);
    if (cur == "quick" || cur == "library.projects" || (cpi.group && *cpi.group)) break;
    std::string parent;
    for (size_t p = 0; p < rnf_menu_page_count(menu_) && parent.empty(); ++p) {
      rnf_menu_item_info it{};
      for (size_t i = 0; rnf_menu_item_get(menu_, p, i, &it); ++i)
        if (it.kind == RNF_MENU_ITEM_PAGE && pageId.size() && it.target == cur) {
          rnf_menu_page_info pi{};
          rnf_menu_page_get(menu_, p, &pi);
          parent = pi.id;
          break;
        }
    }
    if (parent.empty()) break;
    chain.insert(chain.begin(), parent);
  }
  rnf_menu_page_info first{};
  rnf_menu_page_get(menu_, size_t(rnf_menu_page_find(menu_, chain.front().c_str())), &first);
  if (hasSession() && std::strcmp(first.id, "quick") != 0 && chain.front() != "library.projects") chain.insert(chain.begin(), "quick");
  if (!hasSession() && chain.front() == "quick") chain.erase(chain.begin());
  if (chain.empty()) return false;
  if (!menuOpen_) setMenu(true);
  rnf_menu_open(menu_, chain.front().c_str());
  for (size_t i = 1; i < chain.size(); ++i) {
    // Focus the item that leads on, so going back lands on it.
    size_t cur = rnf_menu_current(menu_);
    rnf_menu_item_info it{};
    rnf_menu_page_info want{};
    rnf_menu_page_get(menu_, size_t(rnf_menu_page_find(menu_, chain[i].c_str())), &want);
    for (size_t k = 0; rnf_menu_item_get(menu_, cur, k, &it); ++k) {
      if (it.kind != RNF_MENU_ITEM_PAGE) continue;
      rnf_menu_page_info t{};
      rnf_menu_page_get(menu_, size_t(rnf_menu_page_find(menu_, it.target)), &t);
      // The item opening that page, or (Settings pages) the one opening its group.
      if (chain[i] == it.target || (want.group && *want.group && t.group && std::strcmp(t.group, want.group) == 0))
        rnf_menu_set_focus(menu_, k);
    }
    rnf_menu_push(menu_, chain[i].c_str());
  }
  pageChangedAt_ = -1;
  return true;
}

void UI::menuEvent(rnf_menu_event e, int dir) {
  switch (e) {
    case RNF_MENU_EVENT_NONE:
    case RNF_MENU_EVENT_MOVED: break;
    case RNF_MENU_EVENT_PUSHED:
    case RNF_MENU_EVENT_POPPED:
    case RNF_MENU_EVENT_SWITCHED: pageChangedAt_ = -1; break;
    case RNF_MENU_EVENT_CLOSE:
      if (hasSession()) closeMenuAndResume();
      else setMenu(false);
      break;
    case RNF_MENU_EVENT_ACTIVATE: {
      rnf_menu_page_info pi{};
      rnf_menu_page_get(menu_, rnf_menu_current(menu_), &pi);
      if (pi.kind == RNF_MENU_PAGE_LIST || pi.kind == RNF_MENU_PAGE_CARDS) activateRow(pi.id, rnf_menu_focus(menu_));
      else if (pi.kind == RNF_MENU_PAGE_CUSTOM) activateRow(pi.id, size_t(diagramFocus_));
      else activateItem(focusedItemId());
      break;
    }
    case RNF_MENU_EVENT_ADJUST: {
      rnf_menu_page_info pi{};
      rnf_menu_page_get(menu_, rnf_menu_current(menu_), &pi);
      if (pi.kind == RNF_MENU_PAGE_SETTINGS) adjustItem(focusedItemId(), dir);
      break;
    }
  }
}

// ------------------------------------------------------------------ input

void UI::handleMenuInput(double now) {
  if (popupOpen() || popupLastFrame_ || ImGui::GetFrameCount() == menuInputFrame_) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || !capturingAction_.empty()) return;
  auto pressed = [](ImGuiKey k, bool repeat) { return ImGui::IsKeyPressed(k, repeat); };
  int dx = 0, dy = 0;
  if (pressed(ImGuiKey_GamepadDpadLeft, true) || pressed(ImGuiKey_LeftArrow, true)) dx = -1;
  if (pressed(ImGuiKey_GamepadDpadRight, true) || pressed(ImGuiKey_RightArrow, true)) dx = 1;
  if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_UpArrow, true)) dy = -1;
  if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_DownArrow, true)) dy = 1;
  bool confirm = pressed(confirmKey(), false) || pressed(ImGuiKey_Enter, false) || pressed(ImGuiKey_KeypadEnter, false);
  bool back = pressed(cancelKey(), false) || pressed(ImGuiKey_Backspace, false);
  bool keyX = pressed(kPadX, false) || pressed(ImGuiKey_Delete, false);
  bool keyY = pressed(kPadY, false) || pressed(ImGuiKey_F2, false);
  if (pressed(ImGuiKey_PageUp, false)) shoulder(-1);
  if (pressed(ImGuiKey_PageDown, false)) shoulder(1);
  if (!menuOpen_) return;
  // L2 / R2 held: rewind / fast-forward the game behind the menu, as assigned for play.
  if (hasSession())
    for (bool left : {true, false}) {
      if (!ImGui::IsKeyDown(left ? ImGuiKey_GamepadL2 : ImGuiKey_GamepadR2)) continue;
      std::string a = triggerAction(left);
      if (a == "hk.rewind") d_.emu->setRewindHeld(true);
      else if (a == "hk.fast_forward") d_.emu->setFastForwardHeld(true);
    }
  rnf_menu_page_info pi{};
  rnf_menu_page_get(menu_, rnf_menu_current(menu_), &pi);
  if (pi.kind == RNF_MENU_PAGE_CUSTOM) {
    // The controller diagram: the D-pad moves between the buttons on the picture.
    if (dx || dy) {
      int best = -1;
      float bestScore = 1e9f;
      if (diagramFocus_ < int(diagramCenters_.size())) {
        ImVec2 c = diagramCenters_[size_t(diagramFocus_)];
        for (size_t i = 0; i < diagramCenters_.size(); ++i) {
          if (int(i) == diagramFocus_) continue;
          float vx = diagramCenters_[i].x - c.x, vy = diagramCenters_[i].y - c.y;
          float along = vx * float(dx) + vy * float(dy), across = std::fabs(vx * float(dy)) + std::fabs(vy * float(dx));
          if (along <= 1) continue;
          float score = along + across * 2.2f;
          if (score < bestScore) bestScore = score, best = int(i);
        }
      }
      if (best >= 0) diagramFocus_ = best;
    }
    if (confirm) menuEvent(RNF_MENU_EVENT_ACTIVATE, 0);
    else if (back) menuEvent(rnf_menu_back(menu_), 0);
    else if (keyX) rowAction(pi.id, 0, 'x');
    else if (keyY) rowAction(pi.id, 0, 'y');
    return;
  }
  if (dx || dy) {
    int dir = 0;
    rnf_menu_event e = rnf_menu_move(menu_, dx, dy, &dir);  // sets dir: evaluate before passing it on
    menuEvent(e, dir);
  }
  if (confirm) menuEvent(rnf_menu_confirm(menu_), 0);
  else if (back) menuEvent(rnf_menu_back(menu_), 0);
  else if (keyX) rowAction(pi.id, rnf_menu_focus(menu_), 'x');
  else if (keyY) rowAction(pi.id, rnf_menu_focus(menu_), 'y');
  (void)now;
}

// ------------------------------------------------------------------ drawing helpers

void UI::drawFocus(const LRect& r, float radius, double now) {
  std::string key = menuPageId() + "#" + std::to_string(rnf_menu_focus(menu_)) + "#" + std::to_string(diagramFocus_);
  if (key != focusKey_) {
    bool first = focusKey_.empty() || focusKey_.substr(0, focusKey_.find('#')) != menuPageId();
    float t = easeOut((now - focusChangedAt_) / kAnim);
    focusFrom_ = first ? r : lerp(focusFrom_, focusTo_, t);
    focusTo_ = r;
    focusChangedAt_ = now;
    focusKey_ = key;
  } else if (std::fabs(focusTo_.x - r.x) + std::fabs(focusTo_.y - r.y) + std::fabs(focusTo_.w - r.w) > 0.5f) {
    focusTo_ = focusFrom_ = r;  // the layout moved (window size): no animation
  }
  LRect cur = lerp(focusFrom_, focusTo_, easeOut((now - focusChangedAt_) / kAnim));
  focusRing(ImGui::GetWindowDrawList(), cur, radius, metrics_.ring(), S(3));
}

bool UI::itemHit(const char* id, const LRect& r, size_t index) {
  ImGui::SetCursorScreenPos(tl(r));
  ImGui::PushID(id);
  ImGui::PushID(int(index));
  bool clicked = ImGui::InvisibleButton("##hit", ImVec2(std::max(1.0f, r.w), std::max(1.0f, r.h)));
  bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  ImGui::PopID();
  ImGuiIO& io = ImGui::GetIO();
  if (hovered && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || clicked)) rnf_menu_set_focus(menu_, index);
  return clicked;
}

// ------------------------------------------------------------------ frame

void UI::buildMenu(double now) {
  ImGuiIO& io = ImGui::GetIO();
  const UiMetrics& m = metrics_;
  syncMenuCounts();
  handleMenuInput(now);
  if (!menuOpen_ || !rnf_menu_depth(menu_)) return;
  size_t page = rnf_menu_current(menu_);
  rnf_menu_page_info pi{};
  rnf_menu_page_get(menu_, page, &pi);
  bool header = pi.group && *pi.group;
  size_t count = pi.count, reserve = 0;
  if (pi.kind == RNF_MENU_PAGE_LIST) {
    size_t first = rnf_menu_sheet(menu_) * RNF_MENU_MAX_ITEMS;
    count = pi.count > first ? std::min<size_t>(RNF_MENU_MAX_ITEMS, pi.count - first) : 0;
    bool sheets = rnf_menu_sheet_count(menu_) > 1;
    // A list's line on top: its sheets (L / R) and / or what the column is.
    std::string id = pi.id;
    header = sheets || id == "controls.keyboard" || id == "controls.assign" || id == "takes" || id == "library.projects";
    if (sheets) reserve = RNF_MENU_MAX_ITEMS;
  }
  PageGeometry g = layoutPage(m, pi.kind, count, pi.columns, header, reserve);
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##quickmenu", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float open = easeOut((now - menuOpenedAt_) / kAnim);
  // The game stays visible behind the menu, dimmed (cheap on every GPU); the library has its own
  // backdrop.
  if (hasSession()) dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, alpha(kDim, open));
  else dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(16, 16, 19, 255));
  int vtx0 = dl->VtxBuffer.Size;
  buildBreadcrumb(m.topBar());
  layoutRecord(g.content);
  switch (pi.kind) {
    case RNF_MENU_PAGE_TILES: buildTilesPage(g, page, now); break;
    case RNF_MENU_PAGE_SETTINGS: buildSettingsPage(g, page, now); break;
    case RNF_MENU_PAGE_CARDS: buildCardsPage(g, now); break;
    case RNF_MENU_PAGE_LIST: buildListPage(g, page, now); break;
    case RNF_MENU_PAGE_CUSTOM: buildControllerPage(g, now); break;
  }
  buildHintBar(m.bottomBar());
  float t = easeOut((now - std::max(menuOpenedAt_, pageChangedAt_)) / kAnim);
  if (t < 1) animateVertices(dl, vtx0, t, (1 - t) * S(12));
  ImGui::PopItemFlag();
  ImGui::End();
}

void UI::buildBreadcrumb(const LRect& bar) {
  rnf_menu_crumb c[8];
  size_t n = std::min<size_t>(8, rnf_menu_breadcrumb(menu_, c, 8));
  crumbRects_.clear();
  if (n == 0) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const UiMetrics& m = metrics_;
  float fs = m.title(), x = bar.x, cy = bar.y + bar.h / 2;
  float maxX = pill_.w > 0 ? pill_.x - S(16) : bar.right();
  icon(dl, c[0].icon, ImVec2(x + fs * 0.55f, cy), fs, kText);
  float segX = x;  // the first segment includes the icon
  x += fs * 1.3f;
  int select = -1;
  for (size_t i = 0; i < n; ++i) {
    std::string t = txt(c[i].label);
    bool last = i + 1 == n;
    float w = measure(fs, t.c_str()).x;
    bool hovered = false;
    if (!last && x < maxX) {
      // An ancestor segment: a click / tap goes back to that level (rnf_menu_crumb_select).
      float hw = std::min(x + w, maxX) - segX;
      crumbRects_.push_back(LRect{segX, bar.y, std::max(1.0f, hw), std::max(1.0f, bar.h)});
      ImGui::SetCursorScreenPos(ImVec2(segX, bar.y));
      ImGui::PushID("crumb");
      ImGui::PushID(int(i));
      if (ImGui::InvisibleButton("##crumb", ImVec2(std::max(1.0f, hw), std::max(1.0f, bar.h)))) select = int(i);
      hovered = ImGui::IsItemHovered();
      ImGui::PopID();
      ImGui::PopID();
      if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddLine(ImVec2(x, cy + fs * 0.62f), ImVec2(std::min(x + w, maxX), cy + fs * 0.62f), kTextDim, std::max(1.0f, S(1.5f)));
      }
    }
    textFit(dl, fs, ImVec2(x, cy - fs / 2), std::max(0.0f, maxX - x), last || hovered ? kText : kTextDim, t);
    x += w;
    if (!last) {
      icon(dl, "chevron-right", ImVec2(x + fs * 0.6f, cy), fs * 0.8f, kTextFaint);
      x += fs * 1.2f;
      segX = x;
    }
  }
  if (select >= 0) menuEvent(rnf_menu_crumb_select(menu_, size_t(select)), 0);
}

// One row of large tiles: the Quick Menu, Retry, Share, Game.
void UI::buildTilesPage(const PageGeometry& g, size_t page, double now) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const UiMetrics& m = metrics_;
  size_t focus = rnf_menu_focus(menu_);
  rnf_menu_item_info it{};
  for (size_t i = 0; i < g.items.size() && rnf_menu_item_get(menu_, page, i, &it); ++i) {
    const LRect& r = g.items[i];
    bool f = i == focus, en = itemEnabled(it.id);
    if (itemHit(it.id, r, i)) menuEvent(rnf_menu_confirm(menu_), 0);
    float lift = f ? S(4) * easeOut((now - focusChangedAt_) / kAnim) : 0;
    LRect rr{r.x, r.y - lift, r.w, r.h};
    dl->AddRectFilled(tl(rr), br(rr), f ? kTileFocus : kTile, m.tileRadius());
    ImU32 fg = en ? kText : kTextFaint;
    float is = std::min(rr.w * 0.30f, rr.h * 0.34f);
    icon(dl, it.icon, ImVec2(rr.x + rr.w / 2, rr.y + rr.h * 0.40f), is, it.kind == RNF_MENU_ITEM_RESUME ? kBrand : fg);
    textCentered(dl, m.label(), inset(rr, S(10)), rr.y + rr.h * 0.70f, fg, txt(it.label));
    if (it.kind == RNF_MENU_ITEM_TOGGLE) {
      // State under the label ("On" in the brand color).
      bool on = itemOn(it.id);
      textCentered(dl, m.hint(), inset(rr, S(10)), rr.y + rr.h * 0.70f + m.label() * 1.25f, on ? kBrand : kTextFaint,
                   on ? TR("On") : TR("Off"));
    }
    if (f) {
      drawFocus(rr, m.tileRadius(), now);
      description_ = txt(it.description);
    }
  }
  bool root = rnf_menu_depth(menu_) == 1;
  if (focusedItemId() == "resume") prompt({"ui.confirm"}, TR("Resume"));
  else prompt({"ui.confirm"}, TR("Select"));
  prompt({"ui.cancel"}, root && hasSession() ? TR("Resume") : TR("Back"));
}

// The tabs of a settings group / the sheet of a list.
void UI::buildPageHeader(const PageGeometry& g, size_t page) {
  if (g.header.h <= 0) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const UiMetrics& m = metrics_;
  rnf_controller_family fam = promptFamily();
  rnf_menu_page_info pi{};
  rnf_menu_page_get(menu_, page, &pi);
  float gh = m.hint() * 1.5f, cy = g.header.y + g.header.h / 2;
  if (pi.group && *pi.group) {
    // L  Display  Controls  Sound  System  R
    std::vector<std::pair<size_t, std::string>> tabs;
    for (size_t p = 0; p < rnf_menu_page_count(menu_); ++p) {
      rnf_menu_page_info q{};
      rnf_menu_page_get(menu_, p, &q);
      if (q.group && std::strcmp(q.group, pi.group) == 0) tabs.push_back({p, txt(q.title)});
    }
    float lw = glyph(dl, ImVec2(), "leftShoulder", fam, gh, false), rw = glyph(dl, ImVec2(), "rightShoulder", fam, gh, false);
    float total = lw + rw + S(16) * 2;
    for (auto& t : tabs) total += measure(m.label(), t.second.c_str()).x + S(28);
    float x = g.header.x + (g.header.w - total) / 2;
    x += glyph(dl, ImVec2(x, cy - gh / 2), "leftShoulder", fam, gh) + S(16);
    for (size_t i = 0; i < tabs.size(); ++i) {
      float w = measure(m.label(), tabs[i].second.c_str()).x + S(28);
      LRect r{x, g.header.y, w, g.header.h};
      bool cur = tabs[i].first == page;
      if (cur) dl->AddRectFilled(tl(r), br(r), IM_COL32(255, 255, 255, 30), r.h / 2);
      dl->AddText(ImGui::GetFont(), m.label(), ImVec2(x + S(14), cy - m.label() / 2), cur ? kText : kTextDim, tabs[i].second.c_str());
      // Mouse / touch: a tab switches to that page.
      ImGui::SetCursorScreenPos(tl(r));
      ImGui::PushID(int(tabs[i].first));
      if (ImGui::InvisibleButton("##tab", ImVec2(r.w, r.h)) && !cur) {
        int steps = 0;
        size_t at = 0;
        for (size_t k = 0; k < tabs.size(); ++k)
          if (tabs[k].first == page) at = k;
        steps = int(i) - int(at);
        for (int s = 0; s < std::abs(steps); ++s) rnf_menu_switch(menu_, steps < 0 ? -1 : 1);
        pageChangedAt_ = -1;
      }
      ImGui::PopID();
      x += w;
    }
    x += S(16);
    glyph(dl, ImVec2(x, cy - gh / 2), "rightShoulder", fam, gh);
    return;
  }
  // LIST: "2 / 3" with L / R, right-aligned; the page's own column title on the left.
  size_t sheets = rnf_menu_sheet_count(menu_);
  std::string left;
  std::string id = pi.id;
  if (id == "controls.keyboard") left = TR("Action");
  else if (id == "controls.assign" && !assignElement_.empty()) {
    int slot = 0;
    rnf_input_controller_slot(assignElement_.c_str(), &slot);
    std::string el = assignElement_.substr(assignElement_.find(':') + 1);
    left = str(rnf_input_element_title(el.c_str(), diagramFamily(slot), nullptr));
  }
  else if (id == "takes") left = TRF("Undo steps available: %lld", {(long long)d_.emu->status().undoDepth});
  else if (id == "library.projects" && projectsRom()) left = projectsRom()->name;
  if (!left.empty()) textFit(dl, m.hint() * 1.1f, ImVec2(g.header.x + S(6), cy - m.hint() * 0.55f), g.header.w * 0.6f, kTextDim, left);
  if (sheets > 1) {
    std::string n = std::to_string(rnf_menu_sheet(menu_) + 1) + " / " + std::to_string(sheets);
    float rw = glyph(dl, ImVec2(), "rightShoulder", fam, gh, false), nw = measure(m.label(), n.c_str()).x;
    float x = g.header.right() - rw;
    glyph(dl, ImVec2(x, cy - gh / 2), "rightShoulder", fam, gh);
    x -= nw + S(10);
    dl->AddText(ImGui::GetFont(), m.label(), ImVec2(x, cy - m.label() / 2), kText, n.c_str());
    x -= glyph(dl, ImVec2(), "leftShoulder", fam, gh, false) + S(10);
    glyph(dl, ImVec2(x, cy - gh / 2), "leftShoulder", fam, gh);
  }
}

// Rows of a settings page: icon, label, the value on the right.
void UI::buildSettingsPage(const PageGeometry& g, size_t page, double now) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const UiMetrics& m = metrics_;
  dl->AddRectFilled(tl(g.panel), br(g.panel), kPanel, m.panelRadius());
  layoutRecord(g.panel);
  buildPageHeader(g, page);
  size_t focus = rnf_menu_focus(menu_);
  rnf_menu_item_info it{};
  bool adjustable = false;
  for (size_t i = 0; i < g.items.size() && rnf_menu_item_get(menu_, page, i, &it); ++i) {
    const LRect& r = g.items[i];
    bool f = i == focus, en = itemEnabled(it.id);
    ImGuiIO& io = ImGui::GetIO();
    if (itemHit(it.id, r, i) && en) {
      // A click on the value side of a choice / slider steps it that way; elsewhere it confirms.
      if (it.kind == RNF_MENU_ITEM_CHOICE || it.kind == RNF_MENU_ITEM_SLIDER) {
        float valueStart = r.right() - S(200);
        adjustItem(it.id, io.MousePos.x < valueStart ? 1 : io.MousePos.x < valueStart + S(100) ? -1 : 1);
      } else {
        menuEvent(rnf_menu_confirm(menu_), 0);
      }
    }
    dl->AddRectFilled(tl(r), br(r), f ? kRowFocus : kRow, m.tileRadius());
    ImU32 fg = en ? kText : kTextFaint;
    float cy = r.y + r.h / 2;
    icon(dl, it.icon, ImVec2(r.x + S(28), cy), m.label() * 1.15f, en ? kTextDim : kTextFaint);
    float labelX = r.x + S(56);
    float valueRight = r.right() - S(18);
    // The value.
    float valueW = 0;
    switch (it.kind) {
      case RNF_MENU_ITEM_TOGGLE: {
        bool on = itemOn(it.id);
        float h = m.label() * 1.25f, w = h * 1.8f;
        LRect sw{valueRight - w, cy - h / 2, w, h};
        dl->AddRectFilled(tl(sw), br(sw), on ? (en ? kBrand : alpha(kBrand, 0.4f)) : IM_COL32(255, 255, 255, 46), h / 2);
        float kx = on ? sw.right() - h / 2 : sw.x + h / 2;
        dl->AddCircleFilled(ImVec2(kx, cy), h / 2 - S(3), en ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 200, 160));
        valueW = w;
        break;
      }
      case RNF_MENU_ITEM_CHOICE: {
        if (std::strcmp(it.id, "controls.confirm") == 0) {
          // The button pair as glyphs of the controller family: "(A) Confirm  (B) Back".
          bool south = d_.settings->southConfirm;
          rnf_controller_family fam = promptFamily();
          float gh = m.label() * 1.25f, aw = m.label() * 0.9f;
          const std::pair<const char*, const char*> parts[] = {{rnf_ui_confirm_element(south), TR("Confirm")},
                                                               {rnf_ui_cancel_element(south), TR("Back")}};
          float w = aw * 2 + S(12);
          for (const auto& [el, verb] : parts) w += keycap(dl, ImVec2(), gh, padGlyph(fam, el), true, false) + S(6) +
                                                   measure(m.label(), verb).x + S(14);
          float x = valueRight - w;
          icon(dl, "chevron-left", ImVec2(x + aw / 2, cy), m.label(), f ? kText : kTextFaint);
          x += aw + S(6);
          for (const auto& [el, verb] : parts) {
            x += keycap(dl, ImVec2(x, cy - gh / 2), gh, padGlyph(fam, el), true) + S(6);
            dl->AddText(ImGui::GetFont(), m.label(), ImVec2(x, cy - m.label() / 2), fg, verb);
            x += measure(m.label(), verb).x + S(14);
          }
          icon(dl, "chevron-right", ImVec2(valueRight - aw / 2, cy), m.label(), f ? kText : kTextFaint);
          valueW = w;
          break;
        }
        std::string v = itemValue(it.id);
        float vw = measure(m.label(), v.c_str()).x;
        float aw = m.label() * 0.9f;
        float x = valueRight - aw;
        icon(dl, "chevron-right", ImVec2(x + aw / 2, cy), m.label(), f ? kText : kTextFaint);
        x -= vw + S(6);
        dl->AddText(ImGui::GetFont(), m.label(), ImVec2(x, cy - m.label() / 2), fg, v.c_str());
        x -= aw + S(6);
        icon(dl, "chevron-left", ImVec2(x + aw / 2, cy), m.label(), f ? kText : kTextFaint);
        valueW = valueRight - x;
        break;
      }
      case RNF_MENU_ITEM_SLIDER: {
        std::string v = itemValue(it.id);
        float bw = S(150), bh = S(6);
        float fr = std::clamp(itemFraction(it.id), 0.0f, 1.0f);
        LRect bar{valueRight - bw, cy - bh / 2, bw, bh};
        dl->AddRectFilled(tl(bar), br(bar), IM_COL32(255, 255, 255, 46), bh / 2);
        dl->AddRectFilled(tl(bar), ImVec2(bar.x + bw * fr, bar.bottom()), en ? kBrand : kTextFaint, bh / 2);
        dl->AddCircleFilled(ImVec2(bar.x + bw * fr, cy), S(9), en ? kText : kTextFaint);
        float vw = measure(m.label(), v.c_str()).x;
        dl->AddText(ImGui::GetFont(), m.label(), ImVec2(bar.x - S(16) - vw, cy - m.label() / 2), fg, v.c_str());
        valueW = bw + S(16) + vw;
        break;
      }
      case RNF_MENU_ITEM_PAGE:
        icon(dl, "chevron-right", ImVec2(valueRight - m.label() * 0.45f, cy), m.label() * 1.1f, kTextDim);
        valueW = m.label();
        break;
      case RNF_MENU_ITEM_ACTION:
      case RNF_MENU_ITEM_INFO:
      case RNF_MENU_ITEM_RESUME: {
        std::string v = itemValue(it.id);
        if (v.empty()) break;
        float maxW = (r.w - S(56)) * 0.58f;
        float vw = std::min(maxW, measure(m.label(), v.c_str()).x);
        textFit(dl, m.label(), ImVec2(valueRight - vw, cy - m.label() / 2), maxW, it.kind == RNF_MENU_ITEM_INFO ? kTextDim : fg, v);
        valueW = vw;
        break;
      }
    }
    textFit(dl, m.label(), ImVec2(labelX, cy - m.label() / 2), std::max(0.0f, valueRight - valueW - S(16) - labelX), fg, txt(it.label));
    if (f) {
      drawFocus(r, m.tileRadius(), now);
      description_ = txt(it.description);
      adjustable = it.kind == RNF_MENU_ITEM_TOGGLE || it.kind == RNF_MENU_ITEM_CHOICE || it.kind == RNF_MENU_ITEM_SLIDER;
      if (adjustable && en) prompt({"ui.confirm", "dpad.lr"}, TR("Change"));
      else if (it.kind != RNF_MENU_ITEM_INFO && en) prompt({"ui.confirm"}, it.kind == RNF_MENU_ITEM_PAGE ? TR("Open") : TR("Select"));
    }
  }
  prompt({"ui.cancel"}, TR("Back"));
}

// The Practice page: eight A/B slots as 4 x 2 cards (thumbnail of A, name, length).
void UI::buildCardsPage(const PageGeometry& g, double now) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const UiMetrics& m = metrics_;
  const SessionStructure& ss = d_.emu->structure();
  const EmuStatus& st = d_.emu->status();
  size_t focus = rnf_menu_focus(menu_);
  float textH = cardTextHeight(m), pad = S(8);
  for (size_t i = 0; i < g.items.size(); ++i) {
    const LRect& r = g.items[i];
    const SlotInfo* s = i < ss.slots.size() ? &ss.slots[i] : nullptr;
    bool f = i == focus, active = st.practicing && st.practiceSlot == int(i);
    if (itemHit("card", r, i)) menuEvent(rnf_menu_confirm(menu_), 0);
    dl->AddRectFilled(tl(r), br(r), f ? kTileFocus : kTile, m.tileRadius());
    LRect thumb{r.x + pad, r.y + pad, r.w - 2 * pad, r.h - textH - pad};
    ImU32 color = kSlotColors[i % 8];
    bool hasA = s && s->hasA;
    if (hasA) {
      bool drawn = false;
      if (s->hasTakeFrame) {
        ThumbRef img = d_.thumbs->imageBefore(s->takeFrame, 60 * 60 * 10);
        ImTextureID tex{};
        ImVec2 uv0, uv1;
        if (img && thumbImage(img, &tex, &uv0, &uv1)) {
          dl->AddImageRounded(ImTextureRef(tex), tl(thumb), br(thumb), uv0, uv1, IM_COL32(255, 255, 255, 255), m.tileRadius() - pad / 2);
          drawn = true;
        }
      }
      if (!drawn) dl->AddRectFilled(tl(thumb), br(thumb), alpha(color, 0.22f), m.tileRadius() - pad / 2);
    } else {
      dl->AddRectFilled(tl(thumb), br(thumb), IM_COL32(255, 255, 255, 10), m.tileRadius() - pad / 2);
      icon(dl, "plus", ImVec2(thumb.x + thumb.w / 2, thumb.y + thumb.h / 2), thumb.h * 0.32f, f ? kText : kTextFaint);
    }
    // Slot number in its timeline color.
    float bs = m.hint() * 1.7f;
    ImVec2 bc(thumb.x + bs * 0.5f + S(6), thumb.y + bs * 0.5f + S(6));
    dl->AddCircleFilled(bc, bs / 2, color);
    std::string n = std::to_string(i + 1);
    ImVec2 nts = measure(m.hint(), n.c_str());
    dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(bc.x - nts.x / 2, bc.y - nts.y / 2), IM_COL32(0, 0, 0, 255), n.c_str());
    if (active) {
      std::string t = TR("Practicing");
      ImVec2 ts = measure(m.hint(), t.c_str());
      ImVec2 p(thumb.right() - ts.x - S(14), thumb.y + S(6));
      dl->AddRectFilled(p, ImVec2(p.x + ts.x + S(10), p.y + ts.y + S(4)), IM_COL32(255, 149, 0, 230), S(6));
      dl->AddText(ImGui::GetFont(), m.hint(), ImVec2(p.x + S(5), p.y + S(2)), IM_COL32(0, 0, 0, 255), t.c_str());
    }
    // Name + length.
    float ty = thumb.bottom() + S(8);
    std::string name = hasA ? s->displayName() : std::string(TR("Set A Here"));
    textFit(dl, m.label(), ImVec2(r.x + S(12), ty), r.w - S(24), hasA ? kText : kTextDim, name);
    std::string len = !hasA ? std::string() : s->hasB ? timecode(s->length) : std::string(TR("No B yet"));
    if (!len.empty()) textFit(dl, m.hint(), ImVec2(r.x + S(12), ty + m.label() * 1.2f), r.w - S(24), kTextDim, len);
    if (f) drawFocus(r, m.tileRadius(), now);
  }
  // Hints for the focused card.
  const SlotInfo* s = focus < ss.slots.size() ? &ss.slots[focus] : nullptr;
  bool hasA = s && s->hasA, active = st.practicing && st.practiceSlot == int(focus);
  if (!hasA) {
    prompt({"ui.confirm"}, TR("Set A Here"));
    description_ = TR("A = the start of the section: the current position");
  } else if (!s->hasB) {
    prompt({"ui.confirm"}, TR("Set B Here"));
    description_ = TR("B = the end you reach by playing on from A");
  } else {
    prompt({"ui.confirm"}, TR("Practice"));
    description_ = TR("Repeats A to B; nothing is recorded");
  }
  if (hasA) prompt({"face.north"}, TR("Rename"));
  if (active) prompt({"face.west"}, TR("Stop"));
  else if (hasA) prompt({"face.west"}, TR("Clear"));
  prompt({"ui.cancel"}, TR("Back"));
}

// Rows of user data, six per sheet: takes, bookmarks, key bindings, a game's projects.
void UI::buildListPage(const PageGeometry& g, size_t page, double now) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const UiMetrics& m = metrics_;
  dl->AddRectFilled(tl(g.panel), br(g.panel), kPanel, m.panelRadius());
  layoutRecord(g.panel);
  buildPageHeader(g, page);
  rnf_menu_page_info pi{};
  rnf_menu_page_get(menu_, page, &pi);
  std::string id = pi.id;
  size_t first = rnf_menu_sheet(menu_) * RNF_MENU_MAX_ITEMS, focus = rnf_menu_focus(menu_);
  const EmuStatus& st = d_.emu->status();
  const SessionStructure* ss = hasSession() ? &d_.emu->structure() : nullptr;
  std::vector<LibraryProject> projects;
  if (id == "library.projects" && projectsRom()) projects = d_.library->projectsFor(*projectsRom());
  if (pi.count == 0) {
    std::string empty = id == "takes" ? TR("No takes yet") : TR("Nothing here yet");
    textCentered(dl, m.label(), g.panel, g.panel.y + g.panel.h / 2 - m.label() / 2, kTextDim, empty);
  }
  for (size_t k = 0; k < g.items.size(); ++k) {
    size_t row = first + k;
    const LRect& r = g.items[k];
    bool f = row == focus;
    if (itemHit(id.c_str(), r, row)) menuEvent(rnf_menu_confirm(menu_), 0);
    dl->AddRectFilled(tl(r), br(r), f ? kRowFocus : kRow, m.tileRadius());
    float cy = r.y + r.h / 2;
    const char* ic = "";
    std::string label, value, sub;
    ImU32 labelCol = kText;
    if (id == "takes" && ss && row < ss->takes.size()) {
      const TakeInfo& t = ss->takes[row];
      ic = "git-branch";
      label = TRF("Take #%llu", {(unsigned long long)t.id});
      if (t.isActive) sub = TR("Current");
      else if (t.parentId) sub = TRF("from #%llu at %@", {(unsigned long long)t.parentId, timecode(t.branchFrame)});
      value = timecode(t.length);
      if (f) description_ = t.isActive ? TR("The take you are on") : TR("A: switch to this take");
    } else if (id == "bookmarks") {
      if (row == 0) {
        ic = "plus";
        label = TR("Add Bookmark");
        if (f) description_ = TR("Marks the current position");
      } else if (ss && row - 1 < ss->bookmarks.size()) {
        const BookmarkInfo& b = ss->bookmarks[row - 1];
        ic = "bookmark";
        label = b.name;
        value = timecode(b.frame);
        if (!b.onActiveTake) sub = TR("other take");
        if (f) description_ = TR("A: jump there");
      }
    } else if (id == "controls.keyboard") {
      rnf_action_info a{};
      if (rnf_input_action_get(row, &a)) {
        ic = a.group == RNF_GROUP_HOTKEY ? "bolt" : "keyboard";
        label = str(rnf_input_action_label(a.id));
        if (a.group == RNF_GROUP_PLAYER2) label = "2P " + label;
        else if (a.group == RNF_GROUP_PLAYER1) label = "1P " + label;
        if (capturingAction_ == a.id) {
          value = TR("Press a key…");
        } else {
          for (const rnf_binding& b : d_.input->bindings())
            if (std::strcmp(b.action, a.id) == 0 && std::strncmp(b.input, "kb:", 3) == 0) {
              std::string n = str(rnf_input_display_name(RNF_KEYBOARD_SDL, b.input, nullptr));
              std::string kp = TRF("Key %@", {std::string()});  // "Key " prefix of the display name
              if (!kp.empty() && n.compare(0, kp.size(), kp) == 0) n = n.substr(kp.size());
              value += (value.empty() ? "" : " · ") + n;
            }
          if (value.empty()) value = "—";
        }
        if (f) description_ = TR("A: set a key · X: clear");
      }
    } else if (id == "controls.assign") {
      int slot = 0;
      rnf_input_controller_slot(assignElement_.c_str(), &slot);
      std::vector<const char*> choices(rnf_input_assign_choices(slot, nullptr, 0));
      rnf_input_assign_choices(slot, choices.data(), choices.size());
      if (row < choices.size()) {
        std::string a = choices[row];
        rnf_action_info ai{};
        for (size_t i = 0; rnf_input_action_get(i, &ai); ++i)
          if (a == ai.id) break;
        if (a.empty()) {
          ic = "x";
          label = TR("None");
        } else {
          ic = ai.group == RNF_GROUP_HOTKEY ? "bolt" : "device-gamepad";
          label = str(rnf_input_action_label(a.c_str()));
          if (ai.group == RNF_GROUP_PLAYER2) label = "2P " + label;
          else if (ai.group == RNF_GROUP_PLAYER1) label = "1P " + label;
        }
        const std::vector<rnf_binding>& b = d_.input->bindings();
        std::string el = assignElement_.substr(assignElement_.find(':') + 1);
        rnf_list* acts = rnf_input_element_actions(b.data(), b.size(), el.c_str(), slot);
        bool on = a.empty() ? rnf_list_count(acts) == 0 : false;
        for (size_t j = 0; j < rnf_list_count(acts); ++j) on = on || a == rnf_list_a(acts, j);
        rnf_list_free(acts);
        if (on) value = "\xE2\x9C\x93";  // ✓ the action now
        if (f) description_ = ai.group == RNF_GROUP_HOTKEY && !a.empty() ? TR("Hotkeys (not recorded)") : std::string();
      }
    } else if (id == "library.projects") {
      if (row == 0) {
        ic = "player-play";
        label = TR("New Game");
        if (f) description_ = TR("Starts a new project (saved automatically)");
      } else if (row == 1) {
        ic = "eye";
        label = TR("Try Without Saving");
        if (f) description_ = TR("Plays without making a project");
      } else if (row - 2 < projects.size()) {
        const LibraryProject& p = projects[row - 2];
        ic = "folder";
        label = p.name;
        value = localDateTime(p.modified);
        if (f) description_ = TR("A: continue this project");
      }
    }
    icon(dl, ic, ImVec2(r.x + S(28), cy), m.label() * 1.15f, kTextDim);
    float labelX = r.x + S(56), right = r.right() - S(18);
    float vw = value.empty() ? 0 : std::min(r.w * 0.45f, measure(m.label(), value.c_str()).x);
    if (!value.empty())
      textFit(dl, m.label(), ImVec2(right - vw, cy - m.label() / 2), r.w * 0.45f,
              capturingAction_.size() && f ? kBrand : kTextDim, value);
    float lw = std::max(0.0f, right - vw - S(20) - labelX);
    bool cut = textFit(dl, m.label(), ImVec2(labelX, cy - m.label() / 2), lw, labelCol, label);
    if (!sub.empty() && !cut) {
      float x = labelX + measure(m.label(), label.c_str()).x + S(12);
      textFit(dl, m.hint(), ImVec2(x, cy - m.hint() / 2), std::max(0.0f, right - vw - S(20) - x), kTextFaint, sub);
    }
    if (f) drawFocus(r, m.tileRadius(), now);
  }
  // Hints.
  if (id == "takes") prompt({"ui.confirm"}, TR("Switch"));
  else if (id == "bookmarks") {
    prompt({"ui.confirm"}, focus == 0 ? TR("Add") : TR("Go"));
    if (focus > 0) {
      prompt({"face.north"}, TR("Rename"));
      prompt({"face.west"}, TR("Delete"));
    }
  } else if (id == "controls.assign") {
    prompt({"ui.confirm"}, TR("Assign"));
  } else if (id == "controls.keyboard") {
    prompt({"ui.confirm"}, TR("Set Key"));
    prompt({"face.west"}, TR("Clear"));
  } else if (id == "library.projects") {
    prompt({"ui.confirm"}, TR("Play"));
  }
  prompt({"ui.cancel"}, TR("Back"));
  (void)st;
}

// Settings > Controls > Controller: the diagram of the pad, press A on a button to change it.
void UI::buildControllerPage(const PageGeometry& g, double now) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const UiMetrics& m = metrics_;
  dl->AddRectFilled(tl(g.panel), br(g.panel), kPanel, m.panelRadius());
  layoutRecord(g.panel);
  int slot = std::clamp(d_.settings->diagramSlot, 0, InputRouter::kSlots - 1);
  const PadInfo& pad = d_.input->pad(slot);
  rnf_controller_family fam = diagramFamily(slot);
  std::string title = TRF("Pad %lld", {slot + 1}) + "  ·  " +
                      (pad.pad ? pad.name : std::string(TR("Not Connected")) + "  ·  " + str(rnf_controller_family_title(fam)));
  textFit(dl, m.label(), ImVec2(g.panel.x + S(20), g.panel.y + S(16)), g.panel.w - S(40), kText, title);
  LRect area{g.panel.x + S(20), g.panel.y + S(16) + m.label() * 1.8f, g.panel.w - S(40), 0};
  area.h = g.panel.bottom() - area.y - S(16);
  buildDiagram(fam, slot, area, now);
  prompt({"ui.confirm"}, TR("Change"));
  prompt({"face.west"}, TR("Next Pad"));
  prompt({"face.north"}, TR("Reset"));
  prompt({"ui.cancel"}, TR("Back"));
}

// ------------------------------------------------------------------ actions

void UI::activateRow(const std::string& page, size_t row) {
  EmulationController* emu = d_.emu;
  if (page == "practice") {
    const SessionStructure& ss = emu->structure();
    if (row >= ss.slots.size()) return;
    const SlotInfo& s = ss.slots[row];
    int slot = int(row);
    d_.settings->timelineSlot = slot;
    if (!s.hasA) {
      emu->timelineMarkA(slot);
    } else if (!s.hasB) {
      emu->timelineMarkB(slot);
    } else {
      setMenu(false);
      emu->startPractice(slot);
      if (emu->paused()) emu->togglePause();
    }
    changed();
  } else if (page == "takes") {
    const SessionStructure& ss = emu->structure();
    if (row < ss.takes.size() && !ss.takes[row].isActive) {
      emu->activateTake(ss.takes[row].id);
      setMenu(false);  // paused at the switch: the seek bar shows where
    }
  } else if (page == "bookmarks") {
    const SessionStructure& ss = emu->structure();
    if (row == 0) {
      emu->addBookmark();
    } else if (row - 1 < ss.bookmarks.size()) {
      emu->gotoBookmark(ss.bookmarks[row - 1].id);
      if (!emu->status().practicing) setMenu(false);
    }
  } else if (page == "controls.keyboard") {
    rnf_action_info a{};
    if (!rnf_input_action_get(row, &a)) return;
    capturingAction_ = a.id;
    std::string action = a.id;
    d_.input->beginCapture(
        [this, action](const std::string& id) {
          capturingAction_.clear();
          // The press that ended the wait (a key, or B / Esc cancelling) is not a menu press too.
          menuInputFrame_ = ImGui::GetFrameCount() + 1;
          if (id.empty()) return;
          // The key replaces the action's keys (controller buttons stay: they have the diagram).
          std::vector<std::string> old;
          for (const rnf_binding& b : d_.input->bindings())
            if (action == b.action && std::strncmp(b.input, "kb:", 3) == 0) old.push_back(b.input);
          for (const std::string& o : old) d_.input->unbind(o, action);
          d_.input->bind(id, action);
        },
        true);
  } else if (page == "library.projects") {
    const LibraryROM* rom = projectsRom();
    if (!rom) return;
    LibraryROM r = *rom;
    std::vector<LibraryProject> projects = d_.library->projectsFor(r);
    setMenu(false);
    if (row == 0) d_.app->playFromLibrary(r);
    else if (row == 1) d_.app->tryRom(r.path);
    else if (row - 2 < projects.size()) d_.app->continueProject(projects[row - 2].path);
  } else if (page == "controls.controller") {
    if (diagramFocus_ >= 0 && size_t(diagramFocus_) < diagramIds_.size()) openAssign(diagramIds_[size_t(diagramFocus_)]);
  } else if (page == "controls.assign") {
    int slot = 0;
    rnf_input_controller_slot(assignElement_.c_str(), &slot);
    std::vector<const char*> choices(rnf_input_assign_choices(slot, nullptr, 0));
    rnf_input_assign_choices(slot, choices.data(), choices.size());
    if (row >= choices.size() || assignElement_.empty()) return;
    d_.input->setAssignment(assignElement_, choices[row]);
    menuEvent(rnf_menu_back(menu_), 0);  // back to the picture, on the same button
  }
}

void UI::rowAction(const std::string& page, size_t row, char key) {
  EmulationController* emu = d_.emu;
  if (page == "practice") {
    const SessionStructure& ss = emu->structure();
    if (row >= ss.slots.size() || !ss.slots[row].hasA) return;
    int idx = int(row);
    if (key == 'y') {
      askRename(TR("Section Name"), ss.slots[row].name, [emu, idx](const std::string& n) { emu->practiceRename(idx, n); });
    } else if (emu->status().practicing && emu->status().practiceSlot == idx) {
      emu->stopPractice();
    } else {
      emu->practiceClear(idx);
      changed();
    }
  } else if (page == "bookmarks") {
    const SessionStructure& ss = emu->structure();
    if (row == 0 || row - 1 >= ss.bookmarks.size()) return;
    const BookmarkInfo& b = ss.bookmarks[row - 1];
    uint64_t id = b.id;
    if (key == 'y') askRename(TR("Bookmark Name"), b.name, [emu, id](const std::string& n) { emu->renameBookmark(id, n); });
    else emu->removeBookmark(id);
  } else if (page == "controls.keyboard" && key == 'x') {
    rnf_action_info a{};
    if (!rnf_input_action_get(row, &a)) return;
    std::vector<std::string> keys;
    for (const rnf_binding& b : d_.input->bindings())
      if (std::strcmp(b.action, a.id) == 0 && std::strncmp(b.input, "kb:", 3) == 0) keys.push_back(b.input);
    for (const std::string& k : keys) d_.input->unbind(k, a.id);
  } else if (page == "controls.controller") {
    int slot = std::clamp(d_.settings->diagramSlot, 0, InputRouter::kSlots - 1);
    if (key == 'x') {
      d_.settings->diagramSlot = (slot + 1) % InputRouter::kSlots;
      diagramFocus_ = 0;
      changed();
    } else {
      Dialog d;
      d.title = TRF("Reset Pad %lld to Defaults", {slot + 1});
      d.buttons = {TR("Reset"), TR("Cancel")};
      d.destructiveIndex = 0;
      d.cancelIndex = 1;
      d.onResult = [this, slot](int b, bool) {
        if (b == 0) d_.input->resetController(slot);
      };
      showDialog(std::move(d));
    }
  }
}

}  // namespace rnl
