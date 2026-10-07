// SPDX-License-Identifier: GPL-2.0-or-later
#include "osk.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>
#include <tuple>

namespace rnl {

namespace {

OskKey ch(const char* t, const char* s = "") {
  OskKey k;
  k.text = t;
  k.shifted = s;
  return k;
}

OskKey special(OskKey::Kind kind, float w) {
  OskKey k;
  k.kind = kind;
  k.width = w;
  return k;
}

std::vector<OskKey> chars(std::initializer_list<std::pair<const char*, const char*>> l) {
  std::vector<OskKey> r;
  for (const auto& [t, s] : l) r.push_back(ch(t, s));
  return r;
}

std::string upper(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return r;
}

float center(const std::vector<OskKey>& row, int col) {
  float x = 0;
  for (int i = 0; i < col; ++i) x += row[size_t(i)].width;
  return x + row[size_t(col)].width * 0.5f;
}

// One UTF-8 character at s[i] (its byte length).
size_t utf8Len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

}  // namespace

void OskModel::build() {
  using K = OskKey::Kind;
  auto bottom = [&](bool symbols) {
    std::vector<OskKey> r;
    r.push_back(special(K::symbols, 1.5f));
    if (steamKey_) r.push_back(special(K::steam, 2.0f));
    r.push_back(special(K::space, steamKey_ ? 3.5f : 5.5f));
    r.push_back(special(K::left, 1.0f));
    r.push_back(special(K::right, 1.0f));
    r.push_back(special(K::done, 2.0f));
    (void)symbols;
    return r;
  };
  std::vector<OskKey> digits = chars({{"1", "!"}, {"2", "@"}, {"3", "#"}, {"4", "$"}, {"5", "%"}, {"6", "^"},
                                      {"7", "&"}, {"8", "*"}, {"9", "("}, {"0", ")"}, {"-", "_"}});
  letterRows_.clear();
  letterRows_.push_back(digits);
  letterRows_.push_back(chars({{"q", ""}, {"w", ""}, {"e", ""}, {"r", ""}, {"t", ""}, {"y", ""}, {"u", ""}, {"i", ""},
                               {"o", ""}, {"p", ""}, {"'", "\""}}));
  letterRows_.push_back(chars({{"a", ""}, {"s", ""}, {"d", ""}, {"f", ""}, {"g", ""}, {"h", ""}, {"j", ""}, {"k", ""},
                               {"l", ""}, {",", "<"}, {".", ">"}}));
  std::vector<OskKey> r3;
  r3.push_back(special(K::shift, 1.5f));
  for (const char* c : {"z", "x", "c", "v", "b", "n", "m"}) r3.push_back(ch(c));
  r3.push_back(ch("/", "?"));
  r3.push_back(special(K::backspace, 1.5f));
  letterRows_.push_back(r3);
  letterRows_.push_back(bottom(false));

  symbolRows_.clear();
  symbolRows_.push_back(chars({{"1", ""}, {"2", ""}, {"3", ""}, {"4", ""}, {"5", ""}, {"6", ""}, {"7", ""}, {"8", ""},
                               {"9", ""}, {"0", ""}, {"-", ""}}));
  symbolRows_.push_back(chars({{"!", ""}, {"@", ""}, {"#", ""}, {"$", ""}, {"%", ""}, {"^", ""}, {"&", ""}, {"*", ""},
                               {"(", ""}, {")", ""}, {"_", ""}}));
  symbolRows_.push_back(chars({{"+", ""}, {"=", ""}, {"/", ""}, {"\\", ""}, {"|", ""}, {"~", ""}, {"`", ""}, {"\"", ""},
                               {"'", ""}, {":", ""}, {";", ""}}));
  std::vector<OskKey> s3;
  for (const char* c : {"[", "]", "{", "}", "<", ">", "?", ",", "."}) s3.push_back(ch(c));
  s3.push_back(special(K::backspace, 2.0f));
  symbolRows_.push_back(s3);
  symbolRows_.push_back(bottom(true));
}

void OskModel::reset(bool steamKey) {
  steamKey_ = steamKey;
  build();
  symbols_ = false;
  shift_ = Shift::off;
  row_ = 1;
  col_ = 0;
}

std::string OskModel::keyText(const OskKey& k) const {
  if (k.kind != OskKey::Kind::character) return {};
  if (shift_ == Shift::off || symbols_) return k.text;
  return k.shifted.empty() ? upper(k.text) : k.shifted;
}

void OskModel::focus(int row, int col) {
  const auto& rs = rows();
  row_ = std::clamp(row, 0, int(rs.size()) - 1);
  col_ = std::clamp(col, 0, int(rs[size_t(row_)].size()) - 1);
}

void OskModel::move(int dr, int dc) {
  const auto& rs = rows();
  if (dc != 0) {
    int n = int(rs[size_t(row_)].size());
    col_ = (col_ + dc + n) % n;
    return;
  }
  // Up / down: the key of the next row nearest to this one's center (wrapping top <-> bottom).
  float x = center(rs[size_t(row_)], col_);
  int nr = (row_ + dr + int(rs.size())) % int(rs.size());
  const auto& next = rs[size_t(nr)];
  int best = 0;
  float bestD = 1e9f;
  for (int i = 0; i < int(next.size()); ++i) {
    float d = std::fabs(center(next, i) - x);
    if (d < bestD - 1e-4f) {
      bestD = d;
      best = i;
    }
  }
  row_ = nr;
  col_ = best;
}

OskAction OskModel::activate(int row, int col) {
  focus(row, col);
  const OskKey& k = rows()[size_t(row_)][size_t(col_)];
  OskAction a;
  using K = OskKey::Kind;
  switch (k.kind) {
    case K::character:
      a.op = OskAction::Op::insert;
      a.text = keyText(k);
      if (shift_ == Shift::once) shift_ = Shift::off;
      break;
    case K::shift: shift_ = shift_ == Shift::off ? Shift::once : shift_ == Shift::once ? Shift::locked : Shift::off; break;
    case K::symbols: {
      symbols_ = !symbols_;
      focus(row_, col_);  // same bottom row on both pages
      break;
    }
    case K::space: a.op = OskAction::Op::insert; a.text = " "; break;
    case K::backspace: a.op = OskAction::Op::backspace; break;
    case K::left: a.op = OskAction::Op::cursorLeft; break;
    case K::right: a.op = OskAction::Op::cursorRight; break;
    case K::done: a.op = OskAction::Op::done; break;
    case K::steam: a.op = OskAction::Op::steam; break;
  }
  return a;
}

OskAction OskModel::press(OskButton b, bool textEmpty) {
  OskAction a;
  switch (b) {
    case OskButton::up: move(-1, 0); break;
    case OskButton::down: move(1, 0); break;
    case OskButton::left: move(0, -1); break;
    case OskButton::right: move(0, 1); break;
    case OskButton::a: return activate(row_, col_);
    case OskButton::b: a.op = textEmpty ? OskAction::Op::cancel : OskAction::Op::backspace; break;
    case OskButton::x: a.op = OskAction::Op::insert; a.text = " "; break;
    case OskButton::y: shift_ = shift_ == Shift::off ? Shift::once : shift_ == Shift::once ? Shift::locked : Shift::off; break;
    case OskButton::start: a.op = OskAction::Op::done; break;
    case OskButton::l1: a.op = OskAction::Op::cursorLeft; break;
    case OskButton::r1: a.op = OskAction::Op::cursorRight; break;
    case OskButton::view:
      symbols_ = !symbols_;
      focus(row_, col_);
      break;
  }
  return a;
}

bool OskModel::find(const std::string& c, bool* symbols, bool* shifted, int* row, int* col) const {
  // Prefer the letters page without Shift, then with Shift, then the symbols page.
  for (int pass = 0; pass < 3; ++pass) {
    const auto& rs = pass < 2 ? letterRows_ : symbolRows_;
    for (int r = 0; r < int(rs.size()); ++r)
      for (int i = 0; i < int(rs[size_t(r)].size()); ++i) {
        const OskKey& k = rs[size_t(r)][size_t(i)];
        if (k.kind != OskKey::Kind::character) continue;
        std::string t = pass == 1 ? (k.shifted.empty() ? upper(k.text) : k.shifted) : k.text;
        if (t != c) continue;
        *symbols = pass == 2;
        *shifted = pass == 1;
        *row = r;
        *col = i;
        return true;
      }
  }
  return false;
}

std::vector<OskButton> OskModel::pressesFor(const std::string& text) const {
  OskModel m = *this;
  std::vector<OskButton> out;
  for (size_t i = 0; i < text.size();) {
    size_t n = std::min(text.size() - i, utf8Len((unsigned char)text[i]));
    std::string c = text.substr(i, n);
    i += n;
    if (c == " ") {
      out.push_back(OskButton::x);
      continue;
    }
    bool symbols = false, shifted = false;
    int r = 0, k = 0;
    if (!m.find(c, &symbols, &shifted, &r, &k)) return {};
    if (m.symbols_ != symbols) {
      m.press(OskButton::view, false);
      out.push_back(OskButton::view);
    }
    bool shiftOn = m.shift_ != Shift::off;
    if (!symbols && shiftOn != shifted) {
      // Y cycles off -> once -> locked -> off.
      while ((m.shift_ != Shift::off) != shifted || m.shift_ == Shift::locked) {
        m.press(OskButton::y, false);
        out.push_back(OskButton::y);
        if (shifted && m.shift_ == Shift::once) break;
      }
    }
    // Breadth-first search over the focus positions (moves are deterministic).
    using P = std::pair<int, int>;
    std::deque<std::pair<P, std::vector<OskButton>>> q;
    std::set<P> seen;
    q.push_back({{m.row_, m.col_}, {}});
    seen.insert({m.row_, m.col_});
    std::vector<OskButton> path;
    bool found = false;
    while (!q.empty()) {
      auto [p, moves] = q.front();
      q.pop_front();
      if (p == P{r, k}) {
        path = moves;
        found = true;
        break;
      }
      for (OskButton d : {OskButton::up, OskButton::down, OskButton::left, OskButton::right}) {
        OskModel t = m;
        t.row_ = p.first;
        t.col_ = p.second;
        t.press(d, false);
        P np{t.row_, t.col_};
        if (seen.insert(np).second) {
          auto nm = moves;
          nm.push_back(d);
          q.push_back({np, nm});
        }
      }
    }
    if (!found) return {};
    for (OskButton d : path) {
      m.press(d, false);
      out.push_back(d);
    }
    m.press(OskButton::a, false);
    out.push_back(OskButton::a);
  }
  return out;
}

// ------------------------------------------------------------------ repeat

void OskRepeater::set(OskButton b, bool down, double now) {
  held_.erase(std::remove_if(held_.begin(), held_.end(), [b](const Held& h) { return h.button == b; }), held_.end());
  if (down) held_.push_back({b, now + kDelay});
}

std::vector<OskButton> OskRepeater::due(double now) {
  std::vector<OskButton> r;
  for (Held& h : held_)
    while (now >= h.next) {
      r.push_back(h.button);
      h.next += kRate;
    }
  return r;
}

// ------------------------------------------------------------------ which keyboard

TextEntry textEntryFor(const TextEntryContext& c) {
  if (c.mode == "builtin") return TextEntry::builtin;
  if (c.mode == "steam") return c.steamAvailable ? TextEntry::steam : TextEntry::external;
  // Auto: a physical keyboard in use (Desktop Mode / a PC) needs none; otherwise Steam's keyboard
  // when it can be asked for (it can type Japanese), else the built-in one.
  if (c.lastInputKeyboardOrMouse && !c.gamingMode) return TextEntry::external;
  return c.steamAvailable ? TextEntry::steam : TextEntry::builtin;
}

}  // namespace rnl
