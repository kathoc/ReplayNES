// Frame pacing bookkeeping (FramePacing.swift): judder / skipped-frame counting, callback
// regularity and the adaptive present lead.
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

    private func window(_ lead: inout PresentLead, misses: Int) {
        for i in 0..<PresentLead.window { lead.observe(interval: i < misses ? 3.0 / 120 : 2.0 / 120) }
    }

    func testPresentLeadGrowsOnMissesWithinBounds() {
        var l = PresentLead()
        XCTAssertEqual(l.lead, PresentLead.initial)
        window(&l, misses: 1)                       // a stray miss: unchanged
        XCTAssertEqual(l.lead, PresentLead.initial)
        window(&l, misses: 3)
        XCTAssertEqual(l.lead, PresentLead.initial + 0.002, accuracy: 1e-9)
        for _ in 0..<20 { window(&l, misses: 10) }
        XCTAssertEqual(l.lead, PresentLead.maxLead, accuracy: 1e-9)
    }

    func testPresentLeadShrinksSlowlyWhenClean() {
        var l = PresentLead()
        window(&l, misses: 3)                       // grow: then held for a while
        let grown = l.lead
        for _ in 0..<PresentLead.holdAfterGrow { window(&l, misses: 0) }
        XCTAssertEqual(l.lead, grown, accuracy: 1e-9)
        for _ in 0..<PresentLead.shrinkAfter { window(&l, misses: 0) }
        XCTAssertEqual(l.lead, grown - 0.001, accuracy: 1e-9)
        for _ in 0..<1000 { window(&l, misses: 0) }
        XCTAssertEqual(l.lead, PresentLead.minLead, accuracy: 1e-9)
    }
}
