// Display-locked pacing logic (pure, no clocks of its own; see docs/FRAME_PACING.md):
//  * DisplayCadence  - which display refreshes start a new emulated frame
//  * InputDeadline   - how long before the commit deadline input is sampled (just in time)
//  * PresentPath     - whether the layer goes direct to the display (from the update timestamps)
//  * BacklogDrain    - when a steady one-drawable backlog is drained
//  * BuildAhead      - whether GPU-heavy pictures (CRT model) are built one refresh ahead
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

    /// Recent plausible timestamp deltas (the refresh estimate follows their median).
    private var deltas: [Double] = []
    private var deltaHead = 0
    static let deltaWindow = 15

    /// One display callback whose picture appears at `presentation` (seconds). Returns true when
    /// this refresh starts a new emulated frame.
    mutating func refresh(presentation t: Double) -> Bool {
        if let l = lastPresentation, t > l {
            let d = t - l
            if d >= Self.minRefresh && d <= Self.maxRefresh {
                if deltas.count < Self.deltaWindow { deltas.append(d) } else { deltas[deltaHead] = d; deltaHead = (deltaHead + 1) % Self.deltaWindow }
                let median = deltas.sorted()[deltas.count / 2]
                if refresh == 0 || abs(refresh - median) > 0.1 * median {
                    // First estimate, another display, or a wrong estimate from odd timestamps
                    // (a single short delta once stuck the estimate at 2-4x the real rate).
                    refresh = median
                } else if abs(d - refresh) < 0.25 * refresh {
                    refresh += (d - refresh) * 0.05              // refine (skipped callbacks are ignored)
                }
            }
            // Other deltas are not one refresh apart (duplicate / bogus timestamps, or a gap).
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
/// frame committed less than `lateCommit` before its deadline - or, direct to the display, whose
/// present was dropped or late (EmulationController) - and decays while none are.
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

/// Whether the game layer goes direct to the display: every one of the last `window` display link
/// updates had presentDelay (targetPresentationTimestamp - targetTimestamp) of about one refresh.
/// Composited it is two or more refreshes, but Core Animation sometimes predicts one refresh for a
/// composited window too, so a single update is not enough.
struct PresentPath {
    static let window = 60
    private var delays: [Double] = []
    private var head = 0
    mutating func observe(presentDelay: Double) {
        guard presentDelay > 0 else { return }
        if delays.count < Self.window { delays.append(presentDelay) } else { delays[head] = presentDelay; head = (head + 1) % Self.window }
    }
    mutating func reset() { delays.removeAll(keepingCapacity: true); head = 0 }
    /// The largest recent presentDelay (0 before any update).
    var maxDelay: Double { delays.max() ?? 0 }
    func isDirect(refresh: Double) -> Bool { delays.count >= Self.window / 4 && refresh > 0 && maxDelay < 1.5 * refresh }
}

/// A steady backlog: every present a whole refresh behind its target (one drawable too many queued
/// after a hiccup; presenting on every refresh never drains it by itself). Skipping one repeat
/// present drains it. When the layer goes direct to a fixed-refresh display, a late present can
/// only be such a backlog and skipping costs nothing but that refresh, so it is drained as soon as
/// two frames' presents were late. Otherwise it stays rare (half a second of late presents, at
/// most every 5 s): composited, a busy window server also shows presents a refresh late, which no
/// skip fixes; on a variable-refresh panel (ProMotion, Adaptive-Sync) a skipped refresh shifts the
/// panel's timing.
enum BacklogDrain {
    struct Policy: Equatable {
        let lateRun: Int          // consecutive late presents (new or repeat)
        let minInterval: Double   // seconds since the previous drain
    }
    static let fast = Policy(lateRun: 4, minInterval: 0.25)
    static let rare = Policy(lateRun: 60, minInterval: 5)
    static func policy(variableRefresh: Bool, direct: Bool) -> Policy {
        !variableRefresh && direct ? fast : rare
    }
}

/// GPU-heavy pictures (the CRT model) are built one refresh ahead of the present that shows them
/// (GameRenderer) when the picture-building GPU time (p90 of the last `window` frames) exceeds what
/// the display path leaves after the commit deadline: `presentDelay - enterMargin`, where
/// presentDelay = targetPresentationTimestamp - targetTimestamp of the display link updates (one
/// refresh when the layer goes direct to the display, two or more when composited; the largest of
/// the recent ones). Back in the same refresh when clearly below (`presentDelay - leaveMargin`).
/// Built in the same refresh, such a picture misses it and queues behind it (sticky one-refresh
/// backlog, drawable starvation); built ahead, it is shown exactly one refresh later.
struct BuildAhead {
    static let window = 120
    static let delayWindow = 60
    static let enterMargin = 0.0025
    static let leaveMargin = 0.004
    private(set) var active = false
    private var times: [Double] = []
    private var head = 0
    private var delays: [Double] = []
    private var delayHead = 0

    /// GPU seconds of one picture build.
    mutating func add(_ seconds: Double) {
        if times.count < Self.window { times.append(seconds) } else { times[head] = seconds; head = (head + 1) % Self.window }
    }

    /// Forget the history (nothing is built: plain picture).
    mutating func reset() { times.removeAll(keepingCapacity: true); head = 0; active = false }

    /// p90 of the recent builds; nil until a quarter of the window was seen.
    var p90: Double? {
        guard times.count >= Self.window / 4 else { return nil }
        let s = times.sorted()
        return s[Int((Double(s.count - 1) * 0.9).rounded())]
    }

    /// Re-decides at a display link update whose picture appears `presentDelay` seconds after its
    /// commit deadline.
    mutating func update(presentDelay: Double) {
        guard presentDelay > 0 else { return }
        if delays.count < Self.delayWindow { delays.append(presentDelay) } else { delays[delayHead] = presentDelay; delayHead = (delayHead + 1) % Self.delayWindow }
        guard let q = p90, let budget = delays.max() else { return }
        if !active && q > budget - Self.enterMargin { active = true }
        else if active && q < budget - Self.leaveMargin { active = false }
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
