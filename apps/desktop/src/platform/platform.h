// Platform services of the desktop frontend (the small seam besides Renderer, renderer.h):
//   Linux / macOS (tests)  platform_xdg.cpp      XDG folders, freedesktop.org Trash, gamescope
//   Windows                platform_windows.cpp  Known Folders, Recycle Bin (IFileOperation)
// Also per platform: Paths::standard() / Paths::display() (paths.h), the host clock
// (host_clock.h), the UI fonts (fonts.h: apps/linux fontconfig, apps/windows %WINDIR%\Fonts).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

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
/// Opens a file with its default application (a made video). App library only (platform/shell.cpp).
bool openFile(const std::string& path);
/// Add ROMs: the system file picker, the chosen files copied into `dir` (Android: RomImport, which says how
/// it went). false where there is no such picker; canImportFiles() says so before (platform/shell.cpp).
bool canImportFiles();
bool importFiles(const std::string& dir);
/// Share Log: the app's log file to another app (Android: the system's share sheet). false elsewhere.
bool canShareLog();
bool shareLog();
/// Where Share and Open with put ROMs (Android: RomImport); the library's ROM folder, also after a change.
void setImportFolder(const std::string& dir);
/// Change… for the ROM / Projects folder: the system's folder picker (Windows; Android after asking for
/// all-files access), its answer later through takeChosenFolder. false where the app's own chooser picks
/// (Linux, the Steam Deck's Gaming Mode: a gamepad drives it).
bool chooseFolder(const std::string& start);
/// The picker's answer, once: true with *path (empty when cancelled; *error says why when it failed).
bool takeChosenFolder(std::string* path, std::string* error);

/// Search the Device (Add ROMs, rom_finder.h): the storage the app may walk (Android: the device's own and
/// every card / USB drive under /storage), and the folders in them it can never read (*skip: Android/data,
/// Android/obb). Empty where there is no such search.
std::vector<std::string> deviceStorageRoots(std::vector<std::string>* skip);
/// Whether the whole storage may be read now (Android: all-files access; Android 10: the storage permission).
bool storageAccessGranted();
/// Asks for it; the answer later through takeStorageAccessAnswer (Android: the system's page for the app).
void requestStorageAccess();
bool takeStorageAccessAnswer(bool* granted);

/// Running inside Steam's Gaming Mode (gamescope session, Steam Deck): full screen by default,
/// Steam's on-screen keyboard. Always false on Windows.
bool gamingMode();

}  // namespace rnl
