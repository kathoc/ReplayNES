// Platform services of the desktop frontend (the small seam besides Renderer, renderer.h):
//   Linux / macOS (tests)  platform_xdg.cpp      XDG folders, freedesktop.org Trash, gamescope
//   Windows                platform_windows.cpp  Known Folders, Recycle Bin (IFileOperation)
// Also per platform: Paths::standard() / Paths::display() (paths.h), the host clock
// (host_clock.h), the UI fonts (fonts.h: apps/linux fontconfig, apps/windows %WINDIR%\Fonts).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace rnl {

/// Moves a file or folder to the user's Trash (Linux: freedesktop.org home trash, also the host's
/// from inside the Flatpak) / Recycle Bin (Windows). Used for the "Reset Project" backup and when
/// Save As replaces a project. Never deletes permanently without asking (Windows shows the system's
/// "delete permanently?" warning for a drive without a Recycle Bin). false + *error on failure.
bool trashItem(const std::string& path, std::string* error);

/// Recursive copy of a file or folder (fails if `to` exists; a partial copy is removed).
bool copyTree(const std::string& from, const std::string& to, std::string* error);

/// Shows a folder in the file manager (Explorer on Windows). App library only (platform/shell.cpp).
bool openFolder(const std::string& dir);

/// Running inside Steam's Gaming Mode (gamescope session, Steam Deck): full screen by default,
/// Steam's on-screen keyboard. Always false on Windows.
bool gamingMode();

}  // namespace rnl
