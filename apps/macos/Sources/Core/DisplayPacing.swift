// Display-locked pacing decisions (docs/FRAME_PACING.md). The logic lives in the shared frontend
// core (frontend/src/pacing.cpp, rnf_cadence / rnf_input_deadline / ...); these are thin value
// wrappers for the live loop (EmulationController, driven by CAMetalDisplayLink):
//  * DisplayCadence  - which display refreshes start a new emulated frame
//  * InputDeadline   - how long before the commit deadline input is sampled (just in time)
//  * PresentPath     - whether the layer goes direct to the display (from the update timestamps)
//  * BacklogDrain    - when a steady one-drawable backlog is drained
//  * BuildAhead      - whether GPU-heavy pictures (CRT model) are built one refresh ahead
//  * AudioRateControl - dynamic rate control keeping the audio buffer level
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Maps display refreshes to emulated frames (locked to an integer multiple of the NES rate when
/// the display allows it, free otherwise).
struct DisplayCadence {
    static let lockTolerance = RNF_CADENCE_LOCK_TOLERANCE
    private var box: RNFHandle

    init(framePeriod: Double = FramePacing.period) {
        box = RNFHandle(rnf_cadence_new(framePeriod), free: { rnf_cadence_free($0) }, clone: { rnf_cadence_clone($0) })
    }

    var framePeriod: Double { rnf_cadence_frame_period(box.ptr) }
    /// Estimated refresh interval (seconds); 0 until two presentation times were seen.
    var refresh: Double { rnf_cadence_refresh_interval(box.ptr) }

    /// Refreshes per frame when locked, nil when the display rate is not a multiple of the NES rate.
    static func refreshesPerFrame(refresh: Double, framePeriod: Double = FramePacing.period) -> Int? {
        let k = rnf_refreshes_per_frame(refresh, framePeriod)
        return k > 0 ? Int(k) : nil
    }

    var refreshesPerFrame: Int? {
        let k = rnf_cadence_refreshes_per_frame(box.ptr)
        return k > 0 ? Int(k) : nil
    }

    /// Frames per second this cadence emulates (the NES rate when free or unknown).
    var emulationRate: Double { rnf_cadence_emulation_rate(box.ptr) }

    /// Interval two consecutive frames are expected to be on screen apart (seconds).
    var expectedFrameInterval: Double { rnf_cadence_expected_frame_interval(box.ptr) }

    /// One display callback whose picture appears at `presentation` (seconds). Returns true when
    /// this refresh starts a new emulated frame.
    mutating func refresh(presentation t: Double) -> Bool { rnf_cadence_refresh(RNFHandle.unique(&box), t) != 0 }
}

/// Just-in-time input sampling: input is read `lead` seconds before the frame's commit deadline.
struct InputDeadline {
    static let margin = RNF_INPUT_DEADLINE_MARGIN
    static let minLead = RNF_INPUT_DEADLINE_MIN_LEAD
    static let maxLead = RNF_INPUT_DEADLINE_MAX_LEAD
    static let missPenalty = RNF_INPUT_DEADLINE_MISS_PENALTY
    static let maxPenalty = RNF_INPUT_DEADLINE_MAX_PENALTY
    /// A commit later than this before the deadline counts as a miss.
    static let lateCommit = RNF_INPUT_DEADLINE_LATE_COMMIT
    static let decayAfter = Int(RNF_INPUT_DEADLINE_DECAY_AFTER)
    static let penaltyDecay = RNF_INPUT_DEADLINE_PENALTY_DECAY
    static let window = Int(RNF_INPUT_DEADLINE_WINDOW)

    private var box = RNFHandle(rnf_input_deadline_new(), free: { rnf_input_deadline_free($0) },
                                clone: { rnf_input_deadline_clone($0) })

    /// The work quantile (seconds; the upper edge of its 0.1 ms bin), 0 before any sample.
    var workQuantile: Double { rnf_input_deadline_work_quantile(box.ptr) }
    var lead: Double { rnf_input_deadline_lead(box.ptr) }
    var penalty: Double { rnf_input_deadline_penalty(box.ptr) }
    var misses: UInt64 { rnf_input_deadline_misses(box.ptr) }

    /// Seconds from the input sample to the frame's commit.
    mutating func observeWork(_ seconds: Double) { rnf_input_deadline_observe_work(RNFHandle.unique(&box), seconds) }

    /// A frame was committed too close to (or after) its deadline.
    mutating func observeMiss() { rnf_input_deadline_observe_miss(RNFHandle.unique(&box)) }
}

/// Whether the game layer goes direct to the display (every recent presentDelay about one refresh).
struct PresentPath {
    static let window = Int(RNF_PRESENT_PATH_WINDOW)
    private var box = RNFHandle(rnf_present_path_new(), free: { rnf_present_path_free($0) },
                                clone: { rnf_present_path_clone($0) })
    mutating func observe(presentDelay: Double) { rnf_present_path_observe(RNFHandle.unique(&box), presentDelay) }
    mutating func reset() { rnf_present_path_reset(RNFHandle.unique(&box)) }
    /// The largest recent presentDelay (0 before any update).
    var maxDelay: Double { rnf_present_path_max_delay(box.ptr) }
    func isDirect(refresh: Double) -> Bool { rnf_present_path_is_direct(box.ptr, refresh) != 0 }
}

/// When a steady one-drawable backlog is drained (fast when direct to a fixed-refresh display).
enum BacklogDrain {
    struct Policy: Equatable {
        let lateRun: Int          // consecutive late presents (new or repeat)
        let minInterval: Double   // seconds since the previous drain
    }
    static let fast = Policy(lateRun: Int(RNF_BACKLOG_FAST_LATE_RUN), minInterval: RNF_BACKLOG_FAST_MIN_INTERVAL)
    static let rare = Policy(lateRun: Int(RNF_BACKLOG_RARE_LATE_RUN), minInterval: RNF_BACKLOG_RARE_MIN_INTERVAL)
    static func policy(variableRefresh: Bool, direct: Bool) -> Policy {
        let p = rnf_backlog_drain_policy(variableRefresh ? 1 : 0, direct ? 1 : 0)
        return Policy(lateRun: Int(p.late_run), minInterval: p.min_interval)
    }
}

/// GPU-heavy pictures are built one refresh ahead when they would miss their refresh otherwise.
struct BuildAhead {
    static let window = Int(RNF_BUILD_AHEAD_WINDOW)
    static let delayWindow = Int(RNF_BUILD_AHEAD_DELAY_WINDOW)
    static let enterMargin = RNF_BUILD_AHEAD_ENTER_MARGIN
    static let leaveMargin = RNF_BUILD_AHEAD_LEAVE_MARGIN
    private var box = RNFHandle(rnf_build_ahead_new(), free: { rnf_build_ahead_free($0) },
                                clone: { rnf_build_ahead_clone($0) })

    var active: Bool { rnf_build_ahead_active(box.ptr) != 0 }

    /// GPU seconds of one picture build.
    mutating func add(_ seconds: Double) { rnf_build_ahead_add(RNFHandle.unique(&box), seconds) }

    /// Forget the history (nothing is built: plain picture).
    mutating func reset() { rnf_build_ahead_reset(RNFHandle.unique(&box)) }

    /// p90 of the recent builds; nil until a quarter of the window was seen.
    var p90: Double? {
        var q = 0.0
        return rnf_build_ahead_p90(box.ptr, &q) != 0 ? q : nil
    }

    /// Re-decides at a display link update whose picture appears `presentDelay` seconds after its
    /// commit deadline.
    mutating func update(presentDelay: Double) { rnf_build_ahead_update(RNFHandle.unique(&box), presentDelay) }
}

/// Dynamic rate control for the audio stream (ratio within 0.5 % of nominal / actual frame rate).
struct AudioRateControl {
    static let maxDeviation = RNF_DRC_MAX_DEVIATION
    static let gain = RNF_DRC_GAIN
    static let fillSmoothing = RNF_DRC_FILL_SMOOTHING
    private var box: RNFHandle

    init(targetFill: Double) {
        box = RNFHandle(rnf_audio_rate_new(targetFill), free: { rnf_audio_rate_free($0) }, clone: { rnf_audio_rate_clone($0) })
    }

    var targetFill: Double { rnf_audio_rate_target_fill(box.ptr) }
    var base: Double { rnf_audio_rate_base(box.ptr) }
    var ratio: Double { rnf_audio_rate_ratio(box.ptr) }
    var smoothedFill: Double? {
        var f = 0.0
        return rnf_audio_rate_smoothed_fill(box.ptr, &f) != 0 ? f : nil
    }

    /// Frames are emulated at `rate` Hz while the content is `nominal` Hz.
    mutating func setFrameRate(_ rate: Double, nominal: Double = 1 / FramePacing.period) {
        rnf_audio_rate_set_frame_rate(RNFHandle.unique(&box), rate, nominal)
    }

    /// Restart (after mute / underrun): forget the fill history.
    mutating func reset() { rnf_audio_rate_reset(RNFHandle.unique(&box)) }

    /// Buffer level (samples) just before a frame's audio is pushed. Returns the ratio to use.
    mutating func update(fill: Double) -> Double { rnf_audio_rate_update(RNFHandle.unique(&box), fill) }
}

/// Display watchdog (rnf_display_watchdog, docs/FRAME_PACING.md "Display watchdog"): a published
/// picture that has not reached the screen within `stall` seconds while emulation advances asks
/// for a renderer recovery, then a display link restart, then backs off.
struct DisplayWatchdog {
    enum Action: Equatable { case none, recover, restart }
    static let stall = RNF_DISPLAY_WATCHDOG_STALL
    static let backoff = RNF_DISPLAY_WATCHDOG_BACKOFF
    private var box = RNFHandle(rnf_display_watchdog_new(), free: { rnf_display_watchdog_free($0) },
                                clone: { rnf_display_watchdog_clone($0) })

    /// A new picture was published (host seconds).
    mutating func emulated(at now: Double) { rnf_display_watchdog_emulated(RNFHandle.unique(&box), now) }
    /// A new picture was confirmed on screen.
    mutating func presented(at now: Double) { rnf_display_watchdog_presented(RNFHandle.unique(&box), now) }
    /// The viewport is hidden or was replaced: nothing is expected on screen.
    mutating func reset() { rnf_display_watchdog_reset(RNFHandle.unique(&box)) }
    mutating func check(at now: Double) -> Action {
        switch rnf_display_watchdog_check(RNFHandle.unique(&box), now) {
        case RNF_WATCHDOG_RECOVER: return .recover
        case RNF_WATCHDOG_RESTART: return .restart
        default: return .none
        }
    }
    func waiting(at now: Double) -> Double { rnf_display_watchdog_waiting(box.ptr, now) }
    var actions: Int { Int(rnf_display_watchdog_recoveries(box.ptr)) }
}
