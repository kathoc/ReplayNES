// On-screen keyboards for text fields (search, names): which one an active text field gets
// (Settings -> On-screen keyboard, osk.h textEntryFor), the built-in controller keyboard (drawn
// here, the model in osk.h) and asking Steam for its keyboard.
//
// Steam's keyboard: SDL's text input asks Steam for it (SDL_StartTextInput opens
// steam://open/keyboard on the Steam Deck when SDL_HINT_ENABLE_SCREEN_KEYBOARD is on); the hint is
// on only while a field uses Steam's keyboard, so the built-in one never comes with Steam's. While
// Steam's keyboard is up Steam Input has the controller (its config switches to Steam's UI), so a
// press that still reaches ReplayNES means it is not up - observed on a Deck: Steam received the
// request but showed nothing. Such a press brings the built-in keyboard (Auto) or asks again
// (Steam); B / Menu (≡) end the input keeping the text.
//
// The built-in keyboard types through ImGui's input queue (characters, Backspace, ←/→, Enter,
// Escape), as a hardware keyboard would, so every InputText works unchanged. While a text field
// has the controller PadNavFeed feeds ImGui nothing (ImGui's own gamepad navigation would leave the
// field: B reverts it, the D-pad moves the focus away).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "imgui_internal.h"
#include "l10n.h"
#include "platform/platform.h"
#include "settings.h"
#include "ui.h"

namespace rnl {

namespace {

bool oskButton(uint8_t b, OskButton* out) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: *out = OskButton::up; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: *out = OskButton::down; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: *out = OskButton::left; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: *out = OskButton::right; return true;
    case SDL_GAMEPAD_BUTTON_SOUTH: *out = OskButton::a; return true;
    case SDL_GAMEPAD_BUTTON_EAST: *out = OskButton::b; return true;
    case SDL_GAMEPAD_BUTTON_WEST: *out = OskButton::x; return true;
    case SDL_GAMEPAD_BUTTON_NORTH: *out = OskButton::y; return true;
    case SDL_GAMEPAD_BUTTON_START: *out = OskButton::start; return true;
    case SDL_GAMEPAD_BUTTON_BACK: *out = OskButton::view; return true;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: *out = OskButton::l1; return true;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: *out = OskButton::r1; return true;
    default: return false;
  }
}

bool repeats(OskButton b) {
  return b == OskButton::up || b == OskButton::down || b == OskButton::left || b == OskButton::right ||
         b == OskButton::b || b == OskButton::l1 || b == OskButton::r1;
}

const char* buttonName(OskButton b) {
  switch (b) {
    case OskButton::up: return "up";
    case OskButton::down: return "down";
    case OskButton::left: return "left";
    case OskButton::right: return "right";
    case OskButton::a: return "a";
    case OskButton::b: return "b";
    case OskButton::x: return "x";
    case OskButton::y: return "y";
    case OskButton::start: return "menu";
    case OskButton::l1: return "l1";
    case OskButton::r1: return "r1";
    case OskButton::view: return "view";
  }
  return "";
}

}  // namespace

const char* UI::textEntryName() const {
  switch (textEntry_) {
    case TextEntry::none: return "none";
    case TextEntry::builtin: return "builtin";
    case TextEntry::steam: return "steam";
    case TextEntry::external: return "external";
  }
  return "";
}

std::vector<std::string> UI::oskPresses(const std::string& text) const {
  std::vector<std::string> r;
  for (OskButton b : osk_.pressesFor(text)) r.push_back(buttonName(b));
  return r;
}

bool UI::textFieldEmpty() const {
  ImGuiInputTextState* s = ImGui::GetInputTextState(ImGui::GetActiveID());
  return !s || s->TextLen == 0;
}

void UI::sendKey(ImGuiKey k) {
  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(k, true);
  io.AddKeyEvent(k, false);
}

void UI::applyOsk(const OskAction& a) {
  switch (a.op) {
    case OskAction::Op::none: break;
    case OskAction::Op::insert: ImGui::GetIO().AddInputCharactersUTF8(a.text.c_str()); break;
    case OskAction::Op::backspace: sendKey(ImGuiKey_Backspace); break;
    case OskAction::Op::cursorLeft: sendKey(ImGuiKey_LeftArrow); break;
    case OskAction::Op::cursorRight: sendKey(ImGuiKey_RightArrow); break;
    case OskAction::Op::done: sendKey(ImGuiKey_Enter); break;
    case OskAction::Op::cancel: sendKey(ImGuiKey_Escape); break;  // restores the text from before
    case OskAction::Op::steam: requestSteamKeyboard(true); break;
  }
}

void UI::requestSteamKeyboard(bool show) {
  SDL_Window* w = d_.window;
  if (!w) return;
  // SDL hides Steam's keyboard on SDL_StopTextInput and asks for it on SDL_StartTextInput only
  // while the hint is on.
  SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");
  SDL_StopTextInput(w);
  if (!show) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
  SDL_StartTextInput(w);
  sdlOskHint_ = show ? 1 : 0;
  if (show) {
    textEntry_ = TextEntry::steam;
  } else {
    textEntry_ = TextEntry::builtin;
    osk_.reset(SDL_HasScreenKeyboardSupport());
  }
  oskRepeat_.clear();
  std::fprintf(stderr, "text entry: %s\n", textEntryName());
}

bool UI::textEntryEvent(const SDL_Event& e, double now) {
  if (!textEntryOwnsPad() || !ImGui::GetCurrentContext()) return false;
  const bool builtin = textEntry_ == TextEntry::builtin;
  // A press while Steam's keyboard was asked for: it is not up (see the top of this file).
  auto steamPress = [&](OskButton b) {
    if (b == OskButton::b || b == OskButton::start) sendKey(ImGuiKey_Enter);  // done, the text is kept
    else requestSteamKeyboard(d_.settings->onScreenKeyboard == "steam");     // ask again / the built-in one
  };
  auto press = [&](OskButton b) {
    if (!builtin) return steamPress(b);
    applyOsk(osk_.press(b, textFieldEmpty()));
    if (repeats(b)) oskRepeat_.set(b, true, now);
  };
  switch (e.type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
      OskButton b;
      if (oskButton(e.gbutton.button, &b)) press(b);
      return true;  // other buttons (R3, Guide, paddles) do nothing while typing
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      OskButton b;
      if (oskButton(e.gbutton.button, &b)) oskRepeat_.set(b, false, now);
      return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      // The left stick moves like the D-pad (hysteresis as in PadNavFeed).
      if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) return true;
      float v = std::max(-1.0f, float(e.gaxis.value) / 32767.0f);
      bool x = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
      const int neg = x ? 0 : 2, pos = x ? 1 : 3;
      const OskButton dn = x ? OskButton::left : OskButton::up, dp = x ? OskButton::right : OskButton::down;
      for (int i : {neg, pos}) {
        float a = i == neg ? -v : v;
        bool on = oskStick_[i] ? a > 0.35f : a > 0.5f;
        if (on == oskStick_[i]) continue;
        oskStick_[i] = on;
        OskButton b = i == neg ? dn : dp;
        if (on) press(b);
        else oskRepeat_.set(b, false, now);
      }
      return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
      if (!builtin || e.button.button != SDL_BUTTON_LEFT) return false;
      float mx = e.button.x, my = e.button.y;
      if (mx < oskX0_ || mx > oskX1_ || my < oskY0_ || my > oskY1_) return false;
      oskTapDown_ = true;
      for (const OskKeyRect& k : oskKeys_)
        if (mx >= k.x0 && mx < k.x1 && my >= k.y0 && my < k.y1) applyOsk(osk_.activate(k.row, k.col));
      return true;
    }
    case SDL_EVENT_KEY_DOWN:
      // Typing on a hardware keyboard: Auto puts the built-in keyboard away.
      if (builtin && !e.key.repeat && d_.settings->onScreenKeyboard == "auto") {
        textEntry_ = TextEntry::external;
        oskX0_ = oskY0_ = oskX1_ = oskY1_ = 0;
      }
      return false;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (!oskTapDown_) return false;
      oskTapDown_ = false;
      return true;
    default: return false;
  }
}

void UI::updateTextEntry(double now) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImGuiID id = ImGui::GetInputTextState(g.ActiveId) ? g.ActiveId : 0;
  if (id != textEntryId_) {
    // SDL hides Steam's keyboard when ImGui stops text input (the end of this frame): the hint
    // stays on until then.
    if (textEntry_ == TextEntry::steam) steamClosingFrames_ = 2;
    textEntryId_ = id;
    textEntry_ = TextEntry::none;
    oskRepeat_.clear();
    for (bool& s : oskStick_) s = false;
    oskTapDown_ = false;
    if (id) {
      TextEntryContext c;
      c.mode = d_.settings->onScreenKeyboard;
      c.steamAvailable = SDL_HasScreenKeyboardSupport();
      c.gamingMode = gamingMode();
      c.lastInputKeyboardOrMouse = lastInputKeyboardOrMouse_;
      textEntry_ = textEntryFor(c);
      if (textEntry_ == TextEntry::builtin) osk_.reset(c.steamAvailable && c.mode == "auto");
      std::fprintf(stderr, "text entry: %s\n", textEntryName());
    }
  }
  // Held buttons repeat; a held B deletes up to the start of the field and never closes it.
  if (textEntry_ == TextEntry::builtin)
    for (OskButton b : oskRepeat_.due(now)) applyOsk(osk_.press(b, b != OskButton::b && textFieldEmpty()));
  // SDL_StartTextInput (ImGui's backend, at the end of this frame) asks Steam for its keyboard
  // only while this is on.
  int want = textEntry_ == TextEntry::steam || steamClosingFrames_ > 0 ? 1 : 0;
  if (steamClosingFrames_ > 0) --steamClosingFrames_;
  if (want != sdlOskHint_) {
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, want ? "1" : "0");
    sdlOskHint_ = want;
  }
  if (textEntry_ == TextEntry::builtin) buildOsk();
  else if (textEntry_ == TextEntry::steam) buildSteamKeyboardHint();
  else oskX0_ = oskY0_ = oskX1_ = oskY1_ = 0;
}

void UI::buildSteamKeyboardHint() {
  // Under the field: what to do when Steam's keyboard does not show up.
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImGuiIO& io = ImGui::GetIO();
  const char* text = d_.settings->onScreenKeyboard == "steam"
                         ? TR("Steam’s on-screen keyboard was asked for. No keyboard? Press A to ask again.")
                         : TR("Steam’s on-screen keyboard was asked for. No keyboard? Press A for the built-in one.");
  float fs = S(17);
  ImFont* font = ImGui::GetFont();
  ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, text);
  ImVec2 pad(S(12), S(6));
  float x = std::clamp(g.PlatformImeData.InputPos.x - S(8), S(8), std::max(S(8), io.DisplaySize.x - ts.x - pad.x * 2 - S(8)));
  float y = g.PlatformImeData.InputPos.y + g.PlatformImeData.InputLineHeight + S(10);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + ts.x + pad.x * 2, y + ts.y + pad.y * 2), IM_COL32(30, 32, 40, 235), S(8));
  dl->AddRect(ImVec2(x, y), ImVec2(x + ts.x + pad.x * 2, y + ts.y + pad.y * 2), IM_COL32(255, 200, 64, 200), S(8));
  dl->AddText(font, fs, ImVec2(x + pad.x, y + pad.y), IM_COL32(235, 238, 245, 255), text);
}

void UI::buildOsk() {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const auto& rows = osk_.rows();
  const float pad = S(12), keyH = S(54), gap = S(6), header = S(36);
  const float w = std::min(io.DisplaySize.x - S(16), S(1080));
  const float h = pad * 2 + header + keyH * float(rows.size()) + gap * float(rows.size() - 1);
  // At the bottom, or at the top when the field would be under it.
  float x0 = (io.DisplaySize.x - w) * 0.5f;
  float y0 = io.DisplaySize.y - h - S(6);
  float fieldBottom = g.PlatformImeData.InputPos.y + g.PlatformImeData.InputLineHeight + S(10);
  if (fieldBottom > y0) y0 = S(6);
  oskX0_ = x0;
  oskY0_ = y0;
  oskX1_ = x0 + w;
  oskY1_ = y0 + h;
  // Dim behind, the panel.
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(16, 18, 23, 248), S(12));
  dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(255, 255, 255, 40), S(12));
  // Header: the controller's buttons.
  {
    rnf_controller_family f = promptFamily();
    float gh = S(24), fs = S(16);
    ImVec2 p(x0 + pad, y0 + pad + (header - gh) * 0.5f - S(4));
    struct HP {
      const char* el[2];
      const char* text;
    };
    const HP items[] = {
        {{"face.south", nullptr}, TR("Type")},
        {{"face.east", nullptr}, TR("Delete")},
        {{"face.west", nullptr}, TR("Space")},
        {{"face.north", nullptr}, TR("Shift")},
        {{"leftShoulder", "rightShoulder"}, TR("Move Cursor")},
        {{"options", nullptr}, osk_.symbolsPage() ? "ABC" : "?123"},
        {{"menu", nullptr}, TR("Done")},
    };
    for (const HP& it : items) {
      for (const char* el : it.el)
        if (el) p.x += glyph(dl, p, el, f, gh) + S(4);
      p.x += S(2);
      ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, it.text);
      dl->AddText(font, fs, ImVec2(p.x, p.y + (gh - ts.y) * 0.5f), IM_COL32(205, 208, 216, 255), it.text);
      p.x += ts.x + S(18);
    }
  }
  // Keys.
  oskKeys_.clear();
  const float unit = (w - pad * 2) / OskModel::kRowUnits;
  float y = y0 + pad + header;
  for (int r = 0; r < int(rows.size()); ++r) {
    float x = x0 + pad;
    for (int c = 0; c < int(rows[size_t(r)].size()); ++c) {
      const OskKey& k = rows[size_t(r)][size_t(c)];
      float kw = unit * k.width;
      ImVec2 a(x + gap * 0.5f, y), b(x + kw - gap * 0.5f, y + keyH);
      bool focused = r == osk_.row() && c == osk_.col();
      using K = OskKey::Kind;
      bool special = k.kind != K::character && k.kind != K::space;
      ImU32 bg = special ? IM_COL32(44, 48, 58, 255) : IM_COL32(58, 63, 76, 255);
      if (k.kind == K::done) bg = IM_COL32(40, 100, 200, 255);
      if (k.kind == K::shift && osk_.shift() != OskModel::Shift::off)
        bg = osk_.shift() == OskModel::Shift::locked ? IM_COL32(230, 150, 30, 255) : IM_COL32(150, 110, 40, 255);
      if (focused) bg = IM_COL32(96, 110, 140, 255);
      dl->AddRectFilled(a, b, bg, S(7));
      std::string label;
      switch (k.kind) {
        case K::character: label = osk_.keyText(k); break;
        case K::shift: label = TR("Shift"); break;
        case K::symbols: label = osk_.symbolsPage() ? "ABC" : "?123"; break;
        case K::space: label = TR("Space"); break;
        case K::backspace: label = TR("Delete"); break;
        case K::left: label = "◀"; break;
        case K::right: label = "▶"; break;
        case K::done: label = TR("Done"); break;
        case K::steam: label = TR("Steam Keyboard"); break;
      }
      float fs = k.kind == K::character ? S(25) : S(18);
      ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
      if (ts.x > b.x - a.x - S(8)) {  // long labels (Japanese / the Steam key) shrink to fit
        fs *= (b.x - a.x - S(8)) / ts.x;
        ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str());
      }
      dl->AddText(font, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), IM_COL32(240, 242, 248, 255),
                  label.c_str());
      if (k.kind == K::shift && osk_.shift() == OskModel::Shift::locked)
        dl->AddRectFilled(ImVec2(a.x + S(14), b.y - S(8)), ImVec2(b.x - S(14), b.y - S(5)), IM_COL32(255, 255, 255, 230), S(2));
      if (focused) {
        float t = std::max(2.0f, S(3));
        dl->AddRect(ImVec2(a.x - t, a.y - t), ImVec2(b.x + t, b.y + t), IM_COL32(255, 200, 64, 255), S(9), 0, t);
      }
      oskKeys_.push_back({r, c, a.x, a.y, b.x, b.y});
      x += kw;
    }
    y += keyH + gap;
  }
}

}  // namespace rnl
