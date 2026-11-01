// Steam's screen dimming and sleep held off while the app runs (Steam Deck game mode). SDL asks the
// desktop portal's Inhibit from a Flatpak, which game mode's portal does not have, so Steam was never
// told; Steam itself serves org.freedesktop.ScreenSaver on the session bus there, and is asked directly
// (the manifest's --talk-name=org.freedesktop.ScreenSaver).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace steam {

/// Inhibit until the process exits (the bus connection is kept open). False when it could not.
bool inhibitScreensaver();

}  // namespace steam
