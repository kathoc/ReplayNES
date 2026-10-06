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
        for _ in 0..<100 { d.observeWork(0.001) }
        XCTAssertEqual(d.lead, 0.001 + InputDeadline.margin, accuracy: 1e-9)
        d.observeWork(0.004)   // one slow frame raises the lead at once
        XCTAssertEqual(d.lead, 0.004 + InputDeadline.margin, accuracy: 1e-9)
        for _ in 0..<5000 { d.observeWork(0.001) }   // and it relaxes
        XCTAssertEqual(d.lead, 0.001 + InputDeadline.margin, accuracy: 1e-6)
        d.observeMiss()
        XCTAssertEqual(d.lead, 0.001 + InputDeadline.margin + InputDeadline.missPenalty, accuracy: 1e-6)
        for _ in 0..<20 { d.observeMiss() }
        XCTAssertEqual(d.penalty, InputDeadline.maxPenalty, accuracy: 1e-12)
        for _ in 0..<(InputDeadline.decayAfter * 30) { d.observeWork(0.001) }
        XCTAssertEqual(d.penalty, 0, accuracy: 1e-12)
        for _ in 0..<10 { d.observeWork(0.1) }
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
}
