// Frame pacing counters for the latency overlay, the snapshot JSON and scripts/perf-smoke.sh:
// how evenly new emulated frames reach the screen, and how regularly the display callback runs.
// Thin value wrappers over the shared frontend core (rnf_frame_pacing / rnf_callback_regularity).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct FramePacing {
    /// One emulated frame (NTSC, 60.0988 Hz).
    static let period = rnf_frame_period()
    /// A new frame that reaches the screen later than this after the previous one (during
    /// continuous play) was held on screen for an extra refresh: visible judder.
    static let hitchInterval = rnf_frame_pacing_hitch_interval()
    /// Gaps longer than this are pauses / seeks, not pacing problems.
    static let continuityLimit = RNF_FRAME_PACING_CONTINUITY_LIMIT

    private var box = RNFHandle(rnf_frame_pacing_new(), free: { rnf_frame_pacing_free($0) },
                                clone: { rnf_frame_pacing_clone($0) })

    var presents: UInt64 { rnf_frame_pacing_presents(box.ptr) }
    /// Consecutive frames presented more than hitchInterval apart.
    var hitches: UInt64 { rnf_frame_pacing_hitches(box.ptr) }
    /// Emulated frames that were never presented.
    var skipped: UInt64 { rnf_frame_pacing_skipped(box.ptr) }
    /// Largest present-to-present interval of continuous play since the last `takeWindowMax()`.
    var windowMaxInterval: Double { rnf_frame_pacing_window_max(box.ptr) }
    /// The last newly emulated frame that became visible.
    var lastPresent: (frame: UInt64, time: Double)? {
        var f: UInt64 = 0, t = 0.0
        return rnf_frame_pacing_last_present(box.ptr, &f, &t) != 0 ? (f, t) : nil
    }

    /// A newly emulated frame `frame` became visible at `time`.
    mutating func present(frame: UInt64, at time: Double) { rnf_frame_pacing_present(RNFHandle.unique(&box), frame, time) }

    /// Returns and resets the window maximum (seconds).
    mutating func takeWindowMax() -> Double { rnf_frame_pacing_take_window_max(RNFHandle.unique(&box)) }
}

/// Regularity of a periodic callback (display draw, emulation tick): counts callbacks that came
/// later than `lateAfter` after the previous one, ignoring gaps over `idleAfter` (stopped).
struct CallbackRegularity {
    private var box: RNFHandle

    init(lateAfter: Double, idleAfter: Double = 0.25) {
        box = RNFHandle(rnf_callback_regularity_new(lateAfter, idleAfter), free: { rnf_callback_regularity_free($0) },
                        clone: { rnf_callback_regularity_clone($0) })
    }

    var lateAfter: Double {
        get { rnf_callback_regularity_late_after(box.ptr) }
        set { rnf_callback_regularity_set_late_after(RNFHandle.unique(&box), newValue) }
    }
    var idleAfter: Double { rnf_callback_regularity_idle_after(box.ptr) }
    var count: UInt64 { rnf_callback_regularity_count(box.ptr) }
    var late: UInt64 { rnf_callback_regularity_late(box.ptr) }
    var windowMaxGap: Double { rnf_callback_regularity_window_max(box.ptr) }

    mutating func tick(at t: Double) { rnf_callback_regularity_tick(RNFHandle.unique(&box), t) }

    mutating func takeWindowMax() -> Double { rnf_callback_regularity_take_window_max(RNFHandle.unique(&box)) }
}
