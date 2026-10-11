// What the GPU and the presentation side report about the live display path (docs/FRAME_PACING.md
// "Display watchdog"): command buffer failures and the newest picture confirmed on screen. Written
// from Metal completion / presented handlers (any thread), read by the emulation thread, which
// feeds DisplayWatchdog and decides recoveries. Platform-neutral (macOS and iOS share it).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Metal

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

    /// A picture is on screen (presentedTime > 0) and its GPU work completed without error.
    func confirmed(sequence: UInt64, frame: UInt64, at now: Double) {
        lock.lock()
        if sequence != s.confirmedSequence || now > s.confirmedAt {
            s.confirmedSequence = sequence
            s.confirmedFrame = frame
            s.confirmedAt = now
        }
        lock.unlock()
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
