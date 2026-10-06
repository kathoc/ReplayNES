// UI-free transport logic shared by the app and the unit tests: record/replay toggle, slow
// toggle, paused D-pad stepping, practice A/B loop, fast-forward over the recorded take,
// rewind-animation frame history and the practice audio fade-out tail.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum SlowRate: Int, CaseIterable, Identifiable {
    case normal = 1, half = 2, quarter = 4
    var id: Int { rawValue }
    var label: String { self == .normal ? "等速" : self == .half ? "1/2" : "1/4" }
    /// Slow hotkey (L): toggles 0.5x <-> normal (1/4 is only reachable from the menu, if ever).
    var toggled: SlowRate { self == .normal ? .half : .normal }
}

// MARK: - record / replay toggle

/// The single 「録画」 toggle button.
///  * record -> replay: the take actually plays. At the take end it jumps to the start first;
///    otherwise it plays from the current position (= the last seek/scrub position).
///  * replay -> record: back to record mode at the current position, paused; the next input
///    continues the take (or branches if before its end).
enum RecordToggle {
    struct Plan: Equatable {
        var record: Bool
        var seek: UInt64?
        var play: Bool
    }

    static func plan(recording: Bool, frame: UInt64, takeLength: UInt64) -> Plan {
        if recording {
            if takeLength == 0 { return Plan(record: false, seek: nil, play: false) } // nothing recorded yet
            return Plan(record: false, seek: frame >= takeLength ? 0 : nil, play: true)
        }
        return Plan(record: true, seek: nil, play: false)
    }

    /// Play/pause in replay mode at the take end restarts from the beginning (player behaviour).
    static func shouldRestartOnPlay(recording: Bool, practicing: Bool, frame: UInt64, takeLength: UInt64) -> Bool {
        !recording && !practicing && takeLength > 0 && frame >= takeLength
    }
}

// MARK: - paused D-pad stepping (with key repeat)

/// While paused, D-pad left/right step one frame back/forward; holding repeats after a delay.
/// `press` steps immediately; `tick` (once per emulation tick) yields the repeats.
struct StepRepeater {
    static let initialDelayTicks = 18 // ~0.3 s
    static let intervalTicks = 3      // ~20 steps/s
    private(set) var direction = 0
    private var heldTicks = 0

    mutating func press(_ dir: Int) -> Int {
        direction = dir
        heldTicks = 0
        return dir
    }

    mutating func release(_ dir: Int) {
        if dir == direction { direction = 0; heldTicks = 0 }
    }

    mutating func tick() -> Int {
        guard direction != 0 else { return 0 }
        heldTicks += 1
        if heldTicks >= Self.initialDelayTicks, (heldTicks - Self.initialDelayTicks) % Self.intervalTicks == 0 {
            return direction
        }
        return 0
    }

    mutating func reset() { direction = 0; heldTicks = 0 }
}

// MARK: - practice A/B loop

/// Practice loop state machine (wall-clock only decides presentation; the engine decides content).
///   playing --(counter >= length)--> holding 0.5 s (last frame kept, audio fades out)
///           --> rewinding 0.5 s (recent frames shown backwards) --> restart (goto A) --> playing
struct PracticeLoop: Equatable {
    static let holdSeconds = 0.5
    static let rewindSeconds = 0.5

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

    private(set) var phase: Phase = .playing
    private(set) var loops = 0

    var isLooping: Bool { phase != .playing }

    /// One unpaused tick. `length` = slot length in frames (nil = no B: free practice, no loop).
    mutating func tick(now: Double, counter: UInt64, length: UInt64?) -> Action {
        switch phase {
        case .playing:
            if let length, length > 0, counter >= length {
                phase = .holding(since: now)
                return .beginHold
            }
            return .step
        case .holding(let t0):
            if now - t0 >= Self.holdSeconds {
                phase = .rewinding(since: now)
                return .rewindFrame(back: 0)
            }
            return .hold
        case .rewinding(let t0):
            let p = (now - t0) / Self.rewindSeconds
            if p >= 1 {
                phase = .playing
                loops += 1
                return .restart
            }
            return .rewindFrame(back: max(0, p))
        }
    }

    /// The user took over (rewind, step, goto A): drop any hold/rewind animation in progress.
    mutating func interrupt() { phase = .playing }

    mutating func reset() { phase = .playing; loops = 0 }

    /// Index (0 = newest) into a history of `count` frames for animation progress `back`.
    static func historyIndex(back: Double, count: Int) -> Int {
        guard count > 0 else { return 0 }
        return min(count - 1, max(0, Int((back * Double(count)).rounded(.down))))
    }
}

// MARK: - recent frames for the practice rewind animation

/// Fixed-size ring of the most recent emulated frames (256x240 BGRA). Allocated lazily.
final class FrameHistory {
    let capacity: Int
    private static let pixels = Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT)
    private var storage: [UInt32] = []
    private var head = 0 // next write slot
    private(set) var count = 0

    init(capacity: Int) { self.capacity = max(1, capacity) }

    func append(_ src: UnsafePointer<UInt32>) {
        if storage.isEmpty { storage = [UInt32](repeating: 0, count: capacity * Self.pixels) }
        storage.withUnsafeMutableBufferPointer { buf in
            (buf.baseAddress! + head * Self.pixels).update(from: src, count: Self.pixels)
        }
        head = (head + 1) % capacity
        count = min(count + 1, capacity)
    }

    /// back = 0 is the newest frame.
    func withFrame<R>(back: Int, _ body: (UnsafePointer<UInt32>) -> R) -> R? {
        guard back >= 0, back < count else { return nil }
        let slot = (head - 1 - back + capacity * 2) % capacity
        return storage.withUnsafeBufferPointer { body($0.baseAddress! + slot * Self.pixels) }
    }

    func clear() { head = 0; count = 0 }

    /// Releases the memory (leaving practice).
    func release() { clear(); storage = [] }
}

// MARK: - practice: natural audio ending

enum AudioFade {
    /// A short decaying tail built from the last frame's samples, pushed once when practice
    /// reaches B: the sound dies away instead of being cut (no click), the picture stays.
    static func tail(from last: UnsafeBufferPointer<Int16>, repeats: Int = 4) -> [Int16] {
        guard last.count > 0, repeats > 0 else { return [] }
        let n = last.count * repeats
        var out = [Int16](repeating: 0, count: n)
        for i in 0..<n {
            let g = 1.0 - Double(i + 1) / Double(n)   // linear to exactly 0
            out[i] = Int16(clamping: Int((Double(last[i % last.count]) * g * g).rounded()))
        }
        return out
    }
}

// MARK: - fast-forward = replay the recorded take only

/// Fast-forward never records: in record mode the session is switched to replay for the hold
/// and back to record at release (the next recorded frame then continues or branches the take,
/// exactly like after a rewind). It stops at the take end.
struct FastForwardSession {
    private(set) var active = false
    private var restoreRecord = false

    mutating func begin(_ s: EngineSession) throws {
        guard !active else { return }
        active = true
        restoreRecord = s.mode == RN_MODE_RECORD
        if restoreRecord { try s.setMode(RN_MODE_REPLAY) }
    }

    /// Emulates up to n recorded frames. Returns (frames emulated, reached the take end).
    func step(_ s: EngineSession, frames n: Int) throws -> (Int, Bool) {
        var done = 0
        for _ in 0..<n {
            if s.frame >= s.takeLength { return (done, true) }
            let before = s.frame
            let info = try s.step(p1: 0, p2: 0, events: 0)
            if info.end_of_take != 0 && info.frame == before { return (done, true) }
            done += 1
        }
        return (done, s.frame >= s.takeLength)
    }

    mutating func end(_ s: EngineSession) throws {
        guard active else { return }
        active = false
        if restoreRecord { restoreRecord = false; try s.setMode(RN_MODE_RECORD) }
    }

    /// Record mode as the user sees it (record mode is only suspended during the hold).
    func showsRecording(_ s: EngineSession) -> Bool { s.mode == RN_MODE_RECORD || (active && restoreRecord) }
}
