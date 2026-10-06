// Display-locked pacing logic (pure, no clocks of its own; see docs/FRAME_PACING.md):
//  * DisplayCadence  - which display refreshes start a new emulated frame
//  * InputDeadline   - how long before the commit deadline input is sampled (just in time)
//  * AudioRateControl - dynamic rate control: resampling ratio that keeps the audio buffer level
//                       while emulation runs at the display rate instead of 60.0988 Hz
// The live loop (EmulationController) is driven by CAMetalDisplayLink; these types only decide.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Maps display refreshes to emulated frames.
/// Locked: when the refresh rate is (within `lockTolerance`) an integer multiple k of the NES rate
/// (60 Hz: k = 1, 120 Hz ProMotion: k = 2, 240 Hz: k = 4), exactly every k-th refresh starts a
/// frame: emulation runs at refresh / k (60.000 Hz instead of 60.0988 Hz, -0.16 %) and every frame
/// stays on screen for exactly k refreshes - no judder by construction. Logical time is the frame
/// index, so this never affects determinism; audio follows through AudioRateControl.
/// Free: other rates (e.g. 144 Hz, 75 Hz) keep the NTSC rate and pick the nearest refresh (a
/// mixed cadence is unavoidable there without variable refresh).
struct DisplayCadence {
    static let lockTolerance = 0.005
    /// Plausible refresh intervals (500 Hz .. 24 Hz); other timestamp deltas are not refreshes.
    static let minRefresh = 1.0 / 500
    static let maxRefresh = 1.0 / 24
    let framePeriod: Double

    /// Estimated refresh interval (seconds); 0 until two presentation times were seen.
    private(set) var refresh = 0.0
    private var lastPresentation: Double?
    private var lastFrameTime: Double?
    private var contentTime = 0.0   // free mode: ideal start of the next frame

    init(framePeriod: Double = FramePacing.period) { self.framePeriod = framePeriod }

    /// Refreshes per frame when locked, nil when the display rate is not a multiple of the NES rate.
    static func refreshesPerFrame(refresh: Double, framePeriod: Double = FramePacing.period) -> Int? {
        guard refresh > 0 else { return nil }
        let k = (framePeriod / refresh).rounded()
        guard k >= 1, abs(k * refresh - framePeriod) / framePeriod <= lockTolerance else { return nil }
        return Int(k)
    }

    var refreshesPerFrame: Int? { Self.refreshesPerFrame(refresh: refresh, framePeriod: framePeriod) }

    /// Frames per second this cadence emulates (the NES rate when free or unknown).
    var emulationRate: Double {
        guard let k = refreshesPerFrame else { return 1 / framePeriod }
        return 1 / (Double(k) * refresh)
    }

    /// Interval two consecutive frames are expected to be on screen apart (seconds).
    var expectedFrameInterval: Double {
        guard let k = refreshesPerFrame else { return framePeriod }
        return Double(k) * refresh
    }

    /// One display callback whose picture appears at `presentation` (seconds). Returns true when
    /// this refresh starts a new emulated frame.
    mutating func refresh(presentation t: Double) -> Bool {
        if let l = lastPresentation, t > l {
            let d = t - l
            if d < Self.minRefresh || d > Self.maxRefresh {
                // Not one refresh apart (duplicate / bogus timestamps, or a gap): no estimate.
            } else if refresh == 0 || d < refresh * 0.75 {
                refresh = d                                  // first estimate, or a faster display
            } else if d < refresh * 1.25 {
                refresh += (d - refresh) * 0.05              // refine (skipped callbacks are ignored)
            }
        }
        lastPresentation = t
        guard refresh > 0, let last = lastFrameTime else {
            lastFrameTime = t
            contentTime = t + framePeriod
            return true
        }
        if t <= last { return false }
        if let k = refreshesPerFrame {
            // Every k-th refresh after the previous frame (half a refresh of tolerance absorbs
            // timestamp noise; a refresh without a callback - display link hiccup - starts the
            // frame at the next callback instead of skipping one).
            guard t - last >= (Double(k) - 0.5) * refresh else { return false }
            lastFrameTime = t
            contentTime = t + framePeriod
            return true
        }
        // Free: the refresh nearest to the ideal frame time; resynchronise after a long gap.
        guard t >= contentTime - refresh / 2 else { return false }
        contentTime = t - contentTime > 2 * framePeriod ? t + framePeriod : contentTime + framePeriod
        lastFrameTime = t
        return true
    }
}

/// Just-in-time input sampling: input is read `lead` seconds before the frame's commit deadline
/// (CAMetalDisplayLink targetTimestamp), so the emulate + render work is committed just in time.
/// The lead is the 99.5th percentile of the recent work (sample -> commit, last ~10 s: a single
/// stall does not raise it, a run of heavier frames - e.g. the flash filter on a flashing scene -
/// does within a few frames), plus a safety margin (on a ProMotion panel a commit later than about
/// 1.5 ms before the deadline sometimes misses its refresh), plus a penalty that grows on every
/// frame committed less than `lateCommit` before its deadline and decays while none are.
struct InputDeadline {
    static let margin = 0.0015
    static let minLead = 0.002
    static let maxLead = 0.010
    static let missPenalty = 0.0005
    static let maxPenalty = 0.003
    /// A commit later than this before the deadline counts as a miss.
    static let lateCommit = 0.0005
    /// Frames without a miss before the penalty shrinks by `penaltyDecay` (~10 s at 60 fps).
    static let decayAfter = 600
    static let penaltyDecay = 0.0001
    /// Work samples kept (~10 s at 60 fps) and the quantile used.
    static let window = 600
    static let quantile = 0.995
    static let binWidth = 0.0001   // 0.1 ms
    static let binCount = 101      // the last bin collects everything >= 10 ms

    private var ring = [UInt8](repeating: 0, count: InputDeadline.window)
    private var bins = [Int](repeating: 0, count: InputDeadline.binCount)
    private var filled = 0, head = 0
    private(set) var penalty = 0.0
    private var clean = 0
    private(set) var misses: UInt64 = 0

    /// The work quantile (seconds; the upper edge of its 0.1 ms bin), 0 before any sample.
    var workQuantile: Double {
        guard filled > 0 else { return 0 }
        let need = Int((Double(filled) * Self.quantile).rounded(.up))
        var sum = 0
        for (i, n) in bins.enumerated() {
            sum += n
            if sum >= need { return Double(i + 1) * Self.binWidth }
        }
        return Double(Self.binCount) * Self.binWidth
    }

    var lead: Double { min(Self.maxLead, max(Self.minLead, workQuantile + Self.margin + penalty)) }

    /// Seconds from the input sample to the frame's commit.
    mutating func observeWork(_ seconds: Double) {
        let bin = UInt8(min(Self.binCount - 1, max(0, Int(seconds / Self.binWidth))))
        if filled == Self.window { bins[Int(ring[head])] -= 1 } else { filled += 1 }
        ring[head] = bin
        bins[Int(bin)] += 1
        head = (head + 1) % Self.window
        clean += 1
        if clean >= Self.decayAfter {
            clean = 0
            penalty = max(0, penalty - Self.penaltyDecay)
        }
    }

    /// A frame was committed too close to (or after) its deadline.
    mutating func observeMiss() {
        misses &+= 1
        clean = 0
        penalty = min(Self.maxPenalty, penalty + Self.missPenalty)
    }
}

/// Dynamic rate control for the audio stream (RetroArch-style DRC): the emulator produces
/// 48000 / 60.0988 samples per frame, the device consumes 48000 per second of its own clock,
/// and frames are emulated at the display rate. The output is resampled by
/// `ratio = base x (1 + clamp(k x error))`, where `base` is the nominal / actual frame-rate
/// ratio and `error` the smoothed relative distance of the buffer level from its target, so the
/// buffer neither runs dry (underruns) nor grows (latency, skips). |ratio - base| <= 0.5 %:
/// a pitch change far below audibility (0.5 % = 8.6 cents).
struct AudioRateControl {
    static let maxDeviation = 0.005
    /// Full deviation at this relative fill error.
    static let gain = 0.005
    /// Smoothing of the measured fill (per frame).
    static let fillSmoothing = 0.02
    let targetFill: Double   // samples

    private(set) var base = 1.0
    private(set) var ratio = 1.0
    private(set) var smoothedFill: Double?

    init(targetFill: Double) { self.targetFill = targetFill }

    /// Frames are emulated at `rate` Hz while the content is `nominal` Hz.
    mutating func setFrameRate(_ rate: Double, nominal: Double = 1 / FramePacing.period) {
        guard rate > 0 else { return }
        base = nominal / rate
    }

    /// Restart (after mute / underrun): forget the fill history.
    mutating func reset() { smoothedFill = nil; ratio = base }

    /// Buffer level (samples) just before a frame's audio is pushed. Returns the ratio to use for
    /// that frame (output samples per input sample).
    mutating func update(fill: Double) -> Double {
        let f = smoothedFill.map { $0 + (fill - $0) * Self.fillSmoothing } ?? fill
        smoothedFill = f
        let error = (targetFill - f) / targetFill
        let adj = max(-Self.maxDeviation, min(Self.maxDeviation, error * Self.gain))
        ratio = base * (1 + adj)
        return ratio
    }
}
