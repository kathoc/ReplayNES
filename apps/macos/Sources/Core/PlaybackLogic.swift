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
///           --> rewinding 0.5 s (recent frames shown backwards) --> restart (goto A) --> playing
struct PracticeLoop: Equatable {
    static let holdSeconds = RNF_PRACTICE_HOLD_SECONDS
    static let rewindSeconds = RNF_PRACTICE_REWIND_SECONDS

    enum Phase: Equatable {
        case playing
        case holding(since: Double)
        case rewinding(since: Double)
    }

    enum Action: Equatable {
        case step                       // emulate one practice frame (normal pacing)
        case beginHold                  // reached B: keep the picture, fade the audio out
        case hold                       // keep showing the last frame
        case rewindFrame(back: Double)  // 0...1: how far back into the recent frames to show
        case restart                    // goto A and autoplay again
    }

    private var box = RNFHandle(rnf_practice_loop_new(), free: { rnf_practice_loop_free($0) },
                                clone: { rnf_practice_loop_clone($0) })

    var phase: Phase {
        var since = 0.0
        switch rnf_practice_loop_phase(box.ptr, &since) {
        case RNF_PHASE_HOLDING: return .holding(since: since)
        case RNF_PHASE_REWINDING: return .rewinding(since: since)
        default: return .playing
        }
    }
    var loops: Int { Int(rnf_practice_loop_loops(box.ptr)) }

    var isLooping: Bool { phase != .playing }

    static func == (a: PracticeLoop, b: PracticeLoop) -> Bool { a.phase == b.phase && a.loops == b.loops }

    /// One unpaused tick. `length` = slot length in frames (nil = no B: free practice, no loop).
    mutating func tick(now: Double, counter: UInt64, length: UInt64?) -> Action {
        let a = rnf_practice_loop_tick(RNFHandle.unique(&box), now, counter, length == nil ? 0 : 1, length ?? 0)
        switch a.kind {
        case RNF_PRACTICE_BEGIN_HOLD: return .beginHold
        case RNF_PRACTICE_HOLD: return .hold
        case RNF_PRACTICE_REWIND_FRAME: return .rewindFrame(back: a.back)
        case RNF_PRACTICE_RESTART: return .restart
        default: return .step
        }
    }

    /// The user took over (rewind, step, goto A): drop any hold/rewind animation in progress.
    mutating func interrupt() { rnf_practice_loop_interrupt(RNFHandle.unique(&box)) }

    mutating func reset() { rnf_practice_loop_reset(RNFHandle.unique(&box)) }

    /// Index (0 = newest) into a history of `count` frames for animation progress `back`.
    static func historyIndex(back: Double, count: Int) -> Int {
        Int(rnf_practice_history_index(back, Int32(clamping: count)))
    }
}

// MARK: - recent frames for the practice rewind animation

/// Fixed-size ring of the most recent emulated frames (256x240 BGRA). Allocated lazily.
final class FrameHistory {
    private let handle: OpaquePointer
    let capacity: Int

    init(capacity: Int) {
        handle = rnf_frame_history_new(Int32(clamping: capacity))!
        self.capacity = Int(rnf_frame_history_capacity(handle))
    }
    deinit { rnf_frame_history_free(handle) }

    var count: Int { Int(rnf_frame_history_count(handle)) }

    func append(_ src: UnsafePointer<UInt32>) { rnf_frame_history_append(handle, src) }

    /// back = 0 is the newest frame.
    func withFrame<R>(back: Int, _ body: (UnsafePointer<UInt32>) -> R) -> R? {
        guard back >= 0, back <= Int(Int32.max), let p = rnf_frame_history_frame(handle, Int32(back)) else { return nil }
        return body(p)
    }

    func clear() { rnf_frame_history_clear(handle) }

    /// Releases the memory (leaving practice).
    func release() { rnf_frame_history_release(handle) }
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
