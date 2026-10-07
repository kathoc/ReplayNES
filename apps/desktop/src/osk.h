// The built-in on-screen keyboard's model (no ImGui / SDL; ui_osk.cpp draws it and feeds it the
// controller): a QWERTY page and a symbols page on a grid of 11 key units per row, the focused key,
// Shift (off / once / locked) and what a press does. Controller: D-pad / left stick move (wrapping),
// A types the focused key, B deletes (on an empty field: closes the keyboard), X space, Y Shift,
// L1 / R1 move the text cursor, Menu (≡) done, View (⧉) switches letters / symbols.
// Text fields choose between it and Steam's keyboard with Settings -> On-screen keyboard
// (textEntryFor: Auto, Built-in, Steam).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

namespace rnl {

struct OskKey {
  enum class Kind { character, shift, symbols, space, backspace, left, right, done, steam };
  Kind kind = Kind::character;
  std::string text;     // a character key's text
  std::string shifted;  // with Shift ("" = the same)
  float width = 1.0f;   // in key units (every row is 11 units wide)
};

enum class OskButton { up, down, left, right, a, b, x, y, start, l1, r1, view };

struct OskAction {
  enum class Op { none, insert, backspace, cursorLeft, cursorRight, done, cancel, steam };
  Op op = Op::none;
  std::string text;  // insert
};

class OskModel {
 public:
  enum class Shift { off, once, locked };
  static constexpr float kRowUnits = 11.0f;

  /// steamKey: the bottom row has a "Steam Keyboard" key (Steam's keyboard can be asked for).
  explicit OskModel(bool steamKey = false) { reset(steamKey); }
  /// Letters, Shift off, the focus on "q".
  void reset(bool steamKey);

  const std::vector<std::vector<OskKey>>& rows() const { return symbols_ ? symbolRows_ : letterRows_; }
  int row() const { return row_; }
  int col() const { return col_; }
  bool symbolsPage() const { return symbols_; }
  Shift shift() const { return shift_; }
  bool hasSteamKey() const { return steamKey_; }
  /// What a character key types now (Shift applied).
  std::string keyText(const OskKey& k) const;

  /// A controller press. textEmpty: the field is empty (B then closes the keyboard).
  OskAction press(OskButton b, bool textEmpty);
  /// The key at (row, col) chosen (A, or a tap: Delete there never closes).
  OskAction activate(int row, int col);
  void focus(int row, int col);

  /// The presses that type `text` from the current state (scripts, tests). Empty when a
  /// character is not on the keyboard.
  std::vector<OskButton> pressesFor(const std::string& text) const;

 private:
  void move(int dr, int dc);
  void build();
  bool find(const std::string& ch, bool* symbols, bool* shifted, int* row, int* col) const;

  std::vector<std::vector<OskKey>> letterRows_, symbolRows_;
  bool steamKey_ = false;
  bool symbols_ = false;
  Shift shift_ = Shift::off;
  int row_ = 1, col_ = 0;
};

/// Auto-repeat of held D-pad directions / B (backspace): first repeat after kDelay, then every kRate.
class OskRepeater {
 public:
  static constexpr double kDelay = 0.40, kRate = 0.075;
  void set(OskButton b, bool down, double now);
  /// The repeats due at `now`.
  std::vector<OskButton> due(double now);
  void clear() { held_.clear(); }

 private:
  struct Held {
    OskButton button;
    double next;
  };
  std::vector<Held> held_;
};

/// Settings -> On-screen keyboard ("auto", "builtin", "steam") and what a text field that becomes
/// active gets.
enum class TextEntry { none, builtin, steam, external };
struct TextEntryContext {
  std::string mode = "auto";
  bool steamAvailable = false;  // Steam's keyboard can be asked for (SDL_HasScreenKeyboardSupport)
  bool gamingMode = false;      // gamescope: no physical keyboard to expect
  bool lastInputKeyboardOrMouse = false;  // the field was activated with a keyboard / mouse
};
/// builtin / steam (Steam's keyboard first; a controller press that still reaches ReplayNES then
/// brings the built-in one in Auto) or external (a physical keyboard is in use: nothing shown).
TextEntry textEntryFor(const TextEntryContext& c);

}  // namespace rnl
