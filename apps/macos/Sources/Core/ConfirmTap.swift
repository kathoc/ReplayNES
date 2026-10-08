// "A = resume" on the paused seek bar (docs/design/UI_REDESIGN.md), as a tap: the confirm button
// pressed while paused still reaches the game (held for a frame advance, e.g. hold B and step with
// the D-pad), and resumes on its release only when nothing else was pressed or stepped while it was
// held. Same rule as the desktop frontends (apps/desktop/src/ui_logic.h, rnl::ConfirmTap).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct ConfirmTap {
    private var held: [String: Bool] = [:]   // id -> still a tap

    /// Confirm pressed while paused (call after cancel() for this press).
    mutating func press(_ id: String) { held[id] = true }
    /// Another input pressed / a frame step: the confirm buttons held now are no longer taps.
    mutating func cancel() { for k in held.keys { held[k] = false } }
    /// Released: true = it was a tap (resume).
    mutating func release(_ id: String) -> Bool { held.removeValue(forKey: id) ?? false }
    mutating func clear() { held.removeAll() }
}
