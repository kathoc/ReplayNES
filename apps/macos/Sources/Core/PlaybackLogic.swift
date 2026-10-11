// Transport logic used by the app and the tests: record/replay toggle, slow toggle, paused D-pad
// stepping, practice A/B loop, fast-forward over the recorded take, rewind-animation frame history
// and the practice audio fade-out tail. Implemented by the shared frontend core
// (frontend/src/playback.cpp); these are thin Swift wrappers.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum SlowRate: Int, CaseIterable, Identifiable {
    case normal = 1, half = 2, quarter = 4
    var id: Int { rawValue }
    var label: String { rnfString(rnf_slow_label(Int32(rawValue))) }
    /// Slow hotkey (L): toggles 0.5x <-> normal (1/4 is only reachable from the menu, if ever).
    var toggled: SlowRate { SlowRate(rawValue: Int(rnf_slow_toggled(Int32(rawValue)))) ?? .normal }
}

// MARK: - record / replay toggle

/// The single "Record" toggle button.
///  * record -> replay: the take actually plays (from the start at the take end).
///  * replay -> record: back to record mode at the current position, paused.
enum RecordToggle {
    struct Plan: Equatable {
        var record: Bool
        var seek: UInt64?
        var play: Bool
    }

    static func plan(recording: Bool, frame: UInt64, takeLength: UInt64) -> Plan {
        let p = rnf_record_toggle(recording ? 1 : 0, frame, takeLength)
        return Plan(record: p.record != 0, seek: p.has_seek != 0 ? p.seek : nil, play: p.play != 0)
    }

    /// Play/pause in replay mode at the take end restarts from the beginning (player behaviour).
    static func shouldRestartOnPlay(recording: Bool, practicing: Bool, frame: UInt64, takeLength: UInt64) -> Bool {
        rnf_record_toggle_restarts_on_play(recording ? 1 : 0, practicing ? 1 : 0, frame, takeLength) != 0
    }
}

// MARK: - paused D-pad stepping (with key repeat)

/// While paused, D-pad left/right step one frame back/forward; holding repeats after a delay.
struct StepRepeater {
    static let initialDelayTicks = Int(RNF_STEP_REPEAT_INITIAL_DELAY)
    static let intervalTicks = Int(RNF_STEP_REPEAT_INTERVAL)
    private var box = RNFHandle(rnf_step_repeater_new(), free: { rnf_step_repeater_free($0) },
                                clone: { rnf_step_repeater_clone($0) })

    var direction: Int { Int(rnf_step_repeater_direction(box.ptr)) }

    mutating func press(_ dir: Int) -> Int { Int(rnf_step_repeater_press(RNFHandle.unique(&box), Int32(dir))) }
    mutating func release(_ dir: Int) { rnf_step_repeater_release(RNFHandle.unique(&box), Int32(dir)) }
    mutating func tick() -> Int { Int(rnf_step_repeater_tick(RNFHandle.unique(&box))) }
    mutating func reset() { rnf_step_repeater_reset(RNFHandle.unique(&box)) }
}

// MARK: - practice A/B loop

/// Practice loop state machine (wall-clock only decides presentation; the engine decides content).
///   playing --(counter >= length)--> holding 0.5 s (last frame kept, audio fades out)
///           --> rewinding 1 s (the VTR sweep back to A over the reel, whatever the length)
///           --> restart (goto A) --> countdown 3, 2, 1 (optional) --> playing
/// L2 + R2 (`shoulders`) starts the rewind at once (docs/design/UI_REDESIGN.md, "Practice: return to A").
struct PracticeLoop: Equatable {
    static let holdSeconds = RNF_PRACTICE_HOLD_SECONDS
    static let rewindSeconds = RNF_PRACTICE_REWIND_SECONDS
    static let countdownSeconds = RNF_PRACTICE_COUNTDOWN_SECONDS

    enum Phase: Equatable {
        case playing
        case holding(since: Double)
        case rewinding(since: Double)
        case countdown(since: Double)
    }

    enum Action: Equatable {
        case step                       // emulate one practice frame (normal pacing)
        case beginHold                  // reached B: keep the picture, fade the audio out
        case hold                       // keep showing the last frame
        case rewindFrame(back: Double)  // 0...1: how far back towards A (the reel's sweep index)
        case restart                    // goto A (its picture; the countdown follows when on)
        case countdown(count: Int, fraction: Double)  // 3, 2, 1: nothing is emulated
    }

    private var box = RNFHandle(rnf_practice_loop_new(), free: { rnf_practice_loop_free($0) },
                                clone: { rnf_practice_loop_clone($0) })

    var phase: Phase {
        var since = 0.0
        switch rnf_practice_loop_phase(box.ptr, &since) {
        case RNF_PHASE_HOLDING: return .holding(since: since)
        case RNF_PHASE_REWINDING: return .rewinding(since: since)
        case RNF_PHASE_COUNTDOWN: return .countdown(since: since)
        default: return .playing
        }
    }
    var loops: Int { Int(rnf_practice_loop_loops(box.ptr)) }

    var isLooping: Bool { phase != .playing }
    var isReturning: Bool {
        switch phase { case .rewinding, .countdown: return true; default: return false }
    }
    var inCountdown: Bool { if case .countdown = phase { return true } else { return false } }
    var inRewind: Bool { if case .rewinding = phase { return true } else { return false } }

    var countdownEnabled: Bool {
        get { rnf_practice_loop_countdown(box.ptr) != 0 }
        set { rnf_practice_loop_set_countdown(RNFHandle.unique(&box), newValue ? 1 : 0) }
    }

    static func == (a: PracticeLoop, b: PracticeLoop) -> Bool { a.phase == b.phase && a.loops == b.loops }

    /// One unpaused tick. `length` = slot length in frames (nil = no B: free practice, no loop).
    mutating func tick(now: Double, counter: UInt64, length: UInt64?) -> Action {
        let a = rnf_practice_loop_tick(RNFHandle.unique(&box), now, counter, length == nil ? 0 : 1, length ?? 0)
        switch a.kind {
        case RNF_PRACTICE_BEGIN_HOLD: return .beginHold
        case RNF_PRACTICE_HOLD: return .hold
        case RNF_PRACTICE_REWIND_FRAME: return .rewindFrame(back: a.back)
        case RNF_PRACTICE_RESTART: return .restart
        case RNF_PRACTICE_COUNTDOWN: return .countdown(count: Int(a.count), fraction: a.fraction)
        default: return .step
        }
    }

    /// L2 / R2 held (every practice tick). Returns true when the L2 + R2 chord just started the
    /// return to A; `rewind` = whether L2 may rewind the run now.
    mutating func shoulders(now: Double, hasA: Bool, rewindHeld: Bool, ffHeld: Bool) -> (fired: Bool, rewind: Bool) {
        var rw: Int32 = 0
        let f = rnf_practice_loop_shoulders(RNFHandle.unique(&box), now, hasA ? 1 : 0, rewindHeld ? 1 : 0, ffHeld ? 1 : 0, &rw)
        return (f != 0, rw != 0)
    }

    /// The run starts at A: the countdown first when it is on.
    mutating func arrive(now: Double) { rnf_practice_loop_arrive(RNFHandle.unique(&box), now) }

    /// The user took over (rewind, step, goto A): drop any hold/rewind animation in progress.
    mutating func interrupt() { rnf_practice_loop_interrupt(RNFHandle.unique(&box)) }

    mutating func reset() { rnf_practice_loop_reset(RNFHandle.unique(&box)) }

    /// Index (0 = newest) into a history of `count` frames for animation progress `back`.
    static func historyIndex(back: Double, count: Int) -> Int {
        Int(rnf_practice_history_index(back, Int32(clamping: count)))
    }

    /// The countdown's look (shared with the desktop frontends).
    static func countdownVisual(count: Int, fraction: Double) -> rnf_countdown_visual {
        rnf_practice_countdown_visual(Int32(clamping: count), fraction)
    }
}

// MARK: - resume countdown

/// 3, 2, 1 over the paused picture before play resumes from a pause (rnf_resume_countdown;
/// docs/design/UI_REDESIGN.md, "Resume countdown"). The look is the practice countdown's
/// (PracticeLoop.countdownVisual). Only for live play: record, or practice while its loop plays
/// (never stacked on the practice return / its own countdown); never replay. The setting is
/// independent of PracticeLoop.countdownEnabled.
struct ResumeCountdown: Equatable {
    struct State: Equatable {
        var count: Int        // 3, 2, 1 while counting; 0 otherwise
        var fraction: Double  // 0...1 within that number's second
        var done: Bool        // the tick it ended: play from this tick on (reported once)
    }

    private var box = RNFHandle(rnf_resume_countdown_new(), free: { rnf_resume_countdown_free($0) },
                                clone: { rnf_resume_countdown_clone($0) })

    var enabled: Bool {
        get { rnf_resume_countdown_enabled(box.ptr) != 0 }
        set { rnf_resume_countdown_set_enabled(RNFHandle.unique(&box), newValue ? 1 : 0) }
    }
    var active: Bool { rnf_resume_countdown_active(box.ptr) != 0 }

    static func == (a: ResumeCountdown, b: ResumeCountdown) -> Bool { a.enabled == b.enabled && a.active == b.active }

    /// Whether a resume in this state counts down (setting on; record, or practice with its loop playing).
    func wanted(mode: rn_mode, practicePhase: PracticeLoop.Phase) -> Bool {
        rnf_resume_countdown_wanted(box.ptr, mode, Self.phase(practicePhase)) != 0
    }

    /// A resume from pause at `now`: starts the countdown when wanted (true); false = play at once.
    @discardableResult
    mutating func start(now: Double, mode: rn_mode, practicePhase: PracticeLoop.Phase) -> Bool {
        rnf_resume_countdown_start(RNFHandle.unique(&box), now, mode, Self.phase(practicePhase)) != 0
    }

    /// Every unpaused tick while active.
    mutating func tick(now: Double) -> State {
        let s = rnf_resume_countdown_tick(RNFHandle.unique(&box), now)
        return State(count: Int(s.count), fraction: s.fraction, done: s.done != 0)
    }

    /// A modal UI is up: it waits at its start.
    mutating func hold(now: Double) { rnf_resume_countdown_hold(RNFHandle.unique(&box), now) }

    /// Back to paused (a pause, the menu, the cancel tap).
    mutating func cancel() { if active { rnf_resume_countdown_cancel(RNFHandle.unique(&box)) } }

    private static func phase(_ p: PracticeLoop.Phase) -> rnf_practice_phase {
        switch p {
        case .playing: return RNF_PHASE_PLAYING
        case .holding: return RNF_PHASE_HOLDING
        case .rewinding: return RNF_PHASE_REWINDING
        case .countdown: return RNF_PHASE_COUNTDOWN
        }
    }
}

// MARK: - the practice run's reel (pictures for the sweep back to A)

/// Display-only pictures of the practice run, evenly spread from A (rnf_reel: at most
/// RNF_REEL_CAPACITY, decimated when full). Allocated lazily.
final class PracticeReel {
    private let handle: OpaquePointer

    init(capacity: Int = Int(RNF_REEL_CAPACITY)) { handle = rnf_reel_new(Int32(clamping: capacity))! }
    deinit { rnf_reel_free(handle) }

    var count: Int { Int(rnf_reel_count(handle)) }
    func offer(position: UInt64, _ src: UnsafePointer<UInt32>) { rnf_reel_offer(handle, position, src) }
    func truncate(after position: UInt64) { rnf_reel_truncate(handle, position) }
    func clear() { rnf_reel_clear(handle) }
    func release() { rnf_reel_release(handle) }

    /// The picture for sweep progress `back` (0 = the newest ... 1 = the oldest, next to A).
    func withSweepFrame<R>(back: Double, _ body: (UnsafePointer<UInt32>) -> R) -> R? {
        let i = rnf_reel_sweep_index(handle, back)
        guard i >= 0, let p = rnf_reel_frame(handle, i) else { return nil }
        return body(p)
    }
}

// MARK: - VTR effect (display only)

/// The tape-rewind look over the picture (rnf_vtr): fades in / out; zero cost while inactive.
final class VTREffect {
    private let handle: OpaquePointer

    init() { handle = rnf_vtr_new()! }
    deinit { rnf_vtr_free(handle) }

    func configure(enabled: Bool, level: FlashLevel) { rnf_vtr_configure(handle, enabled ? 1 : 0, level.cValue) }
    var active: Bool { rnf_vtr_active(handle) != 0 }
    func tick(now: Double, want: rnf_vtr_kind) -> rnf_vtr_params { rnf_vtr_tick(handle, now, want) }
    func reset() { rnf_vtr_reset(handle) }
    func apply(_ src: UnsafePointer<UInt32>, into dst: UnsafeMutablePointer<UInt32>, _ p: rnf_vtr_params) {
        var q = p
        rnf_vtr_apply(src, dst, &q)
    }
}

// MARK: - practice: natural audio ending

enum AudioFade {
    /// A short decaying tail built from the last frame's samples, pushed once when practice
    /// reaches B: the sound dies away instead of being cut (no click), the picture stays.
    static func tail(from last: UnsafeBufferPointer<Int16>, repeats: Int = 4) -> [Int16] {
        guard let base = last.baseAddress, last.count > 0, repeats > 0 else { return [] }
        let n = rnf_audio_fade_tail(base, last.count, Int32(clamping: repeats), nil, 0)
        var out = [Int16](repeating: 0, count: n)
        _ = out.withUnsafeMutableBufferPointer { rnf_audio_fade_tail(base, last.count, Int32(clamping: repeats), $0.baseAddress, n) }
        return out
    }
}

// MARK: - fast-forward = replay the recorded take only

/// Fast-forward never records: in record mode the session is switched to replay for the hold
/// and back to record at release. It stops at the take end.
struct FastForwardSession {
    private var box = RNFHandle(rnf_fast_forward_new(), free: { rnf_fast_forward_free($0) },
                                clone: { rnf_fast_forward_clone($0) })

    var active: Bool { rnf_fast_forward_active(box.ptr) != 0 }

    mutating func begin(_ s: EngineSession) throws { try rnCheck(rnf_fast_forward_begin(RNFHandle.unique(&box), s.handle)) }

    /// Emulates up to n recorded frames. Returns (frames emulated, reached the take end).
    func step(_ s: EngineSession, frames n: Int) throws -> (Int, Bool) {
        var done: Int32 = 0, end: Int32 = 0
        try rnCheck(rnf_fast_forward_step(box.ptr, s.handle, Int32(clamping: n), &done, &end))
        return (Int(done), end != 0)
    }

    mutating func end(_ s: EngineSession) throws { try rnCheck(rnf_fast_forward_end(RNFHandle.unique(&box), s.handle)) }

    /// Record mode as the user sees it (record mode is only suspended during the hold).
    func showsRecording(_ s: EngineSession) -> Bool { rnf_fast_forward_shows_recording(box.ptr, s.handle) != 0 }
}
