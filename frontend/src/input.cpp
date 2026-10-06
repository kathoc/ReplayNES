// Input catalog: action names + localized labels, default bindings per keyboard scheme, saved
// layout migrations, paused-step directions, physical-id display names, controller families,
// positional face mapping (Apple GameController by family + glyph, SDL3, XInput) and the
// assignment summaries of the controller diagram. The binding table itself lives in the engine
// (rn_input JSON); this file only describes it.
//
// Face buttons are bound by POSITION ("gc<slot>:face.south/east/west/north"), never by label:
// GameController.framework names Xbox / PlayStation / MFi face buttons by position (buttonA =
// south) but Nintendo controllers by their printed label (buttonA = the "A" button = east).
// NES A is always the east button and NES B the south button. SDL3 gamepad buttons are already
// positional (SDL_GAMEPAD_BUTTON_SOUTH ...).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <memory>
#include <set>

#include "common.hpp"
#include "util/Json.h"

using namespace rnf;

namespace {

struct ActionDef {
  const char* id;
  const char* label;  // localization key, or plain text when !localized
  bool localized;
  rnf_action_group group;
};

const ActionDef kActions[] = {
    {"p1.up", RNF_L("↑ Up"), true, RNF_GROUP_PLAYER1},
    {"p1.down", RNF_L("↓ Down"), true, RNF_GROUP_PLAYER1},
    {"p1.left", RNF_L("← Left"), true, RNF_GROUP_PLAYER1},
    {"p1.right", RNF_L("→ Right"), true, RNF_GROUP_PLAYER1},
    {"p1.a", "A", false, RNF_GROUP_PLAYER1},
    {"p1.b", "B", false, RNF_GROUP_PLAYER1},
    {"p1.select", "SELECT", false, RNF_GROUP_PLAYER1},
    {"p1.start", "START", false, RNF_GROUP_PLAYER1},
    {"p1.turbo_a", RNF_L("Turbo A"), true, RNF_GROUP_PLAYER1},
    {"p1.turbo_b", RNF_L("Turbo B"), true, RNF_GROUP_PLAYER1},
    {"p2.up", RNF_L("↑ Up"), true, RNF_GROUP_PLAYER2},
    {"p2.down", RNF_L("↓ Down"), true, RNF_GROUP_PLAYER2},
    {"p2.left", RNF_L("← Left"), true, RNF_GROUP_PLAYER2},
    {"p2.right", RNF_L("→ Right"), true, RNF_GROUP_PLAYER2},
    {"p2.a", "A", false, RNF_GROUP_PLAYER2},
    {"p2.b", "B", false, RNF_GROUP_PLAYER2},
    {"p2.select", "SELECT", false, RNF_GROUP_PLAYER2},
    {"p2.start", "START", false, RNF_GROUP_PLAYER2},
    {"p2.turbo_a", RNF_L("Turbo A"), true, RNF_GROUP_PLAYER2},
    {"p2.turbo_b", RNF_L("Turbo B"), true, RNF_GROUP_PLAYER2},
    {"hk.rewind", RNF_L("Rewind (while held)"), true, RNF_GROUP_HOTKEY},
    {"hk.fast_forward", RNF_L("Fast-forward (while held, recorded range only)"), true, RNF_GROUP_HOTKEY},
    {"hk.pause", RNF_L("Pause / Resume"), true, RNF_GROUP_HOTKEY},
    {"hk.slow", RNF_L("Slow motion (normal ⇔ 1/2)"), true, RNF_GROUP_HOTKEY},
    {"hk.frame_advance", RNF_L("Frame Advance"), true, RNF_GROUP_HOTKEY},
    {"hk.step_back", RNF_L("Step Back One Frame"), true, RNF_GROUP_HOTKEY},
    {"hk.toggle_mode", RNF_L("Record / playback (ends practice)"), true, RNF_GROUP_HOTKEY},
    {"hk.bookmark", RNF_L("Add Bookmark"), true, RNF_GROUP_HOTKEY},
    {"hk.undo_take", RNF_L("Back to Previous Take"), true, RNF_GROUP_HOTKEY},
    {"hk.save", RNF_L("Save"), true, RNF_GROUP_HOTKEY},
    {"hk.soft_reset", RNF_L("Soft Reset"), true, RNF_GROUP_HOTKEY},
    {"hk.power_cycle", RNF_L("Power Cycle"), true, RNF_GROUP_HOTKEY},
};
constexpr size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

struct Pair {
  const char* input;
  const char* action;
};

// Keyboard part of the defaults, macOS virtual key codes.
const Pair kKeyboardMac[] = {
    {"kb:126", "p1.up"}, {"kb:125", "p1.down"}, {"kb:123", "p1.left"}, {"kb:124", "p1.right"},
    {"kb:7", "p1.a"}, {"kb:6", "p1.b"},              // X = A, Z = B
    {"kb:1", "p1.turbo_a"}, {"kb:0", "p1.turbo_b"},  // S = turbo A, A = turbo B
    {"kb:36", "p1.start"}, {"kb:60", "p1.select"},   // Return = START, right Shift = SELECT
    {"kb:42", "p1.select"},                          // \ = SELECT (laptops)
    {"kb:49", "hk.pause"},                           // Space
    {"kb:47", "hk.frame_advance"},                   // .
    {"kb:43", "hk.step_back"},                       // ,
    {"kb:51", "hk.rewind"},                          // Delete (Backspace)
    {"kb:48", "hk.fast_forward"},                    // Tab
    {"kb:37", "hk.slow"},                            // L
    {"kb:11", "hk.bookmark"},                        // B
};
// The same keys as SDL3 scancodes.
const Pair kKeyboardSDL[] = {
    {"kb:82", "p1.up"}, {"kb:81", "p1.down"}, {"kb:80", "p1.left"}, {"kb:79", "p1.right"},
    {"kb:27", "p1.a"}, {"kb:29", "p1.b"},
    {"kb:22", "p1.turbo_a"}, {"kb:4", "p1.turbo_b"},
    {"kb:40", "p1.start"}, {"kb:229", "p1.select"},
    {"kb:49", "p1.select"},
    {"kb:44", "hk.pause"},
    {"kb:55", "hk.frame_advance"},
    {"kb:54", "hk.step_back"},
    {"kb:42", "hk.rewind"},
    {"kb:43", "hk.fast_forward"},
    {"kb:15", "hk.slow"},
    {"kb:5", "hk.bookmark"},
};
// Controller slot 0 -> P1, slot 1 -> P2. Face buttons by POSITION: east = A, south = B
// (Nintendo's own A/B; B/A on Xbox, ○/✕ on PlayStation), north = turbo A, west = turbo B.
// In-game controls (hotkeys, never recorded): L2 hold = rewind, R2 hold = fast-forward,
// L = slow 1/2 toggle, R = pause/play. While paused the D-pad ←/→ steps frames.
const Pair kController[] = {
    {"gc0:dpad.up", "p1.up"}, {"gc0:dpad.down", "p1.down"}, {"gc0:dpad.left", "p1.left"}, {"gc0:dpad.right", "p1.right"},
    {"gc0:lstick.up", "p1.up"}, {"gc0:lstick.down", "p1.down"}, {"gc0:lstick.left", "p1.left"}, {"gc0:lstick.right", "p1.right"},
    {"gc0:face.east", "p1.a"}, {"gc0:face.south", "p1.b"}, {"gc0:face.north", "p1.turbo_a"}, {"gc0:face.west", "p1.turbo_b"},
    {"gc0:menu", "p1.start"}, {"gc0:options", "p1.select"},
    {"gc0:leftTrigger", "hk.rewind"}, {"gc0:rightTrigger", "hk.fast_forward"},
    {"gc0:leftShoulder", "hk.slow"}, {"gc0:rightShoulder", "hk.pause"},
    {"gc1:dpad.up", "p2.up"}, {"gc1:dpad.down", "p2.down"}, {"gc1:dpad.left", "p2.left"}, {"gc1:dpad.right", "p2.right"},
    {"gc1:lstick.up", "p2.up"}, {"gc1:lstick.down", "p2.down"}, {"gc1:lstick.left", "p2.left"}, {"gc1:lstick.right", "p2.right"},
    {"gc1:face.east", "p2.a"}, {"gc1:face.south", "p2.b"}, {"gc1:face.north", "p2.turbo_a"}, {"gc1:face.west", "p2.turbo_b"},
    {"gc1:menu", "p2.start"}, {"gc1:options", "p2.select"},
};
constexpr size_t kKeyboardCount = sizeof(kKeyboardMac) / sizeof(kKeyboardMac[0]);
static_assert(sizeof(kKeyboardSDL) / sizeof(kKeyboardSDL[0]) == kKeyboardCount, "same keyboard layout");
constexpr size_t kControllerCount = sizeof(kController) / sizeof(kController[0]);

const Pair kLegacyControllerHotkeys[] = {
    {"gc0:leftShoulder", "hk.rewind"}, {"gc0:rightShoulder", "hk.fast_forward"},
    {"gc0:leftTrigger", "hk.step_back"}, {"gc0:rightTrigger", "hk.frame_advance"},
};
const Pair kControllerHotkeys[] = {
    {"gc0:leftTrigger", "hk.rewind"}, {"gc0:rightTrigger", "hk.fast_forward"},
    {"gc0:leftShoulder", "hk.slow"}, {"gc0:rightShoulder", "hk.pause"},
};
// Layouts 2-3 had the triggers the other way round (R2 rewind, L2 fast-forward).
const Pair kLayout3TriggerHotkeys[] = {
    {"gc0:rightTrigger", "hk.rewind"}, {"gc0:leftTrigger", "hk.fast_forward"},
};
const char* const kLegacyFaceNames[] = {"buttonA", "buttonB", "buttonX", "buttonY"};

Pair defaultAt(rnf_keyboard_scheme scheme, size_t i) {
  if (i < kKeyboardCount) return scheme == RNF_KEYBOARD_SDL ? kKeyboardSDL[i] : kKeyboardMac[i];
  return kController[i - kKeyboardCount];
}

Pairs defaults(rnf_keyboard_scheme scheme = RNF_KEYBOARD_MACOS) {
  Pairs out;
  for (size_t i = 0; i < kKeyboardCount + kControllerCount; ++i) {
    Pair p = defaultAt(scheme, i);
    out.emplace_back(p.input, p.action);
  }
  return out;
}

template <size_t N>
Pairs toPairs(const Pair (&a)[N]) {
  Pairs out;
  for (auto& p : a) out.emplace_back(p.input, p.action);
  return out;
}

std::string slotPrefix(int slot) { return "gc" + std::to_string(slot) + ":"; }

Pairs legacyFaceDefaults(int slot) {
  std::string g = slotPrefix(slot);
  std::string p = slot == 0 ? "p1" : "p2";
  return {{g + "buttonB", p + ".a"}, {g + "buttonA", p + ".b"}, {g + "buttonY", p + ".turbo_a"}, {g + "buttonX", p + ".turbo_b"}};
}

bool isLegacyFace(const std::string& input, int slot) {
  std::string prefix = slotPrefix(slot);
  if (!hasPrefix(input, prefix)) return false;
  std::string rest = input.substr(prefix.size());
  for (const char* n : kLegacyFaceNames) if (rest == n) return true;
  return false;
}

// Swift Int(String): optional sign, then decimal digits only.
bool parseInt(const std::string& s, long long& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; i = 1; }
  if (i >= s.size()) return false;
  long long v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    if (v > (INT64_MAX - 9) / 10) return false;
    v = v * 10 + (s[i] - '0');
  }
  out = neg ? -v : v;
  return true;
}

// Swift split(separator:): empty pieces dropped.
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

void emit(const Pairs& u, const Pairs& b, rnf_list** unbind, rnf_list** bind) {
  if (unbind) *unbind = makeList(u);
  if (bind) *bind = makeList(b);
}

// ------------------------------------------------------------------ display names
struct KeyName {
  int code;
  const char* name;
  bool localized;
};
// macOS virtual key codes.
const KeyName kMacKeys[] = {
    {0, "A", false}, {1, "S", false}, {2, "D", false}, {3, "F", false}, {4, "H", false}, {5, "G", false},
    {6, "Z", false}, {7, "X", false}, {8, "C", false}, {9, "V", false}, {11, "B", false}, {12, "Q", false},
    {13, "W", false}, {14, "E", false}, {15, "R", false}, {16, "Y", false}, {17, "T", false}, {18, "1", false},
    {19, "2", false}, {20, "3", false}, {21, "4", false}, {22, "6", false}, {23, "5", false}, {24, "=", false},
    {25, "9", false}, {26, "7", false}, {27, "-", false}, {28, "8", false}, {29, "0", false}, {30, "]", false},
    {31, "O", false}, {32, "U", false}, {33, "[", false}, {34, "I", false}, {35, "P", false}, {36, "Return", false},
    {37, "L", false}, {38, "J", false}, {39, "'", false}, {40, "K", false}, {41, ";", false}, {42, "\\", false},
    {43, ",", false}, {44, "/", false}, {45, "N", false}, {46, "M", false}, {47, ".", false}, {48, "Tab", false},
    {49, "Space", false}, {50, "`", false}, {51, "Delete", false}, {53, "Esc", false},
    {54, RNF_L("Right ⌘"), true}, {55, "⌘", false}, {56, RNF_L("Left Shift"), true}, {57, "Caps", false},
    {58, RNF_L("Left Option"), true}, {59, RNF_L("Left Control"), true}, {60, RNF_L("Right Shift"), true},
    {61, RNF_L("Right Option"), true}, {62, RNF_L("Right Control"), true}, {63, "fn", false},
    {65, RNF_L("Keypad ."), true}, {67, RNF_L("Keypad *"), true}, {69, RNF_L("Keypad +"), true}, {71, "Clear", false},
    {75, RNF_L("Keypad /"), true}, {76, "Enter", false}, {78, RNF_L("Keypad -"), true}, {81, RNF_L("Keypad ="), true},
    {82, RNF_L("Keypad 0"), true}, {83, RNF_L("Keypad 1"), true}, {84, RNF_L("Keypad 2"), true},
    {85, RNF_L("Keypad 3"), true}, {86, RNF_L("Keypad 4"), true}, {87, RNF_L("Keypad 5"), true},
    {88, RNF_L("Keypad 6"), true}, {89, RNF_L("Keypad 7"), true}, {91, RNF_L("Keypad 8"), true},
    {92, RNF_L("Keypad 9"), true}, {96, "F5", false}, {97, "F6", false}, {98, "F7", false}, {99, "F3", false},
    {100, "F8", false}, {101, "F9", false}, {103, "F11", false}, {109, "F10", false}, {111, "F12", false},
    {115, "Home", false}, {116, "PageUp", false}, {117, "⌦", false}, {118, "F4", false}, {119, "End", false},
    {120, "F2", false}, {121, "PageDown", false}, {122, "F1", false}, {123, "←", false}, {124, "→", false},
    {125, "↓", false}, {126, "↑", false}, {102, RNF_L("Eisu"), true}, {104, RNF_L("Kana"), true},
};
// SDL3 scancodes (frontends may prefer SDL_GetScancodeName for keys missing here).
const KeyName kSDLKeys[] = {
    {4, "A", false}, {5, "B", false}, {6, "C", false}, {7, "D", false}, {8, "E", false}, {9, "F", false},
    {10, "G", false}, {11, "H", false}, {12, "I", false}, {13, "J", false}, {14, "K", false}, {15, "L", false},
    {16, "M", false}, {17, "N", false}, {18, "O", false}, {19, "P", false}, {20, "Q", false}, {21, "R", false},
    {22, "S", false}, {23, "T", false}, {24, "U", false}, {25, "V", false}, {26, "W", false}, {27, "X", false},
    {28, "Y", false}, {29, "Z", false}, {30, "1", false}, {31, "2", false}, {32, "3", false}, {33, "4", false},
    {34, "5", false}, {35, "6", false}, {36, "7", false}, {37, "8", false}, {38, "9", false}, {39, "0", false},
    {40, "Return", false}, {41, "Esc", false}, {42, "Backspace", false}, {43, "Tab", false}, {44, "Space", false},
    {45, "-", false}, {46, "=", false}, {47, "[", false}, {48, "]", false}, {49, "\\", false}, {51, ";", false},
    {52, "'", false}, {53, "`", false}, {54, ",", false}, {55, ".", false}, {56, "/", false}, {57, "Caps", false},
    {58, "F1", false}, {59, "F2", false}, {60, "F3", false}, {61, "F4", false}, {62, "F5", false}, {63, "F6", false},
    {64, "F7", false}, {65, "F8", false}, {66, "F9", false}, {67, "F10", false}, {68, "F11", false}, {69, "F12", false},
    {73, "Insert", false}, {74, "Home", false}, {75, "PageUp", false}, {76, "Delete", false}, {77, "End", false},
    {78, "PageDown", false}, {79, "→", false}, {80, "←", false}, {81, "↓", false}, {82, "↑", false},
    {84, RNF_L("Keypad /"), true}, {85, RNF_L("Keypad *"), true}, {86, RNF_L("Keypad -"), true},
    {87, RNF_L("Keypad +"), true}, {88, "Enter", false}, {89, RNF_L("Keypad 1"), true}, {90, RNF_L("Keypad 2"), true},
    {91, RNF_L("Keypad 3"), true}, {92, RNF_L("Keypad 4"), true}, {93, RNF_L("Keypad 5"), true},
    {94, RNF_L("Keypad 6"), true}, {95, RNF_L("Keypad 7"), true}, {96, RNF_L("Keypad 8"), true},
    {97, RNF_L("Keypad 9"), true}, {98, RNF_L("Keypad 0"), true}, {99, RNF_L("Keypad ."), true},
    {103, RNF_L("Keypad ="), true}, {224, RNF_L("Left Control"), true}, {225, RNF_L("Left Shift"), true},
    {226, RNF_L("Left Alt"), true}, {227, RNF_L("Left Super"), true}, {228, RNF_L("Right Control"), true},
    {229, RNF_L("Right Shift"), true}, {230, RNF_L("Right Alt"), true}, {231, RNF_L("Right Super"), true},
};

const KeyName* findKey(rnf_keyboard_scheme scheme, long long code) {
  if (scheme == RNF_KEYBOARD_SDL) {
    for (auto& k : kSDLKeys) if (k.code == code) return &k;
  } else {
    for (auto& k : kMacKeys) if (k.code == code) return &k;
  }
  return nullptr;
}

struct NameDef {
  const char* element;
  const char* text;
  bool localized;
};
const NameDef kPrettyElements[] = {
    {"face.south", RNF_L("Bottom Button"), true}, {"face.east", RNF_L("Right Button"), true},
    {"face.west", RNF_L("Left Button"), true}, {"face.north", RNF_L("Top Button"), true},
    {"buttonA", RNF_L("A (legacy)"), true}, {"buttonB", RNF_L("B (legacy)"), true},
    {"buttonX", RNF_L("X (legacy)"), true}, {"buttonY", RNF_L("Y (legacy)"), true},
    {"dpad.up", RNF_L("D-pad ↑"), true}, {"dpad.down", RNF_L("D-pad ↓"), true},
    {"dpad.left", RNF_L("D-pad ←"), true}, {"dpad.right", RNF_L("D-pad →"), true},
    {"lstick.up", RNF_L("L Stick ↑"), true}, {"lstick.down", RNF_L("L Stick ↓"), true},
    {"lstick.left", RNF_L("L Stick ←"), true}, {"lstick.right", RNF_L("L Stick →"), true},
    {"rstick.up", RNF_L("R Stick ↑"), true}, {"rstick.down", RNF_L("R Stick ↓"), true},
    {"rstick.left", RNF_L("R Stick ←"), true}, {"rstick.right", RNF_L("R Stick →"), true},
    {"leftShoulder", "L(L1/LB)", false}, {"rightShoulder", "R(R1/RB)", false},
    {"leftTrigger", "ZL(L2/LT)", false}, {"rightTrigger", "ZR(R2/RT)", false},
    {"menu", "+(Menu)", false}, {"options", "−(Options)", false}, {"home", "Home", false},
    {"leftThumb", "L3", false}, {"rightThumb", "R3", false},
};

std::string displayName(rnf_keyboard_scheme scheme, const std::string& id) {
  long long code = 0;
  if (hasPrefix(id, "kb:") && parseInt(id.substr(3), code) && code >= 0 && code <= 65535 && id[3] != '-') {
    const KeyName* k = findKey(scheme, code);
    std::string name = k ? (k->localized ? tr(k->name) : k->name) : "#" + std::to_string(code);
    return trf(RNF_L("Key %@"), {argS(name)});
  }
  size_t colon = id.find(':');
  if (hasPrefix(id, "gc") && colon != std::string::npos) {
    long long slot = 0;
    if (!parseInt(id.substr(2, colon - 2), slot)) slot = 0;
    std::string name = id.substr(colon + 1);
    std::string pretty = name;
    for (auto& n : kPrettyElements)
      if (name == n.element) { pretty = n.localized ? tr(n.text) : n.text; break; }
    return trf(RNF_L("Pad %lld %@"), {argI(slot + 1), argS(pretty)});
  }
  return id;
}

// ------------------------------------------------------------------ families + labels
rnf_controller_family familyFrom(const std::string& category, const std::string& vendor) {
  std::string s = lower(category + " " + vendor);
  auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
  if (has("switch") || has("joy-con") || has("nintendo") || has("pro controller")) return RNF_FAMILY_NINTENDO;
  if (has("dualsense") || has("dualshock") || has("playstation")) return RNF_FAMILY_PLAYSTATION;
  if (has("xbox")) return RNF_FAMILY_XBOX;
  if (has("steam deck")) return RNF_FAMILY_STEAM_DECK;
  return RNF_FAMILY_GENERIC;
}

const char* familyLabel(rnf_controller_family f, const std::string& e) {
  using F = rnf_controller_family;
  const F N = RNF_FAMILY_NINTENDO, X = RNF_FAMILY_XBOX, P = RNF_FAMILY_PLAYSTATION, D = RNF_FAMILY_STEAM_DECK;
  if (f == N) {
    if (e == "face.south") return "B";
    if (e == "face.east") return "A";
    if (e == "face.west") return "Y";
    if (e == "face.north") return "X";
  }
  if (f == P) {
    if (e == "face.south") return "✕";
    if (e == "face.east") return "○";
    if (e == "face.west") return "□";
    if (e == "face.north") return "△";
  }
  if (e == "face.south") return "A";
  if (e == "face.east") return "B";
  if (e == "face.west") return "X";
  if (e == "face.north") return "Y";
  if (f == N) {
    if (e == "leftShoulder") return "L";
    if (e == "rightShoulder") return "R";
    if (e == "leftTrigger") return "ZL";
    if (e == "rightTrigger") return "ZR";
    if (e == "menu") return "+";
    if (e == "options") return "−";
    if (e == "home") return "HOME";
  }
  if (f == X) {
    if (e == "leftShoulder") return "LB";
    if (e == "rightShoulder") return "RB";
    if (e == "leftTrigger") return "LT";
    if (e == "rightTrigger") return "RT";
    if (e == "menu") return "≡";
    if (e == "options") return "View";
    if (e == "home") return "Xbox";
  }
  if (f == P) {
    if (e == "menu") return "OPTIONS";
    if (e == "options") return "CREATE";
    if (e == "home") return "PS";
  }
  if (f == D) {
    if (e == "menu") return "≡";
    if (e == "options") return "⧉";
    if (e == "home") return "STEAM";
    if (e == "misc") return "…";
    if (e == "rightPaddle1") return "R4";
    if (e == "leftPaddle1") return "L4";
    if (e == "rightPaddle2") return "R5";
    if (e == "leftPaddle2") return "L5";
  }
  if (e == "leftShoulder") return "L1";
  if (e == "rightShoulder") return "R1";
  if (e == "leftTrigger") return "L2";
  if (e == "rightTrigger") return "R2";
  if (e == "menu") return "Menu";
  if (e == "options") return "Options";
  if (e == "home") return "Home";
  if (f == X) {
    if (e == "leftThumb") return "LS";
    if (e == "rightThumb") return "RS";
  }
  if (e == "leftThumb") return "L3";
  if (e == "rightThumb") return "R3";
  return "";
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

const char* labelFromSymbol(const char* name) {
  if (!name) return nullptr;
  std::string base = replaceAll(replaceAll(name, ".fill", ""), ".circle", "");
  if (base == "a") return "A";
  if (base == "b") return "B";
  if (base == "x") return "X";
  if (base == "y") return "Y";
  if (base == "xmark") return "✕";
  if (base == "circle") return "○";
  if (base == "square") return "□";
  if (base == "triangle") return "△";
  return nullptr;
}

// ------------------------------------------------------------------ assignments
std::vector<std::string> elementActions(const Pairs& b, const std::string& element, int slot) {
  std::string id = slotPrefix(slot) + element;
  std::set<std::string> bound;
  for (auto& p : b) if (p.first == id) bound.insert(p.second);
  std::vector<std::string> out;
  for (auto& a : kActions) if (bound.count(a.id)) out.push_back(a.id);
  return out;
}

std::string shortLabel(const std::string& action, int slot) {
  static const NameDef hk[] = {
      {"hk.rewind", RNF_L("Rewind"), true}, {"hk.fast_forward", RNF_L("Fast Fwd"), true},
      {"hk.pause", RNF_L("Pause"), true}, {"hk.slow", RNF_L("Slow"), true},
      {"hk.frame_advance", RNF_L("Advance"), true}, {"hk.step_back", RNF_L("Step Back"), true},
      {"hk.toggle_mode", RNF_L("Rec/Play"), true}, {"hk.bookmark", RNF_L("Bookmark"), true},
      {"hk.undo_take", RNF_L("Prev Take"), true}, {"hk.save", RNF_L("Save"), true},
      {"hk.soft_reset", RNF_L("Reset"), true}, {"hk.power_cycle", RNF_L("Power"), true},
  };
  for (auto& h : hk) if (action == h.element) return tr(h.text);
  auto parts = split(action, '.');
  if (parts.size() != 2) return action;
  static const NameDef game[] = {
      {"a", "A", false}, {"b", "B", false}, {"select", "SELECT", false}, {"start", "START", false},
      {"up", "↑", false}, {"down", "↓", false}, {"left", "←", false}, {"right", "→", false},
      {"turbo_a", RNF_L("Turbo A"), true}, {"turbo_b", RNF_L("Turbo B"), true},
  };
  std::string name = parts[1];
  for (auto& g : game) if (parts[1] == g.element) { name = g.localized ? tr(g.text) : g.text; break; }
  int player = parts[0] == "p2" ? 1 : 0;
  return player == slot ? name : std::to_string(player + 1) + "P " + name;
}

}  // namespace

// ------------------------------------------------------------------ config
struct rnf_input_config {
  Pairs bindings;
  int turboPeriod = 2, turboDuty = 1;
  std::string socd = "neutral";
  double analogThreshold = 0.5;
};

extern "C" {

size_t rnf_input_action_count(void) { return kActionCount; }
int rnf_input_action_get(size_t i, rnf_action_info* out) {
  if (i >= kActionCount || !out) return 0;
  out->id = kActions[i].id;
  out->group = kActions[i].group;
  return 1;
}
char* rnf_input_action_label(const char* id) {
  if (!id) return nullptr;
  for (auto& a : kActions)
    if (std::strcmp(a.id, id) == 0) return dup(a.localized ? tr(a.label) : a.label);
  return nullptr;
}
char* rnf_input_group_title(rnf_action_group g) {
  switch (g) {
    case RNF_GROUP_PLAYER1: return dup(tr(RNF_L("Player 1")));
    case RNF_GROUP_PLAYER2: return dup(tr(RNF_L("Player 2")));
    case RNF_GROUP_HOTKEY: return dup(tr(RNF_L("Hotkeys")));
  }
  return dup("");
}

size_t rnf_input_default_binding_count(rnf_keyboard_scheme) { return kKeyboardCount + kControllerCount; }
int rnf_input_default_binding_get(rnf_keyboard_scheme scheme, size_t i, rnf_binding* out) {
  if (i >= kKeyboardCount + kControllerCount || !out) return 0;
  Pair p = defaultAt(scheme, i);
  out->input = p.input;
  out->action = p.action;
  return 1;
}

char* rnf_input_default_config_json(rnf_keyboard_scheme scheme) {
  RNF_GUARD_BEGIN
  // Same object as the original JSONSerialization output (sorted keys).
  rn::Json root = rn::Json::object();
  root.set("analogThreshold", 0.5);
  rn::Json b = rn::Json::array();
  for (auto& p : defaults(scheme)) {
    rn::Json e = rn::Json::object();
    e.set("action", p.second);
    e.set("input", p.first);
    b.push(e);
  }
  root.set("bindings", b);
  root.set("socd", "neutral");
  rn::Json t = rn::Json::object();
  t.set("duty", 2);
  t.set("period", 4);
  root.set("turbo", t);
  root.set("version", 1);
  return dup(root.dump(0));
  RNF_GUARD_END(nullptr)
}

rnf_list* rnf_input_legacy_controller_hotkeys(void) { return makeList(toPairs(kLegacyControllerHotkeys)); }
rnf_list* rnf_input_controller_hotkeys(void) { return makeList(toPairs(kControllerHotkeys)); }
rnf_list* rnf_input_legacy_face_defaults(int slot) { return makeList(legacyFaceDefaults(slot)); }
int rnf_input_is_legacy_face(const char* input, int slot) { return input && isLegacyFace(input, slot); }

rn_status rnf_input_config_parse(const char* json, rnf_input_config** out) {
  if (out) *out = nullptr;
  if (!json || !out) return fail(RN_ERR_INVALID_ARG, "null argument");
  RNF_GUARD_BEGIN
  rn::Json root;
  std::string err;
  if (!rn::Json::parse(json, root, &err) || !root.isObject()) return fail(RN_ERR_CORRUPT, "bad input config: " + err);
  auto c = std::make_unique<rnf_input_config>();
  const rn::Json& b = root["bindings"];
  if (b.isArray()) {
    for (auto& e : b.items()) {
      if (!e.isObject()) continue;
      const rn::Json& i = e["input"];
      const rn::Json& a = e["action"];
      if (!i.isString() || !a.isString()) continue;
      c->bindings.emplace_back(i.asString(), a.asString());
    }
  }
  const rn::Json& t = root["turbo"];
  if (t.isObject()) {
    const rn::Json& p = t["period"];
    const rn::Json& d = t["duty"];
    if (p.isNumber() || p.isBool()) c->turboPeriod = p.isBool() ? int(p.asBool()) : int(p.asInt());
    if (d.isNumber() || d.isBool()) c->turboDuty = d.isBool() ? int(d.asBool()) : int(d.asInt());
  }
  if (root["socd"].isString()) c->socd = root["socd"].asString();
  const rn::Json& at = root["analogThreshold"];
  if (at.isNumber()) c->analogThreshold = at.asDouble();
  else if (at.isBool()) c->analogThreshold = at.asBool() ? 1 : 0;
  *out = c.release();
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}
void rnf_input_config_free(rnf_input_config* c) { delete c; }
size_t rnf_input_config_binding_count(const rnf_input_config* c) { return c ? c->bindings.size() : 0; }
int rnf_input_config_binding_get(const rnf_input_config* c, size_t i, rnf_binding* out) {
  if (!c || i >= c->bindings.size() || !out) return 0;
  out->input = c->bindings[i].first.c_str();
  out->action = c->bindings[i].second.c_str();
  return 1;
}
int rnf_input_config_turbo_period(const rnf_input_config* c) { return c ? c->turboPeriod : 2; }
int rnf_input_config_turbo_duty(const rnf_input_config* c) { return c ? c->turboDuty : 1; }
const char* rnf_input_config_socd(const rnf_input_config* c) { return c ? c->socd.c_str() : "neutral"; }
double rnf_input_config_analog_threshold(const rnf_input_config* c) { return c ? c->analogThreshold : 0.5; }

void rnf_input_controller_layout_migration(const rnf_binding* bindings, size_t count, rnf_list** unbind,
                                           rnf_list** bind) {
  Pairs b = rnf::toPairs(bindings, count);
  auto has = [&](const Pair& p) {
    return std::any_of(b.begin(), b.end(), [&](auto& x) { return x.first == p.input && x.second == p.action; });
  };
  size_t hotkeyOnGC0 = size_t(std::count_if(b.begin(), b.end(), [](auto& x) {
    return hasPrefix(x.first, "gc0:") && hasPrefix(x.second, "hk.");
  }));
  bool all = std::all_of(std::begin(kLegacyControllerHotkeys), std::end(kLegacyControllerHotkeys), has);
  constexpr size_t n = sizeof(kLegacyControllerHotkeys) / sizeof(kLegacyControllerHotkeys[0]);
  if (!all || hotkeyOnGC0 != n) return emit({}, {}, unbind, bind);
  emit(toPairs(kLegacyControllerHotkeys), toPairs(kControllerHotkeys), unbind, bind);
}

void rnf_input_trigger_swap_migration(const rnf_binding* bindings, size_t count, rnf_list** unbind, rnf_list** bind) {
  // Only when both triggers of pad 1 still do exactly what layout 3 assigned (nothing else on
  // them): customised triggers are left alone.
  Pairs b = rnf::toPairs(bindings, count);
  for (const Pair& old : kLayout3TriggerHotkeys) {
    size_t onInput = size_t(std::count_if(b.begin(), b.end(), [&](auto& x) { return x.first == old.input; }));
    bool has = std::any_of(b.begin(), b.end(), [&](auto& x) { return x.first == old.input && x.second == old.action; });
    if (!has || onInput != 1) return emit({}, {}, unbind, bind);
  }
  Pairs u = toPairs(kLayout3TriggerHotkeys), nb;
  for (const Pair& p : kControllerHotkeys)
    if (std::string(p.input) == "gc0:leftTrigger" || std::string(p.input) == "gc0:rightTrigger") nb.emplace_back(p.input, p.action);
  emit(u, nb, unbind, bind);
}

void rnf_input_face_layout_migration(const rnf_binding* bindings, size_t count, rnf_list** unbind, rnf_list** bind) {
  Pairs b = rnf::toPairs(bindings, count);
  Pairs u, nb;
  Pairs defs = defaults();
  for (int slot = 0; slot < 2; ++slot) {
    std::vector<std::string> legacy;
    for (auto& p : b) if (isLegacyFace(p.first, slot)) legacy.push_back(p.first + "→" + p.second);
    Pairs old = legacyFaceDefaults(slot);
    std::set<std::string> oldSet;
    for (auto& p : old) oldSet.insert(p.first + "→" + p.second);
    if (legacy.size() != old.size() || std::set<std::string>(legacy.begin(), legacy.end()) != oldSet) continue;
    u.insert(u.end(), old.begin(), old.end());
    std::string prefix = slotPrefix(slot) + "face.";
    for (auto& d : defs) if (hasPrefix(d.first, prefix)) nb.push_back(d);
  }
  emit(u, nb, unbind, bind);
}

void rnf_input_legacy_face_translation(const rnf_binding* bindings, size_t count, int slot, const char* const* names,
                                       const int* positions, size_t position_count, rnf_list** unbind,
                                       rnf_list** bind) {
  Pairs b = rnf::toPairs(bindings, count);
  std::string prefix = slotPrefix(slot);
  Pairs u, nb;
  for (auto& p : b) {
    if (!isLegacyFace(p.first, slot)) continue;
    std::string name = p.first.substr(prefix.size());
    const char* element = nullptr;
    for (size_t i = 0; names && positions && i < position_count; ++i)
      if (names[i] && name == names[i]) { element = rnf_face_position_element(rnf_face_position(positions[i])); break; }
    if (!element || !*element) continue;
    u.emplace_back(p.first, p.second);
    nb.emplace_back(prefix + element, p.second);
  }
  emit(u, nb, unbind, bind);
}

void rnf_input_controller_reset_plan(const rnf_binding* bindings, size_t count, int slot, rnf_list** unbind,
                                     rnf_list** bind) {
  Pairs b = rnf::toPairs(bindings, count);
  std::string prefix = slotPrefix(slot);
  Pairs u, nb;
  for (auto& p : b) if (hasPrefix(p.first, prefix)) u.push_back(p);
  for (auto& d : defaults()) if (hasPrefix(d.first, prefix)) nb.push_back(d);
  emit(u, nb, unbind, bind);
}

int rnf_input_apply_plan(rn_input* in, const rnf_list* unbind, const rnf_list* bind) {
  if (!in) return 0;
  size_t nu = rnf_list_count(unbind), nb = rnf_list_count(bind);
  for (size_t i = 0; i < nu; ++i) (void)rn_input_unbind(in, rnf_list_a(unbind, i), rnf_list_b(unbind, i));
  for (size_t i = 0; i < nb; ++i) (void)rn_input_bind(in, rnf_list_a(bind, i), rnf_list_b(bind, i));
  return nu > 0 || nb > 0;
}

rnf_list* rnf_input_paused_step_directions(const rnf_binding* bindings, size_t count) {
  Pairs b = rnf::toPairs(bindings, count);
  auto* l = new rnf_list;
  auto set = [&](const std::string& input, int dir) {
    for (auto& it : l->items) if (it.a == input) { it.value = dir; return; }
    l->items.push_back({input, "", dir});
  };
  for (auto& p : b) {
    if (!hasPrefix(p.first, "gc") || p.first.find("stick") != std::string::npos) continue;
    if (p.second == "p1.left" || p.second == "p2.left") set(p.first, -1);
    if (p.second == "p1.right" || p.second == "p2.right") set(p.first, 1);
  }
  return l;
}

char* rnf_input_display_name(rnf_keyboard_scheme scheme, const char* physical_id, const char* face_label) {
  if (!physical_id) return dup("");
  RNF_GUARD_BEGIN
  std::string id = physical_id;
  std::string base = displayName(scheme, id);
  size_t colon = id.find(':');
  long long slot = 0;
  if (face_label && hasPrefix(id, "gc") && colon != std::string::npos && parseInt(id.substr(2, colon - 2), slot) &&
      hasPrefix(id.substr(colon + 1), "face."))
    base += "(" + std::string(face_label) + ")";
  return dup(base);
  RNF_GUARD_END(nullptr)
}

int rnf_input_controller_slot(const char* physical_id, int* slot) {
  if (!physical_id) return 0;
  std::string id = physical_id;
  size_t colon = id.find(':');
  long long s = 0;
  if (!hasPrefix(id, "gc") || colon == std::string::npos || !parseInt(id.substr(2, colon - 2), s)) return 0;
  if (slot) *slot = int(s);
  return 1;
}

// ------------------------------------------------------------------ controllers
const char* rnf_face_position_element(rnf_face_position p) {
  switch (p) {
    case RNF_FACE_SOUTH: return "face.south";
    case RNF_FACE_EAST: return "face.east";
    case RNF_FACE_WEST: return "face.west";
    case RNF_FACE_NORTH: return "face.north";
  }
  return "";
}
const char* rnf_face_position_name(rnf_face_position p) {
  switch (p) {
    case RNF_FACE_SOUTH: return "south";
    case RNF_FACE_EAST: return "east";
    case RNF_FACE_WEST: return "west";
    case RNF_FACE_NORTH: return "north";
  }
  return "";
}

rnf_controller_family rnf_controller_family_from(const char* category, const char* vendor) {
  return familyFrom(category ? category : "", vendor ? vendor : "");
}
char* rnf_controller_family_title(rnf_controller_family f) {
  switch (f) {
    case RNF_FAMILY_NINTENDO: return dup(tr(RNF_L("Nintendo (Pro Controller / Joy-Con)")));
    case RNF_FAMILY_XBOX: return dup("Xbox");
    case RNF_FAMILY_PLAYSTATION: return dup("PlayStation");
    case RNF_FAMILY_STEAM_DECK: return dup("Steam Deck");
    case RNF_FAMILY_GENERIC: break;
  }
  return dup(tr(RNF_L("Other")));
}
const char* rnf_controller_family_label(rnf_controller_family f, const char* element) {
  return element ? familyLabel(f, element) : "";
}
int rnf_controller_family_is_symmetric(rnf_controller_family f) { return f == RNF_FAMILY_PLAYSTATION; }

void rnf_gc_face_positions(const char* product_category, const char* vendor_name, const char* const symbols[4],
                           int out[4]) {
  if (!out) return;
  std::string cat = lower(product_category ? product_category : "");
  auto set = [&](int a, int b, int x, int y) { out[0] = a; out[1] = b; out[2] = x; out[3] = y; };
  // A single Joy-Con held sideways (SDL_mfijoystick.m: A = south, B = west, X = east, Y = north).
  if (cat.find("joy-con") != std::string::npos && (hasSuffix(cat, "(l)") || hasSuffix(cat, "(r)"))) {
    return set(RNF_FACE_SOUTH, RNF_FACE_WEST, RNF_FACE_EAST, RNF_FACE_NORTH);
  }
  if (familyFrom(product_category ? product_category : "", vendor_name ? vendor_name : "") == RNF_FAMILY_NINTENDO) {
    // The printed glyph fixes the position on a Nintendo layout (A east, B south, X north, Y west).
    int glyph[4];
    std::set<int> seen;
    int n = 0;
    for (int i = 0; i < 4; ++i) {
      const char* l = labelFromSymbol(symbols ? symbols[i] : nullptr);
      if (!l) continue;
      std::string s = l;
      int pos = s == "A" ? RNF_FACE_EAST : s == "B" ? RNF_FACE_SOUTH : s == "X" ? RNF_FACE_NORTH : s == "Y" ? RNF_FACE_WEST : -1;
      if (pos < 0) continue;
      glyph[i] = pos;
      seen.insert(pos);
      ++n;
    }
    if (n == 4 && seen.size() == 4) return set(glyph[0], glyph[1], glyph[2], glyph[3]);
    // No glyphs: assume named by label (A is the right (east) button, B the bottom one).
    return set(RNF_FACE_EAST, RNF_FACE_SOUTH, RNF_FACE_NORTH, RNF_FACE_WEST);
  }
  // Apple's documented diamond (GCExtendedGamepad.h): A bottom, B right, X left, Y top.
  set(RNF_FACE_SOUTH, RNF_FACE_EAST, RNF_FACE_WEST, RNF_FACE_NORTH);
}

const char* rnf_gc_label_from_symbol(const char* symbol) { return labelFromSymbol(symbol); }

const char* rnf_sdl_button_element(int b) {
  // SDL_GamepadButton (SDL3).
  static const char* const names[] = {
      "face.south", "face.east", "face.west", "face.north",   // SOUTH, EAST, WEST, NORTH
      "options", "home", "menu",                               // BACK, GUIDE, START
      "leftThumb", "rightThumb", "leftShoulder", "rightShoulder",
      "dpad.up", "dpad.down", "dpad.left", "dpad.right",
      "misc",                                                  // MISC1 (Share / capture / Deck "...")
      "rightPaddle1", "leftPaddle1", "rightPaddle2", "leftPaddle2",
      "touchpad",
  };
  if (b < 0 || b >= int(sizeof(names) / sizeof(names[0]))) return nullptr;
  return names[b];
}

const char* rnf_sdl_axis_element(int axis, int positive) {
  switch (axis) {  // SDL_GamepadAxis
    case 0: return positive ? "lstick.right" : "lstick.left";
    case 1: return positive ? "lstick.down" : "lstick.up";
    case 2: return positive ? "rstick.right" : "rstick.left";
    case 3: return positive ? "rstick.down" : "rstick.up";
    case 4: return positive ? "leftTrigger" : nullptr;
    case 5: return positive ? "rightTrigger" : nullptr;
  }
  return nullptr;
}

rnf_controller_family rnf_sdl_controller_family(int type, const char* name) {
  std::string n = lower(name ? name : "");
  if (n.find("steam deck") != std::string::npos) return RNF_FAMILY_STEAM_DECK;
  switch (type) {  // SDL_GamepadType
    case 2: case 3: return RNF_FAMILY_XBOX;                 // XBOX360, XBOXONE
    case 4: case 5: case 6: return RNF_FAMILY_PLAYSTATION;  // PS3, PS4, PS5
    case 7: case 8: case 9: case 10: return RNF_FAMILY_NINTENDO;  // SWITCH_PRO, JOYCON_L/R/PAIR
    default: break;
  }
  return familyFrom(name ? name : "", "");
}

const char* rnf_sdl_button_label_text(int label) {
  switch (label) {  // SDL_GamepadButtonLabel
    case 1: return "A";
    case 2: return "B";
    case 3: return "X";
    case 4: return "Y";
    case 5: return "✕";
    case 6: return "○";
    case 7: return "□";
    case 8: return "△";
  }
  return nullptr;
}

const char* rnf_xinput_button_element(uint32_t bit) {
  switch (bit) {
    case 0x0001: return "dpad.up";
    case 0x0002: return "dpad.down";
    case 0x0004: return "dpad.left";
    case 0x0008: return "dpad.right";
    case 0x0010: return "menu";
    case 0x0020: return "options";
    case 0x0040: return "leftThumb";
    case 0x0080: return "rightThumb";
    case 0x0100: return "leftShoulder";
    case 0x0200: return "rightShoulder";
    case 0x1000: return "face.south";
    case 0x2000: return "face.east";
    case 0x4000: return "face.west";
    case 0x8000: return "face.north";
  }
  return nullptr;
}

// ------------------------------------------------------------------ assignments
rnf_list* rnf_input_element_actions(const rnf_binding* bindings, size_t count, const char* element, int slot) {
  Pairs out;
  for (auto& a : elementActions(rnf::toPairs(bindings, count), element ? element : "", slot)) out.emplace_back(a, "");
  return makeList(out);
}

char* rnf_input_element_title(const char* element_c, rnf_controller_family f, const char* label_override) {
  RNF_GUARD_BEGIN
  std::string element = element_c ? element_c : "";
  std::string label = label_override ? label_override : familyLabel(f, element);
  auto parts = split(element, '.');
  if (parts.size() == 2) {
    const char* arrow = parts[1] == "up" ? "↑" : parts[1] == "down" ? "↓" : parts[1] == "left" ? "←"
                      : parts[1] == "right" ? "→" : nullptr;
    if (arrow) {
      std::string base = parts[0] == "dpad" ? tr(RNF_L("D-pad")) : parts[0] == "lstick" ? tr(RNF_L("Left Stick"))
                       : parts[0] == "rstick" ? tr(RNF_L("Right Stick")) : parts[0];
      return dup(base + " " + arrow);
    }
  }
  static const NameDef positional[] = {
      {"face.south", RNF_L("Bottom Button"), true}, {"face.east", RNF_L("Right Button"), true},
      {"face.west", RNF_L("Left Button"), true}, {"face.north", RNF_L("Top Button"), true},
      {"leftThumb", RNF_L("Left Stick Press"), true}, {"rightThumb", RNF_L("Right Stick Press"), true},
  };
  for (auto& p : positional) {
    if (element != p.element) continue;
    std::string pt = tr(p.text);
    return dup(label.empty() ? pt : trf(RNF_L("%@ (%@)"), {argS(pt), argS(label)}));
  }
  return dup(label.empty() ? element : label);
  RNF_GUARD_END(nullptr)
}

char* rnf_input_action_short_label(const char* action, int slot) {
  RNF_GUARD_BEGIN
  return dup(shortLabel(action ? action : "", slot));
  RNF_GUARD_END(nullptr)
}

char* rnf_input_element_badge(const rnf_binding* bindings, size_t count, const char* element, int slot) {
  RNF_GUARD_BEGIN
  auto a = elementActions(rnf::toPairs(bindings, count), element ? element : "", slot);
  if (a.empty()) return nullptr;
  std::string out;
  for (size_t i = 0; i < a.size(); ++i) {
    if (i) out += tr(RNF_L(" · "));
    out += shortLabel(a[i], slot);
  }
  return dup(out);
  RNF_GUARD_END(nullptr)
}

rnf_group_summary rnf_input_group_summary(const rnf_binding* bindings, size_t count, const char* group_c, int slot,
                                          char** movement_text) {
  if (movement_text) *movement_text = nullptr;
  RNF_GUARD_BEGIN
  Pairs b = rnf::toPairs(bindings, count);
  std::string group = group_c ? group_c : "";
  const char* dirs[] = {"up", "down", "left", "right"};
  std::vector<std::vector<std::string>> per;
  bool allEmpty = true;
  for (auto d : dirs) {
    per.push_back(elementActions(b, group + "." + d, slot));
    if (!per.back().empty()) allEmpty = false;
  }
  if (allEmpty) return RNF_GROUP_NONE;
  for (const char* p : {"p1", "p2"}) {
    bool ok = true;
    for (int i = 0; i < 4; ++i)
      if (per[size_t(i)] != std::vector<std::string>{std::string(p) + "." + dirs[i]}) { ok = false; break; }
    if (!ok) continue;
    int player = std::string(p) == "p2" ? 1 : 0;
    if (movement_text)
      *movement_text = dup(player == slot ? std::string(tr(RNF_L("Move"))) : trf(RNF_L("%lldP Move"), {argI(player + 1)}));
    return RNF_GROUP_MOVEMENT;
  }
  return RNF_GROUP_CUSTOM;
  RNF_GUARD_END(RNF_GROUP_NONE)
}

}  // extern "C"
