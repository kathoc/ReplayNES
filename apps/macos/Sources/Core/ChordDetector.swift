// The Quick Menu's L+R (docs/design/UI_REDESIGN.md, "Input model") through the shared core's chord
// detector (frontend/include/replaynes/frontend.h, rnf_chord_*), so every frontend decides the chord
// the same way: both members of a combo bound to "hk.menu" (default "gc0:leftShoulder+gc0:rightShoulder")
// down within RNF_CHORD_WINDOW (either order) = the chord; a member alone acts on its RELEASE
// (`alone(down: false)`: pause / slow in play, a page in menus, a frame step while paused);
// `alone(down: true)` only says "held alone" (a member bound to a game button is held from there).
// Repeat mode (paused frame steps): a member held alone fires `repeated` after 400 ms, then every 50 ms.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum ChordEvent: Equatable {
    /// Both members down: the combo's action (the Quick Menu). `id` = the combo id ("a+b").
    case comboDown(String)
    /// Both released after comboDown.
    case comboUp(String)
    /// A member on its own: held alone (down) / released (up: the single press's trigger).
    /// `member`: 0 = the combo's first id (L), 1 = the second (R). `repeats`: on the release, the
    /// repeats fired during this hold (the single action is skipped when > 0).
    case alone(String, down: Bool, member: Int, repeats: Int)
    /// Repeat mode: held alone >= RNF_CHORD_HOLD_DELAY (and every RNF_CHORD_REPEAT_INTERVAL).
    case repeated(String, member: Int)
}

/// Thin wrapper over rnf_chord. Not thread-safe: use it from one queue. Times are seconds on one
/// monotonic clock.
final class ChordDetector {
    private let handle: OpaquePointer

    /// Both members within this many seconds = the chord.
    static let window = Double(RNF_CHORD_WINDOW)

    init(window: Double = 0) { handle = rnf_chord_new(window)! }
    deinit { rnf_chord_free(handle) }

    /// Combos from a binding table: every binding of `action` whose input is a combo id. Drops all
    /// state (nothing fires for members held now). Returns the number of combos.
    @discardableResult
    func configure(_ bindings: [(input: String, action: String)], action: String = "hk.menu") -> Int {
        withBindings(bindings) { b, n in Int(rnf_chord_configure(handle, b, n, action)) }
    }

    var comboCount: Int { Int(rnf_chord_combo_count(handle)) }
    func isMember(_ id: String) -> Bool { rnf_chord_is_member(handle, id) != 0 }

    /// A press / release. `taken` = id is a combo member (do not pass it on: its outcome is in
    /// `events`); otherwise `events` holds only members that timed out before this moment.
    func feed(_ id: String, down: Bool, now: Double) -> (taken: Bool, events: [ChordEvent]) {
        let taken = rnf_chord_feed(handle, id, down ? 1 : 0, now) != 0
        return (taken, poll())
    }

    /// Members held alone for the whole window and the repeats fire (call at `nextDeadline`).
    func tick(now: Double) -> [ChordEvent] {
        rnf_chord_tick(handle, now)
        return poll()
    }

    /// When `tick` has something to do (nil: nothing pending).
    var nextDeadline: Double? {
        let d = rnf_chord_deadline(handle)
        return d > 0 ? d : nil
    }

    /// Forget everything held / pending without events (controller detached).
    func reset() { rnf_chord_reset(handle) }

    /// Repeat mode (members held alone fire `repeated`): on only while paused in play.
    var repeatMode: Bool {
        get { rnf_chord_repeat(handle) != 0 }
        set { rnf_chord_set_repeat(handle, newValue ? 1 : 0) }
    }

    private func poll() -> [ChordEvent] {
        var out: [ChordEvent] = []
        var e = rnf_chord_event()
        while rnf_chord_poll(handle, &e) != 0 {
            let id = String(cString: e.input)
            switch e.kind {
            case RNF_CHORD_COMBO_DOWN: out.append(.comboDown(id))
            case RNF_CHORD_COMBO_UP: out.append(.comboUp(id))
            case RNF_CHORD_ALONE_DOWN: out.append(.alone(id, down: true, member: Int(e.member), repeats: Int(e.repeats)))
            case RNF_CHORD_ALONE_REPEAT: out.append(.repeated(id, member: Int(e.member)))
            default: out.append(.alone(id, down: false, member: Int(e.member), repeats: Int(e.repeats)))
            }
        }
        return out
    }
}
