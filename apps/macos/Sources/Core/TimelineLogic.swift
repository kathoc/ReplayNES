// Timeline logic: frame <-> x mapping of the take timeline, A/B range gestures, take lineage and
// which A/B slots lie on the active take. Implemented by the shared frontend core
// (frontend/src/timeline.cpp); these are the Swift types the views use.
// Positions are CURSOR frames (rn_frame values 0...takeLength), like rn_seek / rn_practice_set_range.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Linear frame <-> x mapping over the full timeline width.
struct TimelineGeometry: Equatable {
    var width: Double
    var length: UInt64

    func x(forFrame f: UInt64) -> Double { rnf_timeline_x_for_frame(width, length, f) }

    /// Nearest cursor frame at x (clamped to 0...length).
    func frame(atX x: Double) -> UInt64 { rnf_timeline_frame_at_x(width, length, x) }
}

/// An A/B slot as drawn on the timeline. b == nil: only A is set.
struct TimelineRange: Equatable {
    var slot: Int
    var a: UInt64
    var b: UInt64?

    var cValue: rnf_timeline_range { rnf_timeline_range(slot: Int32(slot), a: a, has_b: b == nil ? 0 : 1, b: b ?? 0) }

    init(slot: Int, a: UInt64, b: UInt64?) {
        self.slot = slot
        self.a = a
        self.b = b
    }

    init(_ r: rnf_timeline_range) { self.init(slot: Int(r.slot), a: r.a, b: r.has_b != 0 ? r.b : nil) }
}

enum TimelineHandle: Equatable {
    case a, b
    var cValue: rnf_timeline_handle { self == .a ? RNF_HANDLE_A : RNF_HANDLE_B }
}

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
        var a: UInt64 = 0, b: UInt64 = 0
        return rnf_timeline_range_from_drag(g.width, g.length, x0, x1, &a, &b) != 0 ? (a, b) : nil
    }

    /// Handles (within `tolerance` points of an A or B edge) win over bodies; ties go to
    /// `preferred` (the selected slot), then to the range drawn last (on top).
    static func hitTest(x: Double, ranges: [TimelineRange], in g: TimelineGeometry, tolerance: Double = 5,
                        preferred: Int? = nil) -> TimelineHit {
        let c = ranges.map(\.cValue)
        let h = rnf_timeline_hit_test(x, c, c.count, g.width, g.length, tolerance, preferred == nil ? 0 : 1, Int32(preferred ?? 0))
        switch h.kind {
        case RNF_HIT_HANDLE: return .handle(slot: Int(h.slot), h.handle == RNF_HANDLE_A ? .a : .b)
        case RNF_HIT_BODY: return .body(slot: Int(h.slot))
        default: return .none
        }
    }

    /// Moves one handle of [a, b) to x. Keeps a < b (at least one frame) inside 0...length.
    static func drag(_ handle: TimelineHandle, of r: (a: UInt64, b: UInt64), toX x: Double,
                     in g: TimelineGeometry) -> (a: UInt64, b: UInt64) {
        var a: UInt64 = 0, b: UInt64 = 0
        rnf_timeline_drag(handle.cValue, r.a, r.b, x, g.width, g.length, &a, &b)
        return (a, b)
    }

    private static func plan(_ fn: (UnsafePointer<rnf_timeline_range>?, UnsafeMutablePointer<UInt64>,
                                    UnsafeMutablePointer<UInt64>, UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>) -> rnf_mark_plan,
                             existing: TimelineRange?) -> TimelineMarkPlan {
        var a: UInt64 = 0, b: UInt64 = 0
        var msg: UnsafeMutablePointer<CChar>?
        var c = existing?.cValue ?? rnf_timeline_range()
        let p = existing == nil ? fn(nil, &a, &b, &msg) : fn(&c, &a, &b, &msg)
        switch p {
        case RNF_MARK_SET_RANGE: return .setRange(a: a, b: b)
        case RNF_MARK_SET_A_ONLY: return .setAOnly
        default: return .invalid(rnfString(msg))
        }
    }

    /// "Set A Here" at cursor f. `existing` = the slot as visible on the active take (nil if empty
    /// or set elsewhere).
    static func markA(at f: UInt64, existing: TimelineRange?) -> TimelineMarkPlan {
        plan({ rnf_timeline_mark_a(f, $0, $1, $2, $3) }, existing: existing)
    }

    /// "Set B Here" at cursor f.
    static func markB(at f: UInt64, existing: TimelineRange?) -> TimelineMarkPlan {
        plan({ rnf_timeline_mark_b(f, $0, $1, $2, $3) }, existing: existing)
    }

    /// Slots whose A (and B) lie on the active take.
    static func visibleRanges(slots: [PracticeSlotInfo], takes: [TakeInfo], activeTake: UInt64,
                              takeLength: UInt64) -> [TimelineRange] {
        let s = slots.map {
            rnf_practice_slot(index: Int32($0.index), has_a: $0.hasA ? 1 : 0, has_b: $0.hasB ? 1 : 0,
                              has_take_frame: $0.hasTakeFrame ? 1 : 0, take_frame: $0.takeFrame, length: $0.length,
                              take_id: $0.takeID)
        }
        let t = takes.map(\.cValue)
        var out = [rnf_timeline_range](repeating: rnf_timeline_range(), count: s.count)
        let n = rnf_timeline_visible_ranges(s, s.count, t, t.count, activeTake, takeLength, &out, out.count)
        return out.prefix(n).map(TimelineRange.init)
    }
}

enum TakeLineage {
    /// Number of leading frames (input records) two takes have in common: the picture at cursor
    /// f is identical on both takes iff f <= sharedPrefix. Unknown takes share nothing.
    static func sharedPrefix(_ takes: [TakeInfo], _ x: UInt64, _ y: UInt64) -> UInt64 {
        let t = takes.map(\.cValue)
        return rnf_take_shared_prefix(t, t.count, x, y)
    }
}

extension TakeInfo {
    var cValue: rn_take_info {
        rn_take_info(id: id, parent_id: parentID, branch_frame: branchFrame, length: length, created_seq: createdSeq,
                     is_active: isActive ? 1 : 0, child_count: childCount)
    }
}
