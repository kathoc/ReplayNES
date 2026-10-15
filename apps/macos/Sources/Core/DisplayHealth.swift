// What the GPU and the presentation side report about the live display path (docs/FRAME_PACING.md
// "Display watchdog"): command buffer failures and the newest picture confirmed on screen. Written
// from Metal completion / presented handlers (any thread), read by the emulation thread, which
// feeds DisplayWatchdog and decides recoveries. Platform-neutral (macOS and iOS share it).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Metal
import QuartzCore

/// GPU work of one new picture (the CRT build, or the plain present): whether it completed.
/// Set by its command buffer's completion handler; read by the presented handler of the drawable
/// that shows the picture (a picture whose build failed is not "confirmed", so a stale CRT output
/// is noticed by the watchdog even though drawables keep being presented).
final class PictureWork {
    private let lock = NSLock()
    private var state = 0   // 0 pending, 1 completed, -1 failed
    func complete(ok: Bool) { lock.lock(); state = ok ? 1 : -1; lock.unlock() }
    var completed: Bool { lock.lock(); defer { lock.unlock() }; return state == 1 }
    var failed: Bool { lock.lock(); defer { lock.unlock() }; return state == -1 }
}

final class DisplayHealth {
    struct Snapshot: Equatable {
        /// FrameBuffer sequence of the newest picture confirmed on screen (0: none yet).
        var confirmedSequence: UInt64 = 0
        /// Emulated frame number of that picture.
        var confirmedFrame: UInt64 = 0
        var confirmedAt = 0.0
        var gpuErrors = 0
        var buildErrors = 0
        var lastError = ""
        var lastErrorAt = 0.0
    }

    private let lock = NSLock()
    private var s = Snapshot()
    private var pendingBuildErrors = 0
    /// Errors logged in full: the first few, then every `logEvery`-th (a persistent failure must not flood the log).
    static let logFirst = 5, logEvery = 300

    var snapshot: Snapshot { lock.lock(); defer { lock.unlock() }; return s }

    /// A picture's GPU work completed without error. Before the platform has reported any
    /// presentation time (virtual machines report none) this is taken as "on screen"; from the
    /// first presentation time on, only `presented` counts (a present whose command buffer
    /// completed may still never reach the screen: dropped, replaced, a layer that is not shown).
    func completed(sequence: UInt64, frame: UInt64, at now: Double) {
        lock.lock()
        if !presentTimesSeen_ { confirmLocked(sequence: sequence, frame: frame, at: now) }
        lock.unlock()
    }

    /// A drawable showing this picture reached the screen (presentedTime > 0) and the GPU work
    /// that built the picture did not fail.
    func presented(sequence: UInt64, frame: UInt64, at now: Double) {
        lock.lock()
        presentTimesSeen_ = true
        confirmLocked(sequence: sequence, frame: frame, at: now)
        lock.unlock()
    }

    /// Kept for callers that know the picture is on screen (tests, platforms without present callbacks).
    func confirmed(sequence: UInt64, frame: UInt64, at now: Double) {
        lock.lock(); confirmLocked(sequence: sequence, frame: frame, at: now); lock.unlock()
    }

    /// Presentation times are reported here: confirmations come from them only.
    var presentTimesSeen: Bool { lock.lock(); defer { lock.unlock() }; return presentTimesSeen_ }
    private var presentTimesSeen_ = false

    private func confirmLocked(sequence: UInt64, frame: UInt64, at now: Double) {
        if sequence != s.confirmedSequence || now > s.confirmedAt {
            s.confirmedSequence = sequence
            s.confirmedFrame = frame
            s.confirmedAt = now
        }
    }

    /// A command buffer of the display path completed with an error. `build`: it built a picture
    /// (CRT passes) rather than presenting one. Returns whether this one should be logged.
    @discardableResult
    func commandBufferFailed(_ description: String, build: Bool, at now: Double) -> Bool {
        lock.lock(); defer { lock.unlock() }
        s.gpuErrors += 1
        if build { s.buildErrors += 1; pendingBuildErrors += 1 }
        s.lastError = description
        s.lastErrorAt = now
        return s.gpuErrors <= Self.logFirst || s.gpuErrors % Self.logEvery == 0
    }

    /// Build failures since the last call (the emulation thread then discards the tube's output and
    /// temporal state, which the failed passes may have left half written).
    func takeBuildErrors() -> Int {
        lock.lock(); defer { lock.unlock() }
        let n = pendingBuildErrors
        pendingBuildErrors = 0
        return n
    }

    /// Text for a failed command buffer (nil when it did not fail).
    static func failure(status: MTLCommandBufferStatus, error: Error?) -> String? {
        guard status == .error else { return nil }
        guard let e = error as NSError? else { return "command buffer error (no detail)" }
        let code = MTLCommandBufferError.Code(rawValue: UInt(e.code)).map(Self.name) ?? "code \(e.code)"
        return "\(code): \(e.localizedDescription)"
    }

    private static func name(_ c: MTLCommandBufferError.Code) -> String {
        switch c {
        case .none: return "none"
        case .timeout: return "timeout"
        case .pageFault: return "pageFault"
        case .notPermitted: return "notPermitted"
        case .outOfMemory: return "outOfMemory"
        case .invalidResource: return "invalidResource"
        case .memoryless: return "memoryless"
        case .stackOverflow: return "stackOverflow"
        case .accessRevoked: return "accessRevoked"
        case .deviceRemoved: return "deviceRemoved"
        case .internal: return "internal"
        default: return "code \(c.rawValue)"
        }
    }
}

/// Display path log lines (failures, recoveries): NSLog, and on iOS also appended to
/// Library/Caches/display-log.txt (kept below ~256 KB) so a freeze reported from a phone can be
/// read back afterwards (`xcrun devicectl device copy from --domain-type appDataContainer`).
enum DisplayLog {
    private static let queue = DispatchQueue(label: "replaynes.display.log", qos: .utility)
    static let maxBytes = 256 * 1024
    /// Lines written so far (tests).
    private static let lock = NSLock()
    private static var count_ = 0
    static var count: Int { lock.lock(); defer { lock.unlock() }; return count_ }

    static var fileURL: URL? {
        #if os(iOS)
        return FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first?.appendingPathComponent("display-log.txt")
        #else
        return nil
        #endif
    }

    static func write(_ message: String) {
        // The text is the format (escaped), not a %@ argument: arguments are <private> in the unified log.
        NSLog(("ReplayNES: " + message).replacingOccurrences(of: "%", with: "%%"))
        lock.lock(); count_ += 1; lock.unlock()
        guard let url = fileURL else { return }
        let line = ISO8601DateFormatter.string(from: Date(), timeZone: .current, formatOptions: [.withInternetDateTime, .withFractionalSeconds])
            + " " + message + "\n"
        queue.async {
            let fm = FileManager.default
            if let size = (try? fm.attributesOfItem(atPath: url.path))?[.size] as? Int, size > maxBytes,
               let data = try? Data(contentsOf: url) {
                try? data.suffix(maxBytes / 2).write(to: url)   // keep the newer half
            }
            if let h = try? FileHandle(forWritingTo: url) {
                h.seekToEndOfFile(); h.write(Data(line.utf8)); try? h.close()
            } else {
                try? Data(line.utf8).write(to: url)
            }
        }
    }
}

/// What a thread is doing and since when (host seconds): the emulation thread marks its phases so
/// a monitor on another thread can tell a thread that stopped (blocked in a GPU call, a lock, a
/// long command) from one that runs but presents nothing.
final class ThreadProbe {
    private let lock = NSLock()
    private var at_ = 0.0
    private var phase_: StaticString = "idle"
    func mark(_ phase: StaticString, at now: Double = CACurrentMediaTime()) {
        lock.lock(); at_ = now; phase_ = phase; lock.unlock()
    }
    var snapshot: (at: Double, phase: String) { lock.lock(); defer { lock.unlock() }; return (at_, "\(phase_)") }
}

/// Command buffers of one queue that were created and not yet completed. The display path never
/// creates more than `limit`: MTLCommandQueue.makeCommandBuffer blocks once its own limit (64) is
/// in flight, and a GPU that stopped completing work (a hang, a fault) would then stop the
/// emulation thread - and the display watchdog with it. Instead the refresh is skipped (nothing
/// new on screen), the watchdog notices and RECOVER replaces the queue (and this counter).
final class CommandBufferBudget {
    static let limit = 16
    private let lock = NSLock()
    private var count_ = 0
    var count: Int { lock.lock(); defer { lock.unlock() }; return count_ }
    /// Takes a slot (false: `limit` reached).
    func acquire(limit: Int = CommandBufferBudget.limit) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard count_ < limit else { return false }
        count_ += 1
        return true
    }
    func release() { lock.lock(); count_ = max(0, count_ - 1); lock.unlock() }
}

/// Fault injection for the display path (tests and on-device checks; docs/FRAME_PACING.md
/// "Display watchdog"): `-displayFault crtStale,noPresent,gpuHang,linkStall,adaptiveFlip` (any
/// subset) at launch, or set from tests. Each models a failure the watchdog must get out of:
///   crtStale     CRT builds silently stop after the first picture (a stale tube output, no error)
///   noPresent    with the CRT model on, drawables are never presented (pictures that never reach
///                the screen although their command buffers complete)
///   gpuHang      every 10 s a CRT build waits 3 s on the GPU (a hung queue: work piles up behind it)
///   linkStall    every 10 s the display link's callbacks are ignored for 1.5 s (a lost display link)
///   adaptiveFlip the CRT tube size changes every half second (adaptive scale / thermal / size churn)
///   skipPresents every 10 s, for 1.5 s, display link updates arrive but nothing is presented
/// `crtInjectGPUError N` (CRT builds reported failed) is the GPU-error case.
final class DisplayFaults {
    enum Fault: String, CaseIterable { case crtStale, noPresent, gpuHang, linkStall, adaptiveFlip, skipPresents, linkRestart }
    static let shared = DisplayFaults(UserDefaults.standard.string(forKey: "displayFault") ?? "")
    private let lock = NSLock()
    private var active_: Set<Fault>
    init(_ list: String) {
        active_ = Set(list.split(separator: ",").compactMap { Fault(rawValue: $0.trimmingCharacters(in: .whitespaces)) })
    }
    var active: Set<Fault> {
        get { lock.lock(); defer { lock.unlock() }; return active_ }
        set { lock.lock(); active_ = newValue; lock.unlock() }
    }
    func on(_ f: Fault) -> Bool { lock.lock(); defer { lock.unlock() }; return active_.contains(f) }
    /// True during the first `length` seconds of every `period` (host seconds `now`).
    static func window(_ now: Double, period: Double, length: Double) -> Bool {
        now.truncatingRemainder(dividingBy: period) < length
    }
}

/// Pictures that reached the screen, each counted once at its first appearance: through its own
/// present or - when that drawable was replaced before the display latched it (presentedTime 0,
/// "dropped") - through the repeat present that showed the same picture a refresh later. The
/// display watchdog confirms pictures the same way, so a run of dropped own presents whose
/// repeats are shown is not a stall (the picture is late by a refresh, not frozen).
struct OnScreenCounter {
    private(set) var count: UInt64 = 0
    private var last: UInt64?
    /// A drawable showing the picture of emulated frame `frame` reached the screen.
    mutating func shown(frame: UInt64) {
        guard frame != last else { return }
        last = frame
        count &+= 1
    }
}
