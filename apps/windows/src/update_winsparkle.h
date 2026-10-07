// In-app updates on Windows through WinSparkle (https://winsparkle.org, MIT; WinSparkle.dll next to
// ReplayNES.exe, loaded at run time - without it there are no in-app updates): the same appcast
// format and EdDSA (ed25519) signatures as Sparkle on macOS, with the release's own feed
// appcast-windows.xml (one item per version, one enclosure per architecture: sparkle:os
// "windows-x64" / "windows-arm64"; scripts/release-windows.sh). WinSparkle checks (automatically
// once a day if the user agreed - it asks on the second launch, as Sparkle does - or with "Check
// for Updates…"), shows its own dialogs, downloads the zip and verifies its
// signature with the public key built in (SUPublicEDKey of apps/macos/project.yml, or the CMake
// cache variable RNW_UPDATE_PUBLIC_KEY). Installing a zip is ours (WinSparkle runs installers):
//   1. the zip is extracted with Windows' tar.exe into %LOCALAPPDATA%\ReplayNES\Update\<n>;
//   2. its ReplayNES.exe is started as `--finish-update <install dir> <pid> -- <arguments>`;
//   3. WinSparkle asks the app to quit (it saves the session as usual);
//   4. the helper waits for the old process, copies the new files over the installation (with a
//      backup it restores on failure) and starts the updated ReplayNES with the same arguments.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "update_service.h"

namespace rnl {

/// nullptr when WinSparkle.dll is missing (developer builds) or the build has no public key.
std::unique_ptr<UpdateService> makeWinSparkleUpdates(const std::vector<std::string>& argv);

/// `ReplayNES.exe --finish-update <install dir> <pid> [-- args...]` (step 4 above). Exit code.
int finishUpdate(int argc, char** argv);

}  // namespace rnl
