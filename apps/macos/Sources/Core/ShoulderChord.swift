// L+R chord of the Quick Menu (docs/design/UI_REDESIGN.md, "Input model"): both shoulders down
// within 100 ms (either order) = the chord, which fires once both are down. L or R alone keeps
// its own action (slow / pause by default) but fires on release, or once it has been held for
// 100 ms without the other shoulder.
//
// TODO(core-chord): the shared core (frontend/, C API) gets the same detector (rnf_chord_*) and
// the "hk.menu" combo binding. After that merge, replace SwiftShoulderChord with a thin wrapper
// over rnf_chord_* that conforms to ShoulderChordDetector; the only construction site is
// InputManager.makeChordDetector() (apps/macos/Sources/App/InputManager.swift), and InputManager
// only depends on the protocol and on the ChordEvent contract below.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum Shoulder: Int, CaseIterable {
    case left = 0, right = 1
    var other: Shoulder { self == .left ? .right : .left }
}

enum ChordEvent: Equatable {
    /// L+R: open / close the Quick Menu.
    case chord
    /// A shoulder pressed on its own: its own action, as a press and later a release.
    case alone(Shoulder, down: Bool)
}

/// One controller's shoulder pair. Times are seconds on one monotonic clock.
protocol ShoulderChordDetector: AnyObject {
    /// A shoulder changed. Returns the events to act on now (in order).
    func update(_ s: Shoulder, down: Bool, now: Double) -> [ChordEvent]
    /// Call at `nextDeadline` (or later): a shoulder held alone for the whole window fires.
    func tick(now: Double) -> [ChordEvent]
    /// When `tick` has something to do (nil: nothing pending).
    var nextDeadline: Double? { get }
    /// Forget everything (controller detached): nothing fires for buttons held now.
    func reset()
}

final class SwiftShoulderChord: ShoulderChordDetector {
    /// Both shoulders within this many seconds = the chord.
    static let window = 0.100

    private enum State: Equatable {
        case up
        case pending(since: Double)  // down, waiting for the other shoulder or the timeout
        case alone                   // fired as itself (release still to come)
        case chord                   // part of a chord: its release is swallowed
    }
    private var state: [State] = [.up, .up]

    func update(_ s: Shoulder, down: Bool, now: Double) -> [ChordEvent] {
        var out = tick(now: now)
        let i = s.rawValue, o = s.other.rawValue
        if down {
            guard state[i] == .up else { return out }   // repeat of a held button
            if case .pending = state[o] {
                state[i] = .chord
                state[o] = .chord
                out.append(.chord)
            } else {
                state[i] = .pending(since: now)
            }
        } else {
            switch state[i] {
            case .pending:
                out.append(.alone(s, down: true))
                out.append(.alone(s, down: false))
            case .alone:
                out.append(.alone(s, down: false))
            case .chord, .up:
                break
            }
            state[i] = .up
        }
        return out
    }

    func tick(now: Double) -> [ChordEvent] {
        var out: [ChordEvent] = []
        for s in Shoulder.allCases {
            if case .pending(let since) = state[s.rawValue], now - since >= Self.window {
                state[s.rawValue] = .alone
                out.append(.alone(s, down: true))
            }
        }
        return out
    }

    var nextDeadline: Double? {
        state.compactMap { st -> Double? in
            if case .pending(let since) = st { return since + Self.window }
            return nil
        }.min()
    }

    func reset() { state = [.up, .up] }
}
