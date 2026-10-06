// Display-locked pacing (DisplayPacing.swift) and the DRC resampler (AudioResampler.swift).
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class DisplayPacingTests: XCTestCase {
    private let p = FramePacing.period

    private func run(_ c: inout DisplayCadence, refresh: Double, count: Int, start: Double = 1000, drop: Set<Int> = []) -> [Int] {
        var frames: [Int] = []
        for i in 0..<count where !drop.contains(i) {
            if c.refresh(presentation: start + Double(i) * refresh) { frames.append(i) }
        }
        return frames
    }

    func testLockedRefreshMultiples() {
        XCTAssertEqual(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 60), 1)
        XCTAssertEqual(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 120), 2)
        XCTAssertEqual(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 240), 4)
        XCTAssertEqual(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 59.94), 1)
        XCTAssertNil(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 144))
        XCTAssertNil(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 75))
        XCTAssertNil(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 50))
    }

    func testEvery120HzRefreshPairIsOneFrame() {
        var c = DisplayCadence()
        let f = run(&c, refresh: 1.0 / 120, count: 1200)
        XCTAssertEqual(f.count, 600)
        // Strictly every other refresh: each frame is on screen for exactly two refreshes.
        for (a, b) in zip(f, f.dropFirst()) { XCTAssertEqual(b - a, 2) }
        XCTAssertEqual(c.emulationRate, 60, accuracy: 1e-6)
        XCTAssertEqual(c.expectedFrameInterval, 2.0 / 120, accuracy: 1e-9)
    }

    func testEvery60HzRefreshIsOneFrame() {
        var c = DisplayCadence()
        let f = run(&c, refresh: 1.0 / 60, count: 600)
        XCTAssertEqual(f, Array(0..<600))
    }

    func testMissedCallbackDoesNotLoseAFrame() {
        var c = DisplayCadence()
        // Refresh 102 (a frame's start) has no callback: the frame starts at the next one, then the
        // cadence continues every two refreshes from there.
        let f = run(&c, refresh: 1.0 / 120, count: 200, drop: [102])
        XCTAssertTrue(f.contains(100))
        XCTAssertTrue(f.contains(103))
        XCTAssertFalse(f.contains(104))
        XCTAssertTrue(f.contains(105))
        XCTAssertEqual(f.count, 100)
    }

    func testTimestampNoiseKeepsTheCadence() {
        var c = DisplayCadence()
        var frames = 0
        var rng = SystemRandomNumberGenerator()
        for i in 0..<2400 {
            let jitter = Double(Int.random(in: -200...200, using: &rng)) * 1e-6   // +-0.2 ms
            if c.refresh(presentation: 50 + Double(i) / 120 + jitter) { frames += 1 }
        }
        XCTAssertEqual(frames, 1200)
    }

    func testBogusTimestampsDoNotBreakTheEstimate() {
        // Seen on a composited full-screen window: updates whose presentation times are almost
        // equal. They must not become the refresh estimate (that once locked onto "k = 71 million").
        var c = DisplayCadence()
        var frames = 0
        for i in 0..<1200 {
            let t = 10 + Double(i) / 120
            if c.refresh(presentation: t) { frames += 1 }
            if i % 50 == 7 && c.refresh(presentation: t + 1e-6) { frames += 1 }
        }
        XCTAssertEqual(c.refresh, 1.0 / 120, accuracy: 1e-6)
        XCTAssertEqual(c.refreshesPerFrame, 2)
        XCTAssertEqual(frames, 600)
    }

    func testOddDeltaDoesNotStickTheEstimate() {
        // One short delta (seen in a window and around display changes: estimates of 157, 241 and
        // 316 Hz on a 120 Hz display) must not become a lasting refresh estimate.
        var c = DisplayCadence()
        var frames = 0
        for i in 0..<1200 {
            let t = 10 + Double(i) / 120
            if c.refresh(presentation: t) { frames += 1 }
            if i == 100 && c.refresh(presentation: t + 0.0035) { frames += 1 }
        }
        XCTAssertEqual(c.refresh, 1.0 / 120, accuracy: 1e-5)
        XCTAssertEqual(c.refreshesPerFrame, 2)
        XCTAssertEqual(frames, 600, accuracy: 2)
    }

    func testDisplayChangeIsFollowed() {
        var c = DisplayCadence()
        for i in 0..<240 { _ = c.refresh(presentation: 10 + Double(i) / 60) }
        XCTAssertEqual(c.refreshesPerFrame, 1)
        for i in 1...240 { _ = c.refresh(presentation: 14 + Double(i) / 120) }
        XCTAssertEqual(c.refresh, 1.0 / 120, accuracy: 1e-5)
        XCTAssertEqual(c.refreshesPerFrame, 2)
    }

    func testFreeCadenceKeepsTheNTSCRate() {
        var c = DisplayCadence()
        let f = run(&c, refresh: 1.0 / 144, count: 144 * 60)   // one minute at 144 Hz
        XCTAssertEqual(Double(f.count), 60 / p, accuracy: 2)
        XCTAssertNil(c.refreshesPerFrame)
        XCTAssertEqual(c.emulationRate, 1 / p, accuracy: 1e-9)
    }

    func testInputDeadlineFollowsWorkAndMisses() {
        var d = InputDeadline()
        XCTAssertEqual(d.lead, InputDeadline.minLead, accuracy: 1e-12)
        for _ in 0..<600 { d.observeWork(0.00105) }
        XCTAssertEqual(d.workQuantile, 0.0011, accuracy: 1e-9)
        XCTAssertEqual(d.lead, 0.0011 + InputDeadline.margin, accuracy: 1e-9)
        d.observeWork(0.050)   // a single stall (seek, autosave) does not move the lead
        XCTAssertEqual(d.lead, 0.0011 + InputDeadline.margin, accuracy: 1e-9)
        for _ in 0..<5 { d.observeWork(0.00405) }   // a run of heavier frames does
        XCTAssertEqual(d.workQuantile, 0.0041, accuracy: 1e-9)
        for _ in 0..<600 { d.observeWork(0.00105) }   // and it relaxes once they leave the window
        XCTAssertEqual(d.workQuantile, 0.0011, accuracy: 1e-9)
        d.observeMiss()
        XCTAssertEqual(d.lead, 0.0011 + InputDeadline.margin + InputDeadline.missPenalty, accuracy: 1e-9)
        for _ in 0..<20 { d.observeMiss() }
        XCTAssertEqual(d.penalty, InputDeadline.maxPenalty, accuracy: 1e-12)
        for _ in 0..<(InputDeadline.decayAfter * 30) { d.observeWork(0.00105) }
        XCTAssertEqual(d.penalty, 0, accuracy: 1e-12)
        for _ in 0..<600 { d.observeWork(0.1) }
        XCTAssertEqual(d.lead, InputDeadline.maxLead)
    }

    func testAudioRateControlConverges() {
        // Simulated ring: producer emulates at 60.000 Hz (display-locked), 798.7 samples per frame
        // at the NES rate; the consumer drains 48000 samples per second. Without DRC the level would
        // fall by ~79 samples per second (underruns within ~20 s).
        var drc = AudioRateControl(targetFill: 1400)
        drc.setFrameRate(60)
        XCTAssertEqual(drc.base, (1 / p) / 60, accuracy: 1e-12)
        var fill = 1400.0
        var minFill = fill, maxFill = fill
        let perFrame = 48000 * p
        for frame in 0..<(60 * 600) {   // ten minutes
            let r = drc.update(fill: fill)
            XCTAssertLessThanOrEqual(abs(r / drc.base - 1), AudioRateControl.maxDeviation + 1e-12)
            fill += perFrame * r
            fill -= 48000.0 / 60
            if frame > 600 { minFill = min(minFill, fill); maxFill = max(maxFill, fill) }
        }
        XCTAssertEqual(minFill, 1400, accuracy: 50)
        XCTAssertEqual(maxFill, 1400, accuracy: 50)
        // A device clock 300 ppm off is absorbed by the proportional term.
        var fill2 = 600.0
        var drc2 = AudioRateControl(targetFill: 1400)
        drc2.setFrameRate(60)
        for _ in 0..<(60 * 600) {
            fill2 += perFrame * drc2.update(fill: fill2)
            fill2 -= 48000.0 * 1.0003 / 60
        }
        XCTAssertEqual(fill2, 1400, accuracy: 200)
    }

    func testResamplerUnityIsTransparent() {
        var r = AudioResampler()
        let input: [Int16] = (0..<2000).map { Int16(truncatingIfNeeded: ($0 * 37) % 20000 - 10000) }
        var out: [Int16] = []
        for chunk in stride(from: 0, to: input.count, by: 800) {
            let part = Array(input[chunk..<min(input.count, chunk + 800)])
            part.withUnsafeBufferPointer { r.process($0, ratio: 1, into: &out) }
        }
        // One sample of delay (the first sample is repeated once; the last one is still held back
        // as look-ahead), otherwise identical.
        XCTAssertEqual(out.count, input.count - 1)
        XCTAssertEqual(out[0], input[0])
        let mismatch = (1..<out.count).first { out[$0] != input[$0 - 1] }
        XCTAssertNil(mismatch, "first difference at output sample \(mismatch ?? -1)")
    }

    func testResamplerRatioAndContinuity() {
        var r = AudioResampler()
        // 1 kHz sine at 48 kHz, 800-sample chunks, ratio 1.0016 (60.0988 -> 60 Hz).
        let ratio = 1.0016
        var phase = 0.0
        var out: [Int16] = []
        var inCount = 0
        for _ in 0..<600 {
            var chunk = [Int16](repeating: 0, count: 799)
            for i in chunk.indices { chunk[i] = Int16(20000 * sin(phase)); phase += 2 * .pi * 1000 / 48000 }
            inCount += chunk.count
            chunk.withUnsafeBufferPointer { r.process($0, ratio: ratio, into: &out) }
        }
        XCTAssertEqual(Double(out.count), Double(inCount) * ratio, accuracy: 3)
        // Still a clean sine (slightly lower pitch): no jumps at chunk boundaries.
        let maxStep = 20000 * 2 * Double.pi * 1000 / 48000 / ratio
        for (a, b) in zip(out, out.dropFirst()) {
            XCTAssertLessThanOrEqual(abs(Double(b) - Double(a)), maxStep * 1.02 + 2)
        }
    }

    // MARK: BuildAhead / BacklogDrain

    func testBuildAheadOnlyForHeavyPicturesOnTheDirectPath() {
        let direct = 1.0 / 120, composited = 2.0 / 120
        var b = BuildAhead()
        for _ in 0..<120 { b.add(0.0001) }        // plain picture: ~0.1 ms of GPU
        b.update(presentDelay: direct)
        XCTAssertFalse(b.active)
        var crt = BuildAhead()
        for i in 0..<29 { crt.add(0.008); crt.update(presentDelay: direct); XCTAssertFalse(crt.active, "decided after \(i + 1) samples") }
        crt.add(0.008)
        crt.update(presentDelay: direct)          // CRT at 1080p: ~8 ms > 8.33 - 2.5 ms
        XCTAssertTrue(crt.active)
        var window = BuildAhead()
        for _ in 0..<120 { window.add(0.008) }
        window.update(presentDelay: composited)   // composited (window): 8 ms fits in 16.7 - 2.5 ms
        XCTAssertFalse(window.active)
        // One composited update among direct ones counts (largest recent delay).
        var mixed = BuildAhead()
        for _ in 0..<120 { mixed.add(0.008) }
        mixed.update(presentDelay: composited)
        for _ in 0..<(BuildAhead.delayWindow - 1) { mixed.update(presentDelay: direct) }
        XCTAssertFalse(mixed.active)
        mixed.update(presentDelay: direct)        // the composited one left the window
        XCTAssertTrue(mixed.active)
    }

    func testBuildAheadHysteresis() {
        let direct = 1.0 / 120
        var b = BuildAhead()
        for _ in 0..<120 { b.add(0.008) }
        b.update(presentDelay: direct)
        XCTAssertTrue(b.active)
        for _ in 0..<120 { b.add(0.005) }         // between 8.33 - 4 and 8.33 - 2.5 ms: stays
        b.update(presentDelay: direct)
        XCTAssertTrue(b.active)
        for _ in 0..<120 { b.add(0.003) }         // clearly below: back in the same refresh
        b.update(presentDelay: direct)
        XCTAssertFalse(b.active)
        for _ in 0..<120 { b.add(0.008) }
        b.update(presentDelay: direct)
        b.reset()                                 // CRT off
        XCTAssertFalse(b.active)
        XCTAssertNil(b.p90)
    }

    func testBacklogDrainPolicy() {
        // Direct to a fixed-refresh display: drain as soon as two frames' presents were late.
        XCTAssertEqual(BacklogDrain.policy(variableRefresh: false, direct: true), BacklogDrain.fast)
        XCTAssertEqual(BacklogDrain.fast, BacklogDrain.Policy(lateRun: 4, minInterval: 0.25))
        // Composited (a busy window server is also late) or variable refresh: rare.
        XCTAssertEqual(BacklogDrain.policy(variableRefresh: false, direct: false), BacklogDrain.rare)
        XCTAssertEqual(BacklogDrain.policy(variableRefresh: true, direct: true), BacklogDrain.rare)
        XCTAssertEqual(BacklogDrain.rare, BacklogDrain.Policy(lateRun: 60, minInterval: 5))
    }

    func testPresentPathNeedsEveryRecentUpdateDirect() {
        let r = 1.0 / 120
        var p = PresentPath()
        XCTAssertFalse(p.isDirect(refresh: r))                 // nothing seen yet
        for _ in 0..<PresentPath.window { p.observe(presentDelay: r) }
        XCTAssertTrue(p.isDirect(refresh: r))
        p.observe(presentDelay: 2 * r)                         // one composited prediction
        XCTAssertFalse(p.isDirect(refresh: r))
        for _ in 0..<(PresentPath.window - 1) { p.observe(presentDelay: r) }
        XCTAssertFalse(p.isDirect(refresh: r))
        p.observe(presentDelay: r)                             // it left the window
        XCTAssertTrue(p.isDirect(refresh: r))
    }
}
