// Moving a file or folder to the user's Trash per the freedesktop.org Trash specification 1.0
// (home trash: $XDG_DATA_HOME/Trash/{files,info}; <name>.trashinfo holds the URL-escaped original
// path and the deletion date, created with O_EXCL first). Used for the "Reset Project" backup and
// when Save As replaces an existing project (the macOS app uses FileManager.trashItem).
// Inside the Flatpak the host's trash is $HOME/.local/share/Trash (HOST_XDG_DATA_HOME), which the
// manifest grants (--filesystem=xdg-data/Trash).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace rnl {

/// The home trash folder: $HOST_XDG_DATA_HOME, else ($FLATPAK_ID set) $HOME/.local/share, else
/// $XDG_DATA_HOME, else $HOME/.local/share; + "/Trash".
std::string homeTrashDir();

/// RFC 2396 escaping of an absolute path for "Path=" (unreserved characters and '/' stay).
std::string trashEscapePath(const std::string& absolutePath);
/// Contents of a .trashinfo file. date: seconds since 1970 (written as local time
/// YYYY-MM-DDThh:mm:ss, as the specification asks).
std::string trashInfoContents(const std::string& absolutePath, double date);
/// "<name>", "<name> 2", "<name> 3" ... (the suffix goes before the extension of a file name).
std::string trashCandidateName(const std::string& name, int n);

/// Moves path into trashDir (created if missing). On success *trashedAs receives the path inside
/// files/. Returns false with *error on failure (nothing is left half-done: a partial copy is
/// removed and the .trashinfo is deleted again).
bool moveToTrash(const std::string& path, const std::string& trashDir, double date, std::string* trashedAs,
                 std::string* error);

/// Recursive copy of a file or folder (fails if `to` exists).
bool copyTree(const std::string& from, const std::string& to, std::string* error);

}  // namespace rnl
