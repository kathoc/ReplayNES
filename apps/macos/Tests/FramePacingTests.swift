// Frame pacing bookkeeping (FramePacing.swift): judder / skipped-frame counting, callback
// regularity.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class FramePacingTests: XCTestCase {
    private let p = FramePacing.period

    func testSteadyPresentsHaveNoJudder() {
        var f = FramePacing()
        for i in 0..<600 { f.present(frame: UInt64(i), at: Double(i) * 2 / 120) }  // every other 120 Hz refresh
        XCTAssertEqual(f.presents, 600)
        XCTAssertEqual(f.hitches, 0)
        XCTAssertEqual(f.skipped, 0)
        XCTAssertEqual(f.takeWindowMax(), 2.0 / 120, accuracy: 1e-9)
        XCTAssertEqual(f.takeWindowMax(), 0)  // reset by the read
    }

    func testHeldAndSkippedFramesAreCounted() {
        var f = FramePacing()
        f.present(frame: 10, at: 1.0)
        f.present(frame: 11, at: 1.0 + 3.0 / 120)   // shown for three refreshes: judder
        f.present(frame: 12, at: 1.0 + 4.0 / 120)   // one refresh
        f.present(frame: 14, at: 1.0 + 6.0 / 120)   // frame 13 never shown
        XCTAssertEqual(f.hitches, 1)
        XCTAssertEqual(f.skipped, 1)
    }

    func testPausesAndSeeksAreNotPacingProblems() {
        var f = FramePacing()
        f.present(frame: 100, at: 1.0)
        f.present(frame: 101, at: 3.0)      // resumed after a pause
        f.present(frame: 500, at: 3.0 + p)  // seek forward
        f.present(frame: 20, at: 3.0 + 2 * p)  // seek back
        XCTAssertEqual(f.hitches, 0)
        XCTAssertEqual(f.skipped, 0)
    }

    func testCallbackRegularity() {
        var r = CallbackRegularity(lateAfter: 1.5 * p)
        var t = 0.0
        for _ in 0..<100 { r.tick(at: t); t += p }
        t += p                // one missed period
        r.tick(at: t)
        t += 5                // stopped (paused): not late
        r.tick(at: t)
        XCTAssertEqual(r.count, 102)
        XCTAssertEqual(r.late, 1)
        XCTAssertEqual(r.takeWindowMax(), 2 * p, accuracy: 1e-9)
    }
}
