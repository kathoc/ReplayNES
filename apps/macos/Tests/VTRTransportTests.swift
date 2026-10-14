// VTR transport wrappers (docs/design/VTR_UX.md) against a real engine session: the overwrite rule,
// retry undo through RetryTracker (what EmulationController.stepOnce / undoRetry do), the status
// label, the coach lines and the Make Video presets. The state machines themselves are tested in
// tests/test_frontend_tape.cpp and tests/test_retry_takes.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class VTRTransportTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-vtr-\(UUID().uuidString)")
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

    private func play(_ r: RetryTracker, _ s: EngineSession, _ n: Int, now: inout Double, button: UInt8 = 0) throws {
        for i in 0..<n {
            now += 1.0 / 60
            try r.step(s, now: now, p1: i % 20 < 8 ? button : 0, p2: 0, events: 0)
        }
    }

    func testOverwriteRule() {
        XCTAssertTrue(Tape.overwrites(mode: RN_MODE_RECORD, frame: 10, length: 100))
        XCTAssertFalse(Tape.overwrites(mode: RN_MODE_RECORD, frame: 100, length: 100))
        XCTAssertFalse(Tape.overwrites(mode: RN_MODE_REPLAY, frame: 10, length: 100))
        XCTAssertFalse(Tape.overwrites(mode: RN_MODE_PRACTICE, frame: 10, length: 100))
    }

    /// Rewind, play from there (a retry), Undo within 10 s: back at the old continuation's end, the
    /// attempt dropped; after the window the chip is gone.
    func testRetryThenUndoRestoresTheContinuation() throws {
        let s = try newSession()
        let r = RetryTracker()
        var now = 100.0
        try play(r, s, 180, now: &now, button: UInt8(RN_BTN_RIGHT))
        let original = s.activeTake, length = s.takeLength
        XCTAssertFalse(r.canUndo(now: now, activeTake: s.activeTake, mode: s.mode))

        try s.rewind(60)
        try play(r, s, 30, now: &now, button: UInt8(RN_BTN_A))
        XCTAssertNotEqual(s.activeTake, original, "the retry plays on a new attempt")
        XCTAssertTrue(r.canUndo(now: now, activeTake: s.activeTake, mode: s.mode))
        XCTAssertGreaterThan(r.undoLeft(now: now), 0)

        try r.applyUndo(s, now: now)
        XCTAssertEqual(s.activeTake, original)
        XCTAssertEqual(s.takeLength, length)
        XCTAssertEqual(s.frame, length, "back where the tape was last played to")
        XCTAssertEqual(s.takes().count, 1, "the abandoned attempt is not kept")
        XCTAssertFalse(r.canUndo(now: now, activeTake: s.activeTake, mode: s.mode))
        XCTAssertThrowsError(try r.applyUndo(s, now: now))

        // A retry left alone: the chip goes after 10 s.
        try s.rewind(30)
        try play(r, s, 10, now: &now)
        XCTAssertTrue(r.canUndo(now: now, activeTake: s.activeTake, mode: s.mode))
        XCTAssertFalse(r.canUndo(now: now + RetryTracker.undoSeconds + 0.1, activeTake: s.activeTake, mode: s.mode))
    }

    /// Rapid retries keep only the latest attempt; Undo still returns to the original continuation.
    func testRetryBurstKeepsOneAttempt() throws {
        let s = try newSession()
        let r = RetryTracker()
        var now = 10.0
        try play(r, s, 240, now: &now, button: UInt8(RN_BTN_RIGHT))
        let original = s.activeTake, length = s.takeLength
        for _ in 0..<3 {
            try s.rewind(20)
            try play(r, s, 15, now: &now, button: UInt8(RN_BTN_B))
        }
        XCTAssertLessThanOrEqual(s.takes().count, 2, "the abandoned short attempts are dropped")
        try r.applyUndo(s, now: now)
        XCTAssertEqual(s.activeTake, original)
        XCTAssertEqual(s.takeLength, length)
    }

    func testStatusLabelOnlyForNonDefaultStates() {
        XCTAssertEqual(Tape.statusLabel(mode: RN_MODE_RECORD, rewinding: false, fastForward: false, slow: .normal, endOfTape: false), "")
        XCTAssertFalse(Tape.statusLabel(mode: RN_MODE_REPLAY, rewinding: false, fastForward: false, slow: .normal, endOfTape: false).isEmpty)
        let both = Tape.statusLabel(mode: RN_MODE_REPLAY, rewinding: true, fastForward: false, slow: .half, endOfTape: false)
        XCTAssertEqual(both.components(separatedBy: " · ").count, 3, both)
        XCTAssertFalse(Tape.statusLabel(mode: RN_MODE_RECORD, rewinding: false, fastForward: false, slow: .normal, endOfTape: true).isEmpty)
        XCTAssertEqual(Tape.timeLabel(3606), "1:00")
    }

    func testCoachShowsEachTipOnce() {
        let c = CoachTips(shown: 0)
        XCTAssertEqual(c.tick(now: 0, playing: true, rewindStopped: false, pausedAtEnd: false, takeLength: 0), RNF_COACH_NONE)
        var tip = RNF_COACH_NONE
        var t = 0.0
        while tip == RNF_COACH_NONE && t < 20 {
            t += 0.1
            tip = c.tick(now: t, playing: true, rewindStopped: false, pausedAtEnd: false, takeLength: 600)
        }
        XCTAssertEqual(tip, RNF_COACH_REWIND)
        XCTAssertNotEqual(c.shown & RNF_COACH_REWIND.rawValue, 0)
        XCTAssertTrue(CoachTips.text(tip, rewindControl: "L2").contains("L2"))
        // Persisted: a new run with the bits never shows it again.
        let again = CoachTips(shown: c.shown)
        for i in 0..<200 {
            XCTAssertNotEqual(again.tick(now: Double(i) * 0.1, playing: true, rewindStopped: false, pausedAtEnd: false, takeLength: 600),
                              RNF_COACH_REWIND)
        }
    }

    func testMakeVideoRangesAndPresets() {
        XCTAssertTrue(MakeVideo.frames(.all, cursor: 50, takeLength: 300, section: nil)! == (0, 300))
        XCTAssertTrue(MakeVideo.frames(.toHere, cursor: 50, takeLength: 300, section: nil)! == (0, 50))
        XCTAssertNil(MakeVideo.frames(.toHere, cursor: 0, takeLength: 300, section: nil))
        XCTAssertNil(MakeVideo.frames(.all, cursor: 0, takeLength: 0, section: nil))
        let sec = TimelineRange(slot: 2, a: 30, b: 90)
        XCTAssertTrue(MakeVideo.frames(.section(2), cursor: 0, takeLength: 300, section: sec)! == (30, 90))
        XCTAssertNil(MakeVideo.frames(.section(2), cursor: 0, takeLength: 300, section: TimelineRange(slot: 2, a: 30, b: nil)))
        XCTAssertEqual(MakeVideo.qualityPresets, 3)
        XCTAssertEqual(MakeVideo.quality(0), .standard)
        XCTAssertEqual(MakeVideo.quality(1), .high)
        XCTAssertEqual(MakeVideo.quality(2), .light)
        for p in 0..<MakeVideo.qualityPresets {
            XCTAssertEqual(MakeVideo.preset(for: MakeVideo.quality(p)), p)
            XCTAssertFalse(MakeVideo.qualityLabel(p).isEmpty)
        }
        let summary = MakeVideo.summary(frames: 3606, bytes: 91_000_000)
        XCTAssertTrue(summary.contains("1:00") && summary.contains("91 MB"), summary)
    }
}
