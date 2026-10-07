// Menu pages: Takes, Bookmarks, Practice, Library (start screen), Controls Guide.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL3/SDL.h>

#include <algorithm>
#include <map>

#include "app_model.h"
#include "emulation.h"
#include "imgui_internal.h"
#include "input_router.h"
#include "l10n.h"
#include "library.h"
#include "pad_nav.h"
#include "paths.h"
#include "platform/platform.h"
#include "settings.h"
#include "ui.h"
#include "ui_widgets.h"

namespace rnl {

namespace {
void heading(const char* text, float size) {
  ImGui::PushFont(nullptr, size);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
}
void wrapped(const char* text) {
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
}
void wrappedDisabled(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  wrapped(text);
  ImGui::PopStyleColor();
}
}  // namespace

// ------------------------------------------------------------------ takes

void UI::buildTakes() {
  const EmuStatus& st = d_.emu->status();
  EmulationController* emu = d_.emu;
  const SessionStructure& ss = emu->structure();
  heading(TRF("Current take #%llu", {(unsigned long long)st.activeTake}).c_str(), S(26));
  ImGui::TextDisabled("%s", TRF("Length %@ (%llu frames) · Takes: %lld",
                                {timecode(st.takeLength), (unsigned long long)st.takeLength, (long long)st.takeCount})
                                .c_str());
  ImGui::Spacing();
  ImGui::BeginDisabled(st.practicing);
  if (ImGui::Button(TR("Re-record from Here"))) {
    emu->rerecordHere();
    setMenu(false);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(st.undoDepth == 0);
  if (ImGui::Button(TR("Back to Previous Take"))) emu->undoTake();
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(TR("Reset Project…"))) d_.app->resetProjectPrompt();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", TRF("Undo steps available: %lld", {(long long)st.undoDepth}).c_str());
  wrappedDisabled(TR("Each time you re-record, a new take is created and the old continuation is kept. Usually “Back to "
                     "Previous Take” is all you need."));
  ImGui::Spacing();
  std::map<uint64_t, const TakeInfo*> byId;
  for (const TakeInfo& t : ss.takes) byId[t.id] = &t;
  auto depth = [&](const TakeInfo& t) {
    int d = 0;
    uint64_t p = t.parentId;
    while (p != 0 && d < 64) {
      auto it = byId.find(p);
      if (it == byId.end()) break;
      ++d;
      p = it->second->parentId;
    }
    return d;
  };
  if (ImGui::BeginTable("##takes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(TR("Take"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn(TR("Branched From"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn(TR("Length"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(280));
    ImGui::TableHeadersRow();
    for (const TakeInfo& t : ss.takes) {
      ImGui::PushID(int(t.id));
      ImGui::TableNextRow(0, S(44));
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      std::string indent(size_t(depth(t)) * 2, ' ');
      ImGui::Text("%s%s#%llu", indent.c_str(), t.parentId ? "└ " : "", (unsigned long long)t.id);
      if (t.isActive) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.4f, 1), "%s", TR("Active"));
      }
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (t.parentId == 0) ImGui::TextDisabled("—");
      else ImGui::Text("#%llu @ %s", (unsigned long long)t.parentId, timecode(t.branchFrame).c_str());
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%s (%lluf)", timecode(t.length).c_str(), (unsigned long long)t.length);
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(t.isActive || st.practicing);
      if (ImGui::Button(TR("Switch to This Take"))) emu->activateTake(t.id);
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}

// ------------------------------------------------------------------ bookmarks

void UI::buildBookmarks() {
  const EmuStatus& st = d_.emu->status();
  EmulationController* emu = d_.emu;
  heading(TR("Bookmarks"), S(26));
  ImGui::BeginDisabled(st.practicing);
  if (ImGui::Button(TR("Add Bookmark"))) emu->addBookmark();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", TR("Add a bookmark at the current position"));
  ImGui::Spacing();
  const SessionStructure& ss = emu->structure();
  if (ss.bookmarks.empty()) wrappedDisabled(TR("No bookmarks yet. Press B to add one."));
  const BookmarkInfo* focused = nullptr;  // the row with the focus: Y renames it
  for (const BookmarkInfo& b : ss.bookmarks) {
    auto noteFocus = [&] {
      if (ImGui::IsItemFocused()) focused = &b;
    };
    ImGui::PushID(int(b.id));
    ImGui::Separator();
    ImGui::TextUnformatted(b.name.c_str());
    std::string info = timecode(b.frame) + " · f" + std::to_string(b.frame) + (b.onActiveTake ? "" : TR(" · other take"));
    ImGui::TextDisabled("%s", info.c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(460) + ImGui::GetCursorPosX() * 0);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - S(470)));
    ImGui::BeginDisabled(st.practicing);
    if (ImGui::Button(TR("Rewind to Here"))) {
      emu->gotoBookmark(b.id);
    }
    noteFocus();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(TR("Rename…"))) {
      uint64_t id = b.id;
      askRename(TR("Bookmark Name"), b.name, [emu, id](const std::string& n) { emu->renameBookmark(id, n); });
    }
    noteFocus();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1));
    if (ImGui::Button(TR("Delete"))) emu->removeBookmark(b.id);
    noteFocus();
    ImGui::PopStyleColor();
    ImGui::PopID();
  }
  // Y: rename the focused bookmark (also "Rename…" on its row).
  if (focused) {
    renamePrompt_ = TR("Rename");
    if (!ImGui::GetIO().WantTextInput && !rename_ && ImGui::IsKeyPressed(kPadY, false)) {
      uint64_t id = focused->id;
      askRename(TR("Bookmark Name"), focused->name, [emu, id](const std::string& n) { emu->renameBookmark(id, n); });
    }
  }
}

// ------------------------------------------------------------------ practice

void UI::buildPracticeTab() {
  const EmuStatus& st = d_.emu->status();
  heading(TR("Practice Mode (A/B Repeat)"), S(26));
  if (st.practicing) {
    if (ImGui::Button(TR("Stop Practicing"))) d_.emu->stopPractice();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", TR("At B it pauses briefly, returns to A and repeats. Nothing is recorded."));
  } else {
    wrappedDisabled(TR("A = start of the section, B = the end you reach by playing on from A. Practice a section as often "
                       "as you like (nothing is recorded)."));
  }
  ImGui::Spacing();
  if (!st.practicing) {
    // The section the hub's timeline edits with X (Set A) / Y (Set B).
    int& sel = d_.settings->timelineSlot;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Section set on the timeline (X: Set A, Y: Set B)"));
    const SessionStructure& ss = d_.emu->structure();
    for (const SlotInfo& slot : ss.slots) {
      ImGui::SameLine();
      if (ImGui::RadioButton((std::to_string(slot.index + 1) + "##tlslot").c_str(), sel == slot.index)) {
        sel = slot.index;
        changed();
      }
    }
    ImGui::Spacing();
  }
  // Y: rename the focused section (also "Rename…" on its row).
  int focused = buildPracticeRows(true);
  const SessionStructure& ss = d_.emu->structure();
  if (focused >= 0 && focused < int(ss.slots.size()) && ss.slots[size_t(focused)].hasA) {
    renamePrompt_ = TR("Rename");
    if (!ImGui::GetIO().WantTextInput && !rename_ && ImGui::IsKeyPressed(kPadY, false)) {
      EmulationController* emu = d_.emu;
      const SlotInfo& s = ss.slots[size_t(focused)];
      int idx = s.index;
      askRename(TR("Section Name"), s.name, [emu, idx](const std::string& n) { emu->practiceRename(idx, n); });
    }
  }
}

// ------------------------------------------------------------------ library

void UI::buildLibrary(double now) {
  LibraryModel* lib = d_.library;
  lib->watch(now);
  buildUpdateNotice();  // in-app update waiting / downloading (ui_update.cpp)
  float h = ImGui::GetContentRegionAvail().y;
  // Header: search (Y), reload, open folder.
  bool noPopup = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && dialogs_.empty() && !chooser_;
  if (noPopup && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(kPadY, false)) focusSearch_ = true;
  ImGui::SetNextItemWidth(S(360));
  ImGui::InputTextWithHint("##search", TR("Search ROMs"), search_, sizeof search_);
  // Y released: start typing in the search field (an on-screen keyboard comes with it, ui_osk.cpp).
  // On the release: the press must not reach the keyboard.
  if (focusSearch_ && !ImGui::IsKeyDown(kPadY)) {
    focusSearch_ = false;
    ImGuiID sid = ImGui::GetItemID();
    ImGui::SetFocusID(sid, ImGui::GetCurrentWindow());
    ImGui::ActivateItemByID(sid);
    ImGui::GetCurrentContext()->NavNextActivateFlags |= ImGuiActivateFlags_PreferInput;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Reload"))) lib->refresh();
  ImGui::SameLine();
  if (ImGui::Button(TR("Open Folder"))) {
    lib->ensureFolders();
    openFolder(lib->romDir());
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoNavOverride))
    ImGui::SetTooltip("%s", kWindows ? TR("Opens the ROM folder in Explorer") : TR("Opens the ROM folder in the file manager (Desktop Mode)"));
  if (lib->scanning()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("Loading…"));
  }
  if (!lib->folderError().empty()) {
    ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "%s", lib->folderError().c_str());
    ImGui::SameLine();
    if (ImGui::Button(TR("Retry"))) {
      lib->ensureFolders();
      lib->refresh();
    }
  } else if (!lib->scanError().empty()) {
    ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "%s", lib->scanError().c_str());
  }
  std::vector<const LibraryROM*> filtered;
  for (const LibraryROM& r : lib->roms())
    if (librarySearchMatches(search_, r)) filtered.push_back(&r);
  const LibraryROM* selected = nullptr;
  const LibraryROM* playNow = nullptr;  // chosen this frame (played after the list is drawn)
  const LibraryROM* focusedRom = nullptr;
  for (const LibraryROM* r : filtered)
    if (r->path == selectedRom_) selected = r;
  if (!selected && !filtered.empty()) {
    selected = filtered.front();
    selectedRom_ = selected->path;
  }
  float footer = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
  float listH = std::max(S(200), ImGui::GetContentRegionAvail().y - footer);
  float listW = std::min(S(520), ImGui::GetContentRegionAvail().x * 0.45f);
  (void)h;
  // ROM list.
  ImGui::BeginChild("##roms", ImVec2(listW, listH), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
  if (lib->roms().empty()) {
    ImGui::Spacing();
    heading(lib->scanning() ? TR("Loading…") : TR("No ROMs"), S(24));
    wrappedDisabled(TR("Put .nes files in the ROM folder and they appear here."));
    ImGui::Spacing();
    wrappedDisabled(TRF("ROM folder: %@", {Paths::display(lib->romDir())}).c_str());
    wrappedDisabled(kWindows ? TR("Copy them into the ROM folder (Open Folder shows it in Explorer), then press Reload.")
                             : TR("On Steam Deck: copy them in Desktop Mode (Dolphin), or over the network, then press Reload."));
  } else if (filtered.empty()) {
    ImGui::TextDisabled("%s", TRF("No ROMs match “%@”", {std::string(search_)}).c_str());
  }
  for (const LibraryROM* r : filtered) {
    ImGui::PushID(r->path.c_str());
    size_t projects = lib->projectsFor(*r).size();
    bool isSel = r == selected;
    if (focusFirst_ && isSel) {
      ImGui::SetKeyboardFocusHere();
      ImGui::SetNavCursorVisible(true);
      focusFirst_ = false;
    }
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool sub = r->relativePath.find('/') != std::string::npos;
    float rowH = sub ? S(56) : S(40);
    if (ImGui::Selectable("##rom", isSel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH))) {
      selectedRom_ = r->path;
      // Double click, or A / Enter: play (a new project). A single click selects.
      if (ImGui::IsMouseDoubleClicked(0) || !ImGui::GetIO().MouseReleased[0]) playNow = r;
    }
    if (ImGui::IsItemFocused()) {
      focusedRom = r;
      if (!isSel && ImGui::GetIO().NavVisible) selectedRom_ = r->path;  // the detail follows the focus
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(p.x + S(8), p.y + S(8)), IM_COL32(235, 238, 245, 255), r->name.c_str());
    if (sub) dl->AddText(nullptr, S(16), ImVec2(p.x + S(8), p.y + S(34)), IM_COL32(150, 155, 165, 255), r->relativePath.c_str());
    if (projects > 0) {
      std::string n = std::to_string(projects);
      float x = p.x + ImGui::GetContentRegionAvail().x - S(36);
      dl->AddRectFilled(ImVec2(x, p.y + S(9)), ImVec2(x + S(30), p.y + S(31)), IM_COL32(70, 110, 180, 200), S(11));
      dl->AddText(nullptr, S(16), ImVec2(x + S(15) - ImGui::CalcTextSize(n.c_str()).x * 0.35f, p.y + S(11)), IM_COL32(255, 255, 255, 255), n.c_str());
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (focusFirst_ && filtered.empty()) focusFirst_ = false;
  // X on a ROM: continue its latest project.
  std::vector<LibraryProject> focusedProjects;
  if (focusedRom) focusedProjects = lib->projectsFor(*focusedRom);
  std::string continuePath;
  if (!focusedProjects.empty()) {
    const LibraryProject* latest = &focusedProjects.front();
    for (const LibraryProject& p : focusedProjects)
      if (p.modified > latest->modified) latest = &p;
    continuePath = latest->path;
  }
  if (focusedRom) {
    prompt({"face.south"}, TR("Play"));
    if (!continuePath.empty()) prompt({"face.west"}, TR("Continue"));
  }
  prompt({"face.north"}, TR("Search ROMs"));
  ImGui::SameLine();
  // Detail.
  ImGui::BeginChild("##detail", ImVec2(0, listH), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
  if (selected) {
    const LibraryROM& r = *selected;
    ImGui::PushFont(nullptr, S(26));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(r.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::TextDisabled("%s", r.relativePath.c_str());
    char size[32];
    std::snprintf(size, sizeof size, "%.1f KB", double(r.size) / 1024.0);
    if (!r.sha256.empty()) ImGui::TextDisabled("%s   %s", size, TRF("SHA-256 %@…", {r.sha256.substr(0, 12)}).c_str());
    else ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "%s   %s", size, TR("Can’t compute SHA-256"));
    ImGui::Spacing();
    if (focusPlay_) {
      ImGui::SetKeyboardFocusHere();
      ImGui::SetNavCursorVisible(true);
      focusPlay_ = false;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.85f, 1));
    if (ImGui::Button((std::string("▶  ") + TR("Play")).c_str(), ImVec2(S(220), S(54)))) d_.app->playFromLibrary(r);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoNavOverride))
      ImGui::SetTooltip("%s", TR("Creates a new project and starts right away (autosaved in the Projects folder)"));
    ImGui::SameLine();
    if (ImGui::Button(TR("Try Without a Project"), ImVec2(0, S(54)))) d_.app->tryRom(r.path);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoNavOverride))
      ImGui::SetTooltip("%s", TR("Plays without creating a project (stored temporarily; you can save it later)"));
    ImGui::Spacing();
    ImGui::Separator();
    heading(TR("Projects for This ROM"), S(22));
    std::vector<LibraryProject> projects = lib->projectsFor(r);
    if (projects.empty()) wrappedDisabled(TR("None yet. Start with “Play” and it is saved automatically."));
    for (const LibraryProject& p : projects) {
      ImGui::PushID(p.path.c_str());
      if (ImGui::Button(TR("Continue"), ImVec2(S(170), S(44)))) d_.app->continueProject(p.path);
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::TextUnformatted(p.name.c_str());
      ImGui::TextDisabled("%s", TRF("Last saved %@", {localDateTime(p.modified)}).c_str());
      ImGui::EndGroup();
      ImGui::PopID();
    }
  } else {
    heading(lib->roms().empty() ? TR("Add some ROMs") : TR("Choose a ROM"), S(24));
    wrappedDisabled(TR("Choose a ROM on the left to start playing right away. Open earlier projects with “Continue”."));
  }
  ImGui::EndChild();
  ImGui::TextDisabled("%s", TRF("ROMs: %@   Projects (autosaved): %@", {Paths::display(lib->romDir()), Paths::display(lib->projectsDir())}).c_str());
  if (noPopup && !continuePath.empty() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(kPadX, false)) {
    d_.app->continueProject(continuePath);
  } else if (playNow) {
    d_.app->playFromLibrary(*playNow);
  }
}

// ------------------------------------------------------------------ guide

void UI::buildGuide() {
  heading(TR("Controls Guide"), S(28));
  auto table = [&](const char* title, const std::vector<std::pair<std::string, std::string>>& rows) {
    ImGui::Spacing();
    heading(title, S(22));
    if (ImGui::BeginTable(title, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 2.0f);
      for (const auto& r : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // Focusable rows: the page scrolls with the D-pad.
        ImGui::Selectable(r.first.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(r.second.c_str());
        ImGui::PopTextWrapPos();
      }
      ImGui::EndTable();
    }
  };
  wrappedDisabled(TR("Steam Deck / controller (defaults; can be changed in Settings)"));
  table(TR("Controller"), {
                              {TR("D-pad / Left Stick"), TR("Move")},
                              {TR("B / A (Steam Deck, Xbox)"), TR("NES A / B (Pro Controller A / B, Xbox B / A, PS ○ / ✕)")},
                              {TR("Y / X (Steam Deck, Xbox)"), TR("Turbo A / Turbo B (Pro Controller X / Y, Xbox Y / X, PS △ / □)")},
                              {TR("Menu (≡) / View (⧉)"), "START / SELECT"},
                              {TR("L2 (hold)"), TR("Rewind")},
                              {TR("R2 (hold)"), TR("Fast-forward (recorded range only; pauses at the end)")},
                              {"R1", TR("Pause and open the menu (timeline, back to the library, settings, …)")},
                              {"L1", TR("Slow 1/2 ⇔ normal speed")},
                              {TR("R3 (right stick click)"), TR("Open the menu / back to the game")},
                          });
  table(TR("In the menu"), {
                               {TR("D-pad / Left Stick"), TR("Move the focus (the orange frame)")},
                               {"A", TR("Choose")},
                               {"B", TR("Back (on the menu: resume play)")},
                               {TR("R1 / Menu (≡)"), TR("Resume play (on a page: back to the menu)")},
                               {TR("View (⧉)"), TR("Controls Guide on / off")},
                               {"L1 / R1", TR("Settings tabs (other pages: previous / next page)")},
                               {"Y", TR("Rename the focused bookmark / practice section (also “Rename…” on its row)")},
                               {TR("Right Stick"), TR("Scroll the page")},
                               {TR("L2 / R2 (hold)"), TR("Rewind / Fast-forward")},
                           });
  table(TR("Timeline (move the focus up to it)"), {
                               {TR("D-pad ← / →"), TR("Move the playhead (hold = faster; at the end of the recording: record one more frame)")},
                               {"L1 / R1", TR("Previous / Next Bookmark (or 5 s)")},
                               {"X / Y", TR("Set A / Set B of the section chosen on the Practice page")},
                               {"A", TR("Play from Here")},
                           });
  table(TR("Library"), {
                           {"A", TR("Play (creates a new project)")},
                           {"X", TR("Continue the latest project of the ROM")},
                           {"Y", TR("Search (on-screen keyboard)")},
                           {TR("Menu (≡)"), TR("Settings")},
                       });
  table(TR("On-screen keyboard (text fields: search, names)"), {
                                                                 {TR("D-pad / Left Stick"), TR("Move between the keys")},
                                                                 {"A", TR("Type the key")},
                                                                 {"B", TR("Delete (on an empty field: close the keyboard)")},
                                                                 {"X", TR("Space")},
                                                                 {"Y", TR("Shift (twice: Caps Lock)")},
                                                                 {"L1 / R1", TR("Move the text cursor")},
                                                                 {TR("View (⧉)"), TR("Letters / symbols")},
                                                                 {TR("Menu (≡)"), TR("Done")},
                                                             });
  wrappedDisabled(TR("Settings → Audio & Controls → On-screen keyboard: Auto, Built-in or Steam. Steam’s keyboard can type "
                     "Japanese; with Auto, a button press that still reaches ReplayNES while Steam’s keyboard was asked for "
                     "brings the built-in one."));
  table(TR("Keyboard (only while ReplayNES is in front)"), {
                                                              {TR("Arrow keys / X / Z"), TR("Move / A / B")},
                                                              {TR("Return / Right Shift, \\"), "START / SELECT"},
                                                              {TR("Backspace (hold)"), TR("Rewind")},
                                                              {TR("Tab (hold)"), TR("Fast-forward")},
                                                              {"Space", TR("Pause / Resume")},
                                                              {"L", TR("Slow 1/2 ⇔ normal speed")},
                                                              {", / .", TR("Step back / Frame advance")},
                                                              {"B", TR("Add Bookmark")},
                                                              {"Esc / F1", TR("Open the menu / back to the game")},
                                                              {"F11 / F3", TR("Full screen / statistics")},
                                                          });
  ImGui::Spacing();
  ImGui::PushFont(nullptr, S(22));
  ImGui::Selectable(TR("Record Button"), false);
  ImGui::PopFont();
  wrapped(TR("A glowing red “Record” means record mode. Click it to switch to gray playback mode, which plays the recorded "
             "take (from the start if you are at the end). Click again to return to a paused state where you can continue "
             "recording from that position. If you rewind and then play, a new take branches off automatically and the old "
             "continuation is kept."));
  ImGui::Spacing();
  ImGui::PushFont(nullptr, S(22));
  ImGui::Selectable(TR("Practice Mode (A/B Repeat)"), false);
  ImGui::PopFont();
  wrapped(TR("Open the section panel with the “Practice” button, press “A” at the start of the section and “B” at the end you "
             "reach by playing on from A. Press ▶︎ to practice that section repeatedly. At B it pauses for 0.5 s, then rewinds "
             "to A and starts again. Nothing is recorded while practicing; “Stop Practicing” returns to the original position "
             "in the take. Up to 8 sections are saved in the project."));
  ImGui::Spacing();
  ImGui::Selectable("##end", false, 0, ImVec2(0, S(8)));
}

}  // namespace rnl
