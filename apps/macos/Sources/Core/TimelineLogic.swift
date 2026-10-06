// Pure timeline logic (unit-tested): frame <-> x mapping of the take timeline, A/B range
// gestures, take lineage (which frames two takes share) and which A/B slots lie on the active take.
// Positions are CURSOR frames (rn_frame values 0...takeLength), like rn_seek / rn_practice_set_range.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Linear frame <-> x mapping over the full timeline width.
struct TimelineGeometry: Equatable {
    var width: Double
    var length: UInt64

    func x(forFrame f: UInt64) -> Double {
        guard length > 0, width > 0 else { return 0 }
        return Double(min(f, length)) / Double(length) * width
    }

    /// Nearest cursor frame at x (clamped to 0...length).
    func frame(atX x: Double) -> UInt64 {
        guard length > 0, width > 0 else { return 0 }
        let t = max(0, min(1, x / width))
        return min(length, UInt64((t * Double(length)).rounded()))
    }
}

/// An A/B slot as drawn on the timeline. b == nil: only A is set.
struct TimelineRange: Equatable {
    var slot: Int
    var a: UInt64
    var b: UInt64?
}

enum TimelineHandle: Equatable { case a, b }

enum TimelineHit: Equatable {
    case handle(slot: Int, TimelineHandle)
    case body(slot: Int)
    case none
}

/// What "Set A Here" / "Set B Here" at the playhead does.
enum TimelineMarkPlan: Equatable {
    case setRange(a: UInt64, b: UInt64)  // rn_practice_set_range
    case setAOnly                        // rn_practice_set_a at the cursor (B cleared)
    case invalid(String)                 // message for the user
}

enum TimelineEditing {
    /// Range selected by dragging from x0 to x1 (either direction). nil if shorter than 1 frame.
    static func range(fromX x0: Double, toX x1: Double, in g: TimelineGeometry) -> (a: UInt64, b: UInt64)? {
        let f0 = g.frame(atX: x0), f1 = g.frame(atX: x1)
        let a = min(f0, f1), b = max(f0, f1)
        return b > a ? (a, b) : nil
    }

    /// Handles (within `tolerance` points of an A or B edge) win over bodies. Among several
    /// candidates the closest edge wins; ties go to `preferred` (the selected slot), then to the
    /// range drawn last (on top).
    static func hitTest(x: Double, ranges: [TimelineRange], in g: TimelineGeometry, tolerance: Double = 5,
                        preferred: Int? = nil) -> TimelineHit {
        var best: (dist: Double, rank: Int, hit: TimelineHit)?
        for (i, r) in ranges.enumerated() {
            let rank = (r.slot == preferred ? 1_000 : 0) + i
            func consider(_ edge: UInt64, _ h: TimelineHandle) {
                let d = abs(g.x(forFrame: edge) - x)
                guard d <= tolerance else { return }
                if let b = best, b.dist < d || (b.dist == d && b.rank > rank) { return }
                best = (d, rank, .handle(slot: r.slot, h))
            }
            consider(r.a, .a)
            if let b = r.b { consider(b, .b) }
        }
        if let best { return best.hit }
        var body: (rank: Int, slot: Int)?
        for (i, r) in ranges.enumerated() {
            guard let b = r.b else { continue }
            let x0 = g.x(forFrame: r.a), x1 = g.x(forFrame: b)
            guard x >= x0 && x <= x1 else { continue }
            let rank = (r.slot == preferred ? 1_000 : 0) + i
            if body == nil || rank > body!.rank { body = (rank, r.slot) }
        }
        return body.map { .body(slot: $0.slot) } ?? .none
    }

    /// Moves one handle of [a, b) to x. Keeps a < b (at least one frame) inside 0...length.
    static func drag(_ handle: TimelineHandle, of r: (a: UInt64, b: UInt64), toX x: Double,
                     in g: TimelineGeometry) -> (a: UInt64, b: UInt64) {
        let f = g.frame(atX: x)
        switch handle {
        case .a:
            let limit = r.b > 0 ? r.b - 1 : 0
            return (min(f, limit), r.b)
        case .b:
            return (r.a, min(max(f, r.a + 1), max(g.length, r.a + 1)))
        }
    }

    /// "Set A Here" at cursor f. `existing` = the slot as visible on the active take (nil if empty
    /// or set elsewhere).
    static func markA(at f: UInt64, existing: TimelineRange?) -> TimelineMarkPlan {
        if let r = existing, let b = r.b, f < b { return .setRange(a: f, b: b) }
        return .setAOnly
    }

    /// "Set B Here" at cursor f.
    static func markB(at f: UInt64, existing: TimelineRange?) -> TimelineMarkPlan {
        guard let r = existing else { return .invalid(String(localized: "Set A of this section first")) }
        guard f > r.a else { return .invalid(String(localized: "Set B at a position after A")) }
        return .setRange(a: r.a, b: f)
    }

    /// Slots whose A (and B) lie on the active take: A set on the take (not inside a practice
    /// run) and every frame up to B shared by the slot's take and the active take.
    static func visibleRanges(slots: [PracticeSlotInfo], takes: [TakeInfo], activeTake: UInt64,
                              takeLength: UInt64) -> [TimelineRange] {
        var out: [TimelineRange] = []
        for s in slots where s.hasA && s.hasTakeFrame {
            let end = s.hasB ? s.takeFrame + s.length : s.takeFrame
            guard end <= takeLength,
                  end <= TakeLineage.sharedPrefix(takes, s.takeID, activeTake) else { continue }
            out.append(TimelineRange(slot: s.index, a: s.takeFrame, b: s.hasB ? end : nil))
        }
        return out
    }
}

enum TakeLineage {
    /// Number of leading frames (input records) two takes have in common: the picture at cursor
    /// f is identical on both takes iff f <= sharedPrefix. Unknown takes share nothing.
    static func sharedPrefix(_ takes: [TakeInfo], _ x: UInt64, _ y: UInt64) -> UInt64 {
        var byID: [UInt64: TakeInfo] = [:]
        for t in takes { byID[t.id] = t }
        guard let tx = byID[x], byID[y] != nil else { return 0 }
        if x == y { return tx.length }
        func chain(_ id: UInt64) -> [UInt64] {  // id, parent, ..., root
            var out: [UInt64] = []
            var cur = id
            while cur != 0, let t = byID[cur], out.count <= byID.count {
                out.append(cur)
                cur = t.parentID
            }
            return out
        }
        let cx = chain(x), cy = chain(y)
        let setX = Set(cx)
        guard let ci = cy.firstIndex(where: { setX.contains($0) }) else { return 0 }
        let common = cy[ci]
        let xi = cx.firstIndex(of: common)!
        // Frames of `common` used by each path end where that path's next segment branches off.
        let limX = xi == 0 ? byID[common]!.length : byID[cx[xi - 1]]!.branchFrame
        let limY = ci == 0 ? byID[common]!.length : byID[cy[ci - 1]]!.branchFrame
        return min(limX, limY)
    }
}
