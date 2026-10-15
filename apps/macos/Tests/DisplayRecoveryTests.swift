// Display path robustness (docs/FRAME_PACING.md "Display watchdog"): CRT renderer stuck states
// (line count vs the tube in use, a failed encode leaving a stale output, non-finite temporal
// state, a tube plan never adopted while sizes keep changing), DisplayHealth bookkeeping
// (confirmation by presentation), the command buffer budget on a hung GPU queue, the fault
// injection switches and the DisplayWatchdog bridge (RECOVER, RESTART, FALLBACK).
// SPDX-License-Identifier: GPL-2.0-or-later
import Metal
import XCTest

final class DisplayRecoveryTests: XCTestCase {
    private var device: MTLDevice!
    private var queue: MTLCommandQueue!
    private let codes = [UInt16]((0..<(256 * 240)).map { UInt16(($0 / 7) % 64) })

    override func setUpWithError() throws {
        guard let d = MTLCreateSystemDefaultDevice(), let q = d.makeCommandQueue() else { throw XCTSkip("no Metal device") }
        device = d
        queue = q
    }

    @discardableResult
    private func frame(_ r: CRTRenderer, _ ordinal: UInt64) -> (encoded: Bool, status: MTLCommandBufferStatus) {
        let cb = queue.makeCommandBuffer()!
        let ok = codes.withUnsafeBufferPointer { r.encode(.codes($0.baseAddress!, burstPhase: 0), ordinal: ordinal, into: cb) }
        cb.commit()
        cb.waitUntilCompleted()
        return (ok, cb.status)
    }

    private func allFinite(_ r: CRTRenderer) -> Bool {
        let out = r.read(.output)
        return !out.isEmpty && out.allSatisfy { $0.x.isFinite && $0.y.isFinite && $0.z.isFinite }
    }

    /// A new line count is planned in the background; until that tube is adopted the old one keeps
    /// rendering with stages of its own size (it used to run with stages sized for the new count:
    /// out-of-bounds reads and writes, a GPU fault on iOS).
    func testLineCountFollowsTheTubeInUse() throws {
        let r = try CRTRenderer(device: device, targetPixelFormat: nil)
        var s = CRTRenderer.Settings()
        r.configure(settings: s, outputWidth: 640, outputHeight: 480, synchronous: true)
        XCTAssertTrue(frame(r, 1).encoded)
        s.lines = 160
        r.configure(settings: s, outputWidth: 640, outputHeight: 480)
        let f = frame(r, 2)
        XCTAssertTrue(f.encoded)
        XCTAssertEqual(f.status, .completed)
        XCTAssertEqual(r.renderedLines, 240, "the 160-line plan cannot be ready yet")
        XCTAssertEqual(r.read(.tubeInput).count, 512 * 240, "stages sized for the tube in use")
        // The new plan is adopted by a later frame; from then on everything is 160 lines.
        var ordinal: UInt64 = 3
        let deadline = Date().addingTimeInterval(20)
        while r.renderedLines != 160 && Date() < deadline {
            frame(r, ordinal); ordinal += 1
            usleep(20_000)
        }
        XCTAssertEqual(r.renderedLines, 160)
        XCTAssertTrue(frame(r, ordinal).encoded)
        XCTAssertEqual(r.read(.tubeInput).count, 512 * 160)
        XCTAssertTrue(allFinite(r))
    }

    /// A frame that could not be encoded must not leave the previous picture on screen as if it
    /// were current (the show pass would repeat it forever while the game plays on).
    func testFailedEncodeDropsTheStaleOutput() throws {
        let r = try CRTRenderer(device: device, targetPixelFormat: nil)
        r.configure(settings: CRTRenderer.Settings(), outputWidth: 640, outputHeight: 480, synchronous: true)
        XCTAssertTrue(frame(r, 1).encoded)
        XCTAssertTrue(r.hasOutput)
        r.debugFailEncodes = 1
        XCTAssertFalse(frame(r, 2).encoded)
        XCTAssertFalse(r.hasOutput)
        XCTAssertNil(r.outputBuffer)
        XCTAssertTrue(frame(r, 3).encoded)
        XCTAssertTrue(r.hasOutput)
        // A GPU failure reported later (DisplayHealth -> discardOutput): nothing shown until rebuilt.
        r.discardOutput()
        XCTAssertFalse(r.hasOutput)
        XCTAssertTrue(frame(r, 4).encoded)
        XCTAssertTrue(r.hasOutput)
        XCTAssertTrue(allFinite(r))
    }

    /// NaN in the AGC gain or the supply state would persist into every later frame (a constant
    /// picture); it is detected before the next frame and the state restarts.
    func testNonFiniteTemporalStateIsReset() throws {
        for quality in [CRTRenderer.Quality.fast, .reference] {
            let r = try CRTRenderer(device: device, targetPixelFormat: nil)
            r.quality = quality
            r.configure(settings: CRTRenderer.Settings(), outputWidth: 640, outputHeight: 480, synchronous: true)
            XCTAssertTrue(frame(r, 1).encoded)
            r.debugPoisonState()
            XCTAssertTrue(frame(r, 2).encoded)
            XCTAssertEqual(r.stateResets, 1, "\(quality)")
            XCTAssertTrue(frame(r, 3).encoded)
            XCTAssertEqual(r.stateResets, 1)
            XCTAssertTrue(allFinite(r), "\(quality)")
        }
    }

    /// Many frames with the tube size, line count and effects changing under them (adaptive
    /// scale, rotation, settings), several command buffers in flight: every command buffer
    /// completes and the picture stays finite.
    func testStressConfigurationChangesInFlight() throws {
        let r = try CRTRenderer(device: device, targetPixelFormat: nil)
        let sizes = [(1156, 867), (984, 738), (836, 627), (1600, 1200), (512, 384)]
        let lines = [240, 160, 240]
        let sem = DispatchSemaphore(value: 3)
        let lock = NSLock()
        var failures = 0
        var encoded = 0
        for i in 0..<240 {
            var s = CRTRenderer.Settings()
            s.lines = lines[(i / 80) % lines.count]
            s.persistence = (i / 50) % 2 == 0
            s.supply = (i / 70) % 2 == 0
            let (w, h) = sizes[(i / 15) % sizes.count]
            r.configure(settings: s, outputWidth: w, outputHeight: h, synchronous: i == 0)
            sem.wait()
            let cb = queue.makeCommandBuffer()!
            if codes.withUnsafeBufferPointer({ r.encode(.codes($0.baseAddress!, burstPhase: UInt32(i % 3)), ordinal: UInt64(i % 97 == 96 ? 1 : i + 1), into: cb) }) {
                encoded += 1
            }
            cb.addCompletedHandler { b in
                if b.status != .completed { lock.lock(); failures += 1; lock.unlock() }
                sem.signal()
            }
            cb.commit()
            usleep(4000)
        }
        for _ in 0..<3 { sem.wait() }
        for _ in 0..<3 { sem.signal() }   // a semaphore must not be released below its initial value
        XCTAssertEqual(failures, 0)
        XCTAssertGreaterThan(encoded, 200)
        XCTAssertEqual(r.stateResets, 0)
        XCTAssertTrue(frame(r, 1000).encoded)
        XCTAssertTrue(allFinite(r))
    }

    func testDisplayHealthBookkeeping() {
        let h = DisplayHealth()
        XCTAssertNil(DisplayHealth.failure(status: .completed, error: nil))
        let e = NSError(domain: MTLCommandBufferErrorDomain, code: Int(MTLCommandBufferError.Code.pageFault.rawValue),
                        userInfo: [NSLocalizedDescriptionKey: "Caused GPU Address Fault Error"])
        let text = DisplayHealth.failure(status: .error, error: e)
        XCTAssertEqual(text?.hasPrefix("pageFault"), true)
        var logged = 0
        for i in 0..<(DisplayHealth.logEvery + 10) where h.commandBufferFailed(text!, build: i % 2 == 0, at: Double(i)) { logged += 1 }
        XCTAssertEqual(logged, DisplayHealth.logFirst + 1, "first few, then every logEvery-th")
        XCTAssertEqual(h.snapshot.gpuErrors, DisplayHealth.logEvery + 10)
        XCTAssertEqual(h.takeBuildErrors(), (DisplayHealth.logEvery + 10 + 1) / 2)
        XCTAssertEqual(h.takeBuildErrors(), 0)
        h.confirmed(sequence: 7, frame: 70, at: 1.5)
        XCTAssertEqual(h.snapshot.confirmedSequence, 7)
        XCTAssertEqual(h.snapshot.confirmedFrame, 70)
        let w = PictureWork()
        XCTAssertFalse(w.completed || w.failed)
        w.complete(ok: false)
        XCTAssertTrue(w.failed)
    }

    /// The Swift bridge of rnf_display_watchdog (the policy itself: tests/test_frontend_pacing.cpp).
    func testDisplayWatchdogBridge() {
        var w = DisplayWatchdog()
        var t = 10.0
        var actions: [DisplayWatchdog.Action] = []
        for _ in 0..<60 {
            w.emulated(at: t)
            let a = w.check(at: t)
            if a != .none { actions.append(a) }
            t += 1 / 60.0
        }
        XCTAssertEqual(actions, [.recover, .restart, .fallback])
        XCTAssertGreaterThanOrEqual(w.fallbackLeft(at: t), DisplayWatchdog.fallbackHold - 1.1)
        XCTAssertGreaterThan(w.waiting(at: t), 0.9)
        w.presented(at: t)
        XCTAssertEqual(w.waiting(at: t), 0)
        XCTAssertEqual(w.check(at: t), .none)
        XCTAssertEqual(w.actions, 3)
        XCTAssertEqual(w.fallbackLeft(at: t + DisplayWatchdog.fallbackHold + 1), 0)
    }

    /// Where presentation times are reported, a present whose command buffer completed does not
    /// confirm its picture by itself (it may never reach the screen: dropped, replaced, a layer
    /// that is not shown); before any was reported (virtual machines) completion is all there is.
    func testPresentationTimesDecideConfirmation() {
        let h = DisplayHealth()
        h.completed(sequence: 1, frame: 10, at: 1)
        XCTAssertEqual(h.snapshot.confirmedSequence, 1, "no presentation times yet: completion confirms")
        XCTAssertFalse(h.presentTimesSeen)
        h.presented(sequence: 2, frame: 11, at: 2)
        XCTAssertTrue(h.presentTimesSeen)
        XCTAssertEqual(h.snapshot.confirmedSequence, 2)
        h.completed(sequence: 3, frame: 12, at: 3)
        XCTAssertEqual(h.snapshot.confirmedSequence, 2, "completed but never shown: not confirmed")
        XCTAssertEqual(h.snapshot.confirmedAt, 2)
        h.presented(sequence: 3, frame: 12, at: 4)
        XCTAssertEqual(h.snapshot.confirmedSequence, 3)
    }

    /// A GPU queue that stops completing work (fault: an event that is not signalled) must not
    /// block the thread that encodes: makeCommandBuffer blocks once the queue's own limit is in
    /// flight, so the display path takes at most CommandBufferBudget.limit and skips refreshes.
    func testCommandBufferBudgetOnAHungQueue() throws {
        guard let event = device.makeSharedEvent() else { throw XCTSkip("no shared events") }
        let budget = CommandBufferBudget()
        var committed = 0
        let start = Date()
        for _ in 0..<(CommandBufferBudget.limit * 4) {
            guard budget.acquire() else { continue }
            let cb = queue.makeCommandBuffer()!
            cb.encodeWaitForEvent(event, value: 1)   // hangs until signalled
            cb.addCompletedHandler { _ in budget.release() }
            cb.commit()
            committed += 1
        }
        XCTAssertLessThan(Date().timeIntervalSince(start), 1, "never blocked")
        XCTAssertEqual(committed, CommandBufferBudget.limit)
        XCTAssertEqual(budget.count, CommandBufferBudget.limit)
        XCTAssertFalse(budget.acquire())
        event.signaledValue = 1
        let deadline = Date().addingTimeInterval(10)
        while budget.count > 0 && Date() < deadline { usleep(5000) }
        XCTAssertEqual(budget.count, 0)
        XCTAssertTrue(budget.acquire())
    }

    /// Sizes that change faster than a tube plan is built (rotation, adaptive scale, thermal
    /// steps; -displayFault adaptiveFlip) used to discard every finished plan: the CRT picture
    /// never appeared. A finished plan is adopted; the newest wanted one follows.
    func testTubePlanAdoptedWhileSizesKeepChanging() throws {
        let r = try CRTRenderer(device: device, targetPixelFormat: nil)
        let sizes = [(1204, 903), (1023, 767)]
        var i = 0
        let deadline = Date().addingTimeInterval(20)
        while r.outputSize == nil && Date() < deadline {
            let (w, h) = sizes[i % 2]
            r.configure(settings: CRTRenderer.Settings(), outputWidth: w, outputHeight: h)   // a new size every frame
            frame(r, UInt64(i + 1))
            i += 1
            usleep(2000)
        }
        XCTAssertNotNil(r.outputSize, "a tube plan was adopted")
        XCTAssertTrue(frame(r, UInt64(i + 1)).encoded)
        XCTAssertTrue(allFinite(r))
    }

    /// The watchdog over what the display path reports, as EmulationController feeds it: each
    /// published picture, and the confirmed sequence whenever it changes. Returns its actions.
    private func runWatchdog(frames: Int, _ present: (DisplayHealth, UInt64, Double) -> Void) -> [DisplayWatchdog.Action] {
        let h = DisplayHealth()
        h.presented(sequence: 0, frame: 0, at: 0)   // the platform reports presentation times (iPhone)
        var w = DisplayWatchdog()
        var confirmed: UInt64 = 0
        var actions: [DisplayWatchdog.Action] = []
        var t = 10.0
        for seq in UInt64(1)...UInt64(frames) {
            w.emulated(at: t)
            present(h, seq, t)
            let snap = h.snapshot
            if snap.confirmedSequence != confirmed { confirmed = snap.confirmedSequence; w.presented(at: snap.confirmedAt) }
            let a = w.check(at: t)
            if a != .none { actions.append(a) }
            t += 1 / 60.0
        }
        return actions
    }

    /// iPhone 17 Pro, CRT on, 120 Hz: for seconds at a time every new frame's own drawable was
    /// replaced before the display latched it (presentedTime 0: "shown 0.5 fps" in the stats) and
    /// the repeat present a refresh later showed the picture (60 pictures / s on screen, the
    /// picture never froze). That is a refresh of latency, not a stall: no recovery, and the
    /// on-screen count sees every picture.
    func testDroppedOwnPresentsShownByRepeatsAreNotAStall() {
        var counter = OnScreenCounter()
        let actions = runWatchdog(frames: 600) { h, seq, t in
            h.completed(sequence: seq, frame: seq, at: t)                  // own present: completed, never shown
            h.presented(sequence: seq, frame: seq, at: t + 1 / 120.0)      // the repeat shows it
            counter.shown(frame: seq)                                     // (repeat of the same picture)
            counter.shown(frame: seq)
        }
        XCTAssertEqual(actions, [])
        XCTAssertEqual(counter.count, 600)
    }

    /// Pictures whose drawables complete but never reach the screen - neither their own present
    /// nor a repeat (presentedTime 0 throughout) - while the game plays on: the watchdog must act
    /// (RECOVER, RESTART, FALLBACK) although every command buffer completed without error.
    func testPicturesThatNeverReachTheScreenTriggerTheWatchdog() {
        let actions = runWatchdog(frames: 60) { h, seq, t in
            h.completed(sequence: seq, frame: seq, at: t)
        }
        XCTAssertEqual(actions, [.recover, .restart, .fallback])
    }

    func testFaultSwitchesAndProbe() {
        let f = DisplayFaults("crtStale, gpuHang,bogus")
        XCTAssertEqual(f.active, [.crtStale, .gpuHang])
        XCTAssertTrue(f.on(.crtStale))
        XCTAssertFalse(f.on(.noPresent))
        f.active = []
        XCTAssertFalse(f.on(.crtStale))
        XCTAssertTrue(DisplayFaults.window(100.2, period: 10, length: 1.5))
        XCTAssertFalse(DisplayFaults.window(101.6, period: 10, length: 1.5))
        let p = ThreadProbe()
        p.mark("present", at: 5)
        XCTAssertEqual(p.snapshot.at, 5)
        XCTAssertEqual(p.snapshot.phase, "present")
        let before = DisplayLog.count
        DisplayLog.write("test line")
        XCTAssertEqual(DisplayLog.count, before + 1)
    }
}
