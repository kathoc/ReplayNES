// Transport / practice wrappers against a real engine session: default hotkeys through rn_input,
// record toggle, practice A/B loop, fast-forward without recording. The state machines themselves
// are tested in tests/test_frontend_playback.cpp / test_frontend_input.cpp.
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

    /// The defaults through the engine input pipeline: triggers/shoulders are hotkeys and never
    /// reach the recorded game input.
    func testHotkeysAreNotGameInput() throws {
        let h = rn_input_new()!
        defer { rn_input_free(h) }
        XCTAssertEqual(rn_input_load_json(h, InputCatalog.defaultConfigJSON()), RN_OK)
        var e: UInt32 = 0, held: UInt32 = 0, p1: UInt8 = 0, p2: UInt8 = 0
        rn_input_poll_hotkeys(h, &e, &held)

        rn_input_set_pressed(h, "gc0:leftTrigger", 1)
        rn_input_poll_hotkeys(h, &e, &held)
        XCTAssertNotEqual(held & UInt32(RN_HK_REWIND), 0)
        rn_input_sample_game(h, 0, &p1, &p2)
        XCTAssertEqual(p1, 0)
        rn_input_set_pressed(h, "gc0:leftTrigger", 0)

        rn_input_set_pressed(h, "gc0:rightTrigger", 1)
        rn_input_poll_hotkeys(h, &e, &held)
        XCTAssertNotEqual(held & UInt32(RN_HK_FAST_FORWARD), 0)
        rn_input_set_pressed(h, "gc0:rightTrigger", 0)

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

    // MARK: record toggle

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
}
