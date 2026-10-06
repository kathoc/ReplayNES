// Frame pacing counters for the latency overlay, the snapshot JSON and scripts/perf-smoke.sh:
// how evenly new emulated frames reach the screen, and how regularly the display callback runs.
// Pure bookkeeping (no clocks of its own): callers pass timestamps in seconds.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct FramePacing {
    /// One emulated frame (NTSC, 60.0988 Hz).
    static let period = Double(RN_FPS_DEN) / Double(RN_FPS_NUM)
    /// A new frame that reaches the screen later than this after the previous one (during
    /// continuous play) was held on screen for an extra refresh: visible judder.
    static let hitchInterval = period * 1.5
    /// Gaps longer than this are pauses / seeks, not pacing problems.
    static let continuityLimit = 0.25

    private(set) var presents: UInt64 = 0
    /// Consecutive frames presented more than hitchInterval apart.
    private(set) var hitches: UInt64 = 0
    /// Emulated frames that were never presented (overwritten in the mailbox before a draw).
    private(set) var skipped: UInt64 = 0
    /// Largest present-to-present interval of continuous play since the last `takeWindowMax()`.
    private(set) var windowMaxInterval = 0.0
    /// The last newly emulated frame that became visible.
    private(set) var lastPresent: (frame: UInt64, time: Double)?

    /// A newly emulated frame `frame` became visible at `time`.
    mutating func present(frame: UInt64, at time: Double) {
        presents &+= 1
        defer { lastPresent = (frame, time) }
        guard let l = lastPresent, frame > l.frame, frame - l.frame <= 8, time > l.time,
              time - l.time < Self.continuityLimit else { return }
        let dt = time - l.time
        skipped &+= frame - l.frame - 1
        if frame - l.frame == 1 && dt > Self.hitchInterval { hitches &+= 1 }
        windowMaxInterval = max(windowMaxInterval, dt)
    }

    /// Returns and resets the window maximum (seconds).
    mutating func takeWindowMax() -> Double {
        defer { windowMaxInterval = 0 }
        return windowMaxInterval
    }
}

/// Regularity of a periodic callback (display draw, emulation tick): counts callbacks that came
/// later than `lateAfter` after the previous one, ignoring gaps over `idleAfter` (stopped).
struct CallbackRegularity {
    var lateAfter: Double
    var idleAfter: Double
    private(set) var count: UInt64 = 0
    private(set) var late: UInt64 = 0
    private(set) var windowMaxGap = 0.0
    private var lastTime: Double?

    init(lateAfter: Double, idleAfter: Double = 0.25) {
        self.lateAfter = lateAfter
        self.idleAfter = idleAfter
    }

    mutating func tick(at t: Double) {
        count &+= 1
        defer { lastTime = t }
        guard let l = lastTime, t > l, t - l < idleAfter else { return }
        let gap = t - l
        if gap > lateAfter { late &+= 1 }
        windowMaxGap = max(windowMaxGap, gap)
    }

    mutating func takeWindowMax() -> Double {
        defer { windowMaxGap = 0 }
        return windowMaxGap
    }
}
