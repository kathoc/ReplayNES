// Transport / practice logic: controller hotkey defaults, record toggle, paused stepping,
// practice A/B loop (fake clock + real engine session), fast-forward without recording.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class PlaybackLogicTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-playback-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws { try? FileManager.default.removeItem(at: tmp) }

    private func newSession() throws -> EngineSession {
        let rom = tmp.appendingPathComponent("test.nes")
        if !FileManager.default.fileExists(atPath: rom.path) { try Engine.writeTestROM(to: rom) }
        let s = try EngineSession.create(rom: rom, projectDir: nil)
        try s.setMode(RN_MODE_RECORD)
        return s
    }

    private func record(_ s: EngineSession, _ n: Int, seed: Int = 0) throws {
        for i in 0..<n {
            let f = i + seed
            var p1: UInt8 = 0
            if f % 30 < 12 { p1 |= UInt8(RN_BTN_RIGHT) }
            if f % 17 == 0 { p1 |= UInt8(RN_BTN_A) }
            try s.step(p1: p1, p2: 0, events: 0)
        }
    }

    // MARK: controller hotkeys

    func testDefaultControllerHotkeys() {
        let b = Set(InputCatalog.defaultBindings.map { "\($0.0)=\($0.1)" })
        XCTAssertTrue(b.contains("gc0:rightTrigger=hk.rewind"), "R2 hold = rewind")
        XCTAssertTrue(b.contains("gc0:leftTrigger=hk.fast_forward"), "L2 hold = fast-forward")
        XCTAssertTrue(b.contains("gc0:leftShoulder=hk.slow"), "L = slow toggle")
        XCTAssertTrue(b.contains("gc0:rightShoulder=hk.pause"), "R = pause/play")
        // Keyboard equivalents stay.
        for k in ["kb:51=hk.rewind", "kb:48=hk.fast_forward", "kb:49=hk.pause", "kb:37=hk.slow", "kb:43=hk.step_back", "kb:47=hk.frame_advance"] {
            XCTAssertTrue(b.contains(k), k)
        }
        // Old controller layout gone; the D-pad is never a hotkey.
        XCTAssertFalse(b.contains("gc0:leftShoulder=hk.rewind"))
        XCTAssertFalse(b.contains("gc0:rightTrigger=hk.frame_advance"))
        XCTAssertFalse(InputCatalog.defaultBindings.contains { $0.0.contains("dpad") && $0.1.hasPrefix("hk.") })
    }

    /// The defaults through the engine input pipeline: triggers/shoulders are hotkeys and never
    /// reach the recorded game input.
    func testHotkeysAreNotGameInput() throws {
        let h = rn_input_new()!
        defer { rn_input_free(h) }
        XCTAssertEqual(rn_input_load_json(h, InputCatalog.defaultConfigJSON()), RN_OK)
        var e: UInt32 = 0, held: UInt32 = 0, p1: UInt8 = 0, p2: UInt8 = 0
        rn_input_poll_hotkeys(h, &e, &held)

        rn_input_set_pressed(h, "gc0:rightTrigger", 1)
        rn_input_poll_hotkeys(h, &e, &held)
        XCTAssertNotEqual(held & UInt32(RN_HK_REWIND), 0)
        rn_input_sample_game(h, 0, &p1, &p2)
        XCTAssertEqual(p1, 0)
        rn_input_set_pressed(h, "gc0:rightTrigger", 0)

        rn_input_set_pressed(h, "gc0:leftTrigger", 1)
        rn_input_poll_hotkeys(h, &e, &held)
        XCTAssertNotEqual(held & UInt32(RN_HK_FAST_FORWARD), 0)
        rn_input_set_pressed(h, "gc0:leftTrigger", 0)

        rn_input_set_pressed(h, "gc0:leftShoulder", 1)
        rn_input_poll_hotkeys(h, &e, &held)
        XCTAssertNotEqual(e & UInt32(RN_HK_SLOW), 0)
        rn_input_set_pressed(h, "gc0:leftShoulder", 0)

        rn_input_set_pressed(h, "gc0:rightShoulder", 1)
        rn_input_poll_hotkeys(h, &e, &held)
        XCTAssertNotEqual(e & UInt32(RN_HK_PAUSE), 0)
        rn_input_sample_game(h, 1, &p1, &p2)
        XCTAssertEqual(p1, 0)
    }

    func testPausedDpadStepDirections() {
        let c = InputCatalog.parse(InputCatalog.defaultConfigJSON())!
        let d = InputCatalog.pausedStepDirections(c)
        XCTAssertEqual(d["gc0:dpad.left"], -1)
        XCTAssertEqual(d["gc0:dpad.right"], 1)
        XCTAssertEqual(d["gc1:dpad.right"], 1)
        XCTAssertNil(d["gc0:lstick.left"], "sticks never step")
        XCTAssertNil(d["kb:123"], "keyboard arrows stay game input (, and . step)")
        XCTAssertNil(d["gc0:dpad.up"])
    }

    func testControllerLayoutMigration() {
        // A 0.1.x bindings file: old controller hotkeys.
        var legacy = InputCatalog.parse(InputCatalog.defaultConfigJSON())!
        legacy.bindings.removeAll { b in InputCatalog.controllerHotkeys.contains { $0.0 == b.input && $0.1 == b.action } }
        legacy.bindings += InputCatalog.legacyControllerHotkeys.map { (input: $0.0, action: $0.1) }
        let m = InputCatalog.controllerLayoutMigration(legacy)
        XCTAssertEqual(m.bind.map { $0.0 + $0.1 }, InputCatalog.controllerHotkeys.map { $0.0 + $0.1 })
        XCTAssertEqual(m.unbind.count, 4)
        // Already new, or customised: untouched.
        XCTAssertTrue(InputCatalog.controllerLayoutMigration(InputCatalog.parse(InputCatalog.defaultConfigJSON())!).bind.isEmpty)
        var custom = legacy
        custom.bindings.append((input: "gc0:buttonY", action: "hk.bookmark"))
        XCTAssertTrue(InputCatalog.controllerLayoutMigration(custom).bind.isEmpty)
    }

    func testSlowToggle() {
        XCTAssertEqual(SlowRate.normal.toggled, .half)
        XCTAssertEqual(SlowRate.half.toggled, .normal)
        XCTAssertEqual(SlowRate.quarter.toggled, .normal)
    }

    func testStepRepeater() {
        var r = StepRepeater()
        XCTAssertEqual(r.press(-1), -1, "press steps immediately")
        var steps: [Int] = []
        for t in 1...30 { let d = r.tick(); if d != 0 { steps.append(t) } }
        XCTAssertEqual(steps.first, StepRepeater.initialDelayTicks, "repeat only after the delay")
        XCTAssertEqual(steps, Array(stride(from: StepRepeater.initialDelayTicks, through: 30, by: StepRepeater.intervalTicks)))
        r.release(-1)
        XCTAssertEqual(r.tick(), 0)
        XCTAssertEqual(r.press(1), 1)
        r.release(-1) // other direction: still held
        XCTAssertEqual(r.direction, 1)
    }

    // MARK: record toggle

    func testRecordToggleStateMachine() {
        // record -> replay at the take end: jump to the start and play.
        XCTAssertEqual(RecordToggle.plan(recording: true, frame: 300, takeLength: 300), .init(record: false, seek: 0, play: true))
        // record -> replay mid-take (last seek position): play from here.
        XCTAssertEqual(RecordToggle.plan(recording: true, frame: 120, takeLength: 300), .init(record: false, seek: nil, play: true))
        // nothing recorded: stays put, no play.
        XCTAssertEqual(RecordToggle.plan(recording: true, frame: 0, takeLength: 0).play, false)
        // replay -> record: current position, paused.
        XCTAssertEqual(RecordToggle.plan(recording: false, frame: 150, takeLength: 300), .init(record: true, seek: nil, play: false))
        // play pressed in replay at the end restarts; not in record mode / practice.
        XCTAssertTrue(RecordToggle.shouldRestartOnPlay(recording: false, practicing: false, frame: 300, takeLength: 300))
        XCTAssertFalse(RecordToggle.shouldRestartOnPlay(recording: true, practicing: false, frame: 300, takeLength: 300))
        XCTAssertFalse(RecordToggle.shouldRestartOnPlay(recording: false, practicing: true, frame: 300, takeLength: 300))
        XCTAssertFalse(RecordToggle.shouldRestartOnPlay(recording: false, practicing: false, frame: 10, takeLength: 300))
    }

    /// The replay toggle really plays: replay mode steps recorded frames from the start.
    func testReplayToggleActuallyPlays() throws {
        let s = try newSession()
        try record(s, 90)
        let plan = RecordToggle.plan(recording: true, frame: s.frame, takeLength: s.takeLength)
        try s.setMode(plan.record ? RN_MODE_RECORD : RN_MODE_REPLAY)
        if let f = plan.seek { try s.seek(f) }
        XCTAssertEqual(s.frame, 0)
        XCTAssertTrue(plan.play)
        for _ in 0..<30 { try s.step(p1: 0xFF, p2: 0, events: 0) } // inputs are ignored in replay
        XCTAssertEqual(s.frame, 30)
        XCTAssertEqual(s.takeLength, 90)
        XCTAssertEqual(s.takes().count, 1)
    }

    // MARK: practice loop

    func testPracticeLoopStateMachineFakeClock() {
        var loop = PracticeLoop()
        var now = 0.0
        var counter: UInt64 = 0
        let length: UInt64 = 10
        var actions: [PracticeLoop.Action] = []
        // play to B
        while true {
            let a = loop.tick(now: now, counter: counter, length: length)
            actions.append(a)
            if a == .step { counter += 1 } else { break }
            now += 1.0 / 60
        }
        XCTAssertEqual(actions.filter { $0 == .step }.count, 10)
        XCTAssertEqual(actions.last, .beginHold)
        let holdStart = now
        // hold 0.5 s: last frame kept
        var a = PracticeLoop.Action.hold
        while case .hold = a { now += 1.0 / 60; a = loop.tick(now: now, counter: counter, length: length) }
        XCTAssertGreaterThanOrEqual(now - holdStart, PracticeLoop.holdSeconds)
        XCTAssertEqual(a, .rewindFrame(back: 0))
        // rewind animation ~0.5 s, going further back each tick
        let rwStart = now
        var lastBack = -1.0
        while true {
            now += 1.0 / 60
            a = loop.tick(now: now, counter: counter, length: length)
            if case .rewindFrame(let b) = a { XCTAssertGreaterThan(b, lastBack); lastBack = b } else { break }
        }
        XCTAssertEqual(a, .restart)
        XCTAssertGreaterThanOrEqual(now - rwStart, PracticeLoop.rewindSeconds)
        XCTAssertEqual(loop.loops, 1)
        // after goto A (counter 0) it plays again
        XCTAssertEqual(loop.tick(now: now, counter: 0, length: length), .step)
        // no B: never loops
        var free = PracticeLoop()
        XCTAssertEqual(free.tick(now: 0, counter: 9999, length: nil), .step)
        // interrupt during hold -> playing
        var l2 = PracticeLoop()
        _ = l2.tick(now: 0, counter: 10, length: 10)
        XCTAssertTrue(l2.isLooping)
        l2.interrupt()
        XCTAssertEqual(l2.tick(now: 1, counter: 5, length: 10), .step)
        XCTAssertEqual(PracticeLoop.historyIndex(back: 0, count: 60), 0)
        XCTAssertEqual(PracticeLoop.historyIndex(back: 0.999, count: 60), 59)
        XCTAssertEqual(PracticeLoop.historyIndex(back: 0.5, count: 0), 0)
    }

    /// Drives the same loop the emulation thread runs, against a real session: reaching B ->
    /// hold -> rewind animation -> goto A, replaying the identical section; the take is untouched.
    func testPracticeLoopOnEngineNeverRecords() throws {
        let s = try newSession()
        try record(s, 30)
        try s.practiceSetA(0)
        try record(s, 60, seed: 30)
        try s.practiceSetB(0)
        XCTAssertEqual(s.practiceSlot(0).length, 60)
        let takeLen = s.takeLength, frame = s.frame, takes = s.takes().count, hash = s.stateHash()

        try s.practiceGotoA(0)
        XCTAssertEqual(s.mode, RN_MODE_PRACTICE)
        // rn_video is not refreshed by loading A; compare the first frame emulated after A.
        try s.step(p1: UInt8(RN_BTN_RIGHT), p2: 0, events: 0)
        let videoAfterA = s.videoHash
        var stepsSinceA = 1
        var firstFrames: [UInt64] = []
        var loop = PracticeLoop()
        var now = 0.0
        var restarts = 0
        var videoAtB: [UInt64] = []
        let history = FrameHistory(capacity: 60)
        var shownBack = 0
        while restarts < 3 {
            now += 1.0 / 60
            switch loop.tick(now: now, counter: s.practiceFrame, length: s.practiceSlot(0).length) {
            case .step:
                try s.step(p1: UInt8(RN_BTN_RIGHT), p2: 0, events: 0) // practice input is live but never recorded
                stepsSinceA += 1
                if stepsSinceA == 1 { firstFrames.append(s.videoHash) }
                if let v = s.video { history.append(v) }
            case .beginHold:
                videoAtB.append(s.videoHash)
                XCTAssertEqual(s.practiceFrame, 60)
            case .hold: break
            case .rewindFrame(let back):
                let idx = PracticeLoop.historyIndex(back: back, count: history.count)
                XCTAssertNotNil(history.withFrame(back: idx) { _ in true })
                shownBack = max(shownBack, idx)
            case .restart:
                try s.practiceGotoA(0)
                history.clear()
                restarts += 1
                stepsSinceA = 0
                XCTAssertEqual(s.practiceFrame, 0)
            }
            XCTAssertEqual(s.takeLength, takeLen)
            XCTAssertEqual(s.frame, frame)
        }
        XCTAssertEqual(loop.loops, 3)
        XCTAssertEqual(firstFrames.count, 2)
        XCTAssertTrue(firstFrames.allSatisfy { $0 == videoAfterA }, "each loop restarts from the same A")
        XCTAssertEqual(Set(videoAtB).count, 1, "every loop replays the same A->B section")
        XCTAssertGreaterThan(shownBack, 20, "the rewind animation walks back through recent frames")

        // R2 in practice rewinds the practice run only and stops at A.
        for _ in 0..<10 { try s.step(p1: 0, p2: 0, events: 0) }
        try s.rewind(100)
        XCTAssertEqual(s.practiceFrame, 0)

        try s.setMode(RN_MODE_RECORD) // "Stop Practicing"
        XCTAssertEqual(s.takeLength, takeLen)
        XCTAssertEqual(s.frame, frame)
        XCTAssertEqual(s.takes().count, takes)
        XCTAssertEqual(s.stateHash(), hash, "the take is exactly as before practice")
    }

    func testSetBAfterSeekIsDiscontinuity() throws {
        let s = try newSession()
        try record(s, 40)
        try s.practiceSetA(1)
        try s.seek(10)
        try s.setMode(RN_MODE_REPLAY)
        for _ in 0..<5 { try s.step(p1: 0, p2: 0, events: 0) }
        XCTAssertThrowsError(try s.practiceSetB(1)) { e in
            XCTAssertEqual((e as? RNError)?.status, RN_ERR_DISCONTINUITY)
        }
        XCTAssertThrowsError(try s.practiceSetB(2)) { e in XCTAssertEqual((e as? RNError)?.status, RN_ERR_NOT_FOUND) }
    }

    // MARK: fast-forward

    func testFastForwardStopsAtTakeEndWithoutRecording() throws {
        let s = try newSession()
        try record(s, 100)
        try s.seek(40)
        let take = s.activeTake, takes = s.takes().count
        var ff = FastForwardSession()
        try ff.begin(s)
        XCTAssertTrue(ff.showsRecording(s), "still record mode for the user")
        XCTAssertEqual(s.mode, RN_MODE_REPLAY)
        var reachedEnd = false
        var guardN = 0
        while !reachedEnd && guardN < 100 {
            let (_, end) = try ff.step(s, frames: 4)
            reachedEnd = end
            guardN += 1
        }
        XCTAssertTrue(reachedEnd)
        XCTAssertEqual(s.frame, 100, "stops exactly at the take end")
        let (n, end) = try ff.step(s, frames: 4)
        XCTAssertEqual(n, 0)
        XCTAssertTrue(end)
        try ff.end(s)
        XCTAssertEqual(s.mode, RN_MODE_RECORD)
        XCTAssertEqual(s.takeLength, 100)
        XCTAssertEqual(s.activeTake, take)
        XCTAssertEqual(s.takes().count, takes, "no branch was created")
    }

    // MARK: helpers

    func testAudioFadeTail() {
        let src = [Int16](repeating: 12000, count: 800)
        let tail = src.withUnsafeBufferPointer { AudioFade.tail(from: $0, repeats: 4) }
        XCTAssertEqual(tail.count, 3200)
        XCTAssertEqual(tail.last, 0)
        XCTAssertLessThanOrEqual(tail.first!, 12000)
        for i in 1..<tail.count { XCTAssertLessThanOrEqual(tail[i], tail[i - 1]) }
        XCTAssertTrue([Int16]().withUnsafeBufferPointer { AudioFade.tail(from: $0) }.isEmpty)
    }

    func testFrameHistoryRing() {
        let h = FrameHistory(capacity: 3)
        let n = Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT)
        for v in 1...5 { let f = [UInt32](repeating: UInt32(v), count: n); f.withUnsafeBufferPointer { h.append($0.baseAddress!) } }
        XCTAssertEqual(h.count, 3)
        XCTAssertEqual(h.withFrame(back: 0) { $0[0] }, 5)
        XCTAssertEqual(h.withFrame(back: 2) { $0[100] }, 3)
        XCTAssertNil(h.withFrame(back: 3) { $0[0] })
        h.release()
        XCTAssertEqual(h.count, 0)
    }
}
