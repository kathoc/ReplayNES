// Localized UI strings of the Linux frontend: the shared table generated from
// apps/macos/Resources/Localizable.xcstrings (rnf_l10n_*). Keys are the English source strings.
// Every literal passed to TR / TRF must be written inline (scripts/check-l10n.py extracts
// TR("...") and TRF("...") from apps/linux/src and checks them against the catalog).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "replaynes/frontend.h"

namespace rnl {

/// One format argument for TRF (Apple-style specifiers: %@ string, %lld int, %llu uint, %.1f double).
struct L10nArg {
  rnf_arg a{};
  std::string keep;  // owns the string of a %@ argument
  L10nArg(const char* s) : keep(s ? s : "") { a.type = RNF_ARG_STRING; }
  L10nArg(const std::string& s) : keep(s) { a.type = RNF_ARG_STRING; }
  L10nArg(int v) { a.type = RNF_ARG_INT; a.i = v; }
  L10nArg(long v) { a.type = RNF_ARG_INT; a.i = v; }
  L10nArg(long long v) { a.type = RNF_ARG_INT; a.i = v; }
  L10nArg(unsigned v) { a.type = RNF_ARG_UINT; a.u = v; }
  L10nArg(unsigned long v) { a.type = RNF_ARG_UINT; a.u = v; }
  L10nArg(unsigned long long v) { a.type = RNF_ARG_UINT; a.u = v; }
  L10nArg(double v) { a.type = RNF_ARG_DOUBLE; a.d = v; }
};

/// Localized text of an English key (static storage). Variation selectors (U+FE0E / U+FE0F, as in
/// "▶︎") are removed: the UI font has no glyph for them.
const char* TR(const char* key);
/// Localized format with arguments.
std::string TRF(const char* key, std::initializer_list<L10nArg> args);

/// Language choice from the system's preferred locales (SDL), "ja" or "en"; REPLAYNES_LANG
/// overrides it (tests, screenshots).
std::string chooseUILanguage(const std::vector<std::string>& preferred, const char* override_);

/// "mm:ss.ss" of a frame count (Engine.timecode on macOS).
std::string timecode(uint64_t frame);

/// "yyyy-MM-dd HH:mm" in local time of seconds since 1970.
std::string localDateTime(double seconds);

}  // namespace rnl
