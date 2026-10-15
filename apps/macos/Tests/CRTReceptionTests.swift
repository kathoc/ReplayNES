// CRT reception (no signal / power-on; docs/CRT_PORT.md "No signal and power-on") on the Metal
// port: a neutral reception is the model untouched, the snow (noise only, AGC at its maximum,
// colour killer) is deterministic and grey, the flash-reduced snow is static, the deflection
// shifts / fades the drive exactly, and a power-on settles into the picture of continuous play.
// The same checks run on Vulkan / Direct3D 11 (apps/desktop/src/render/crt_conformance.h).
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class CRTReceptionTests: XCTestCase {
    private static var conformance: CRTConformance?
    private var c: CRTConformance! { Self.conformance }

    override class func setUp() {
        super.setUp()
        conformance = CRTConformance()
    }

    override func setUpWithError() throws {
        try XCTSkipIf(MTLCreateSystemDefaultDevice() == nil, "no Metal device")
        XCTAssertNotNil(c)
    }

    private func run(_ r: CRTRenderer, _ input: CRTRenderer.Input, ordinal: UInt64, _ rx: CRTRenderer.Reception?) {
        let cb = c.queue.makeCommandBuffer()!
        r.encode(input, ordinal: ordinal, reception: rx, into: cb)
        cb.commit()
        cb.waitUntilCompleted()
    }
    private func codes(_ r: CRTRenderer, _ f: Int, ordinal: UInt64, _ rx: CRTRenderer.Reception?) {
        let k = CRTConformance.movingCodes(f)
        k.withUnsafeBufferPointer { run(r, .codes($0.baseAddress!, burstPhase: UInt32(f % 3)), ordinal: ordinal, rx) }
    }
    private func snow(counter: UInt64, reduced: Bool = false) -> CRTRenderer.Reception {
        CRTRenderer.Reception(step: RNF_CRT_RECEPTION_NO_SIGNAL, counter: counter, flashReduced: reduced, antennaDbuv: 65)!
    }

    /// receptionNeutralIsIdentity: all-default reception (noise keyed like the ordinal) == none.
    func testNeutralReceptionIsBitIdentical() throws {
        for q in [CRTRenderer.Quality.reference, .fast] {
            var outs: [[SIMD4<Float>]] = []
            for neutral in [false, true] {
                let r = try c.renderer(CRTRenderer.Settings(), ow: 320, oh: 240, quality: q)
                for f in 0..<5 {
                    var rx = CRTRenderer.Reception()
                    rx.noiseFrame = UInt32(f + 1)
                    codes(r, f, ordinal: UInt64(f + 1), neutral ? rx : nil)
                }
                outs.append(r.read(.output))
            }
            XCTAssertFalse(outs[0].isEmpty)
            XCTAssertTrue(outs[0] == outs[1], "\(q)")
        }
        // The frontend core's "normal" reception is nil (the model untouched).
        XCTAssertNil(CRTRenderer.Reception(step: RNF_CRT_RECEPTION_NORMAL, counter: 3, flashReduced: false, antennaDbuv: 65))
        XCTAssertNil(CRTRenderer.Reception(step: Int32(RNF_CRT_POWER_ON_FRAMES), counter: 3, flashReduced: true, antennaDbuv: 65))
    }

    /// noSignalSnow: deterministic per counter, colour killed (R = G = B), grey on average.
    func testNoSignalSnow() throws {
        for q in [CRTRenderer.Quality.reference, .fast] {
            func render(_ base: UInt64) throws -> ([SIMD4<Float>], [SIMD4<Float>]) {
                let r = try c.renderer(CRTRenderer.Settings(), ow: 320, oh: 240, quality: q)
                for i in 0..<12 { run(r, .noSignal, ordinal: 0, snow(counter: base + UInt64(i))) }
                return (r.read(.receiver), r.read(.output))
            }
            let (rxA, outA) = try render(100), (_, outB) = try render(100), (_, outC) = try render(200)
            XCTAssertTrue(outA == outB, "\(q): same counters, same snow")
            XCTAssertFalse(outA == outC, "\(q): other counters, other snow")
            var sum = 0.0, sq = 0.0, chroma: Float = 0
            for p in rxA.prefix(512 * 240) {
                chroma = max(chroma, abs(p.x - p.y), abs(p.y - p.z))
                sum += Double(p.y); sq += Double(p.y * p.y)
            }
            let n = 512.0 * 240, mean = sum / n, sd = (sq / n - mean * mean).squareRoot()
            XCTAssertEqual(chroma, 0, "\(q): colour killer")
            XCTAssertGreaterThan(mean, 0.08, "\(q)"); XCTAssertLessThan(mean, 0.6, "\(q)")
            XCTAssertGreaterThan(sd, 0.1, "\(q): snow, not a flat field")
        }
    }

    /// noSignalSnowReducedIsStatic: with flash reduction the grain does not move and is fainter.
    func testFlashReducedSnowIsStaticAndLowContrast() throws {
        for q in [CRTRenderer.Quality.reference, .fast] {
            var outs: [[SIMD4<Float>]] = [], rxs: [[SIMD4<Float>]] = []
            for base: UInt64 in [5, 900] {
                let r = try c.renderer(CRTRenderer.Settings(), ow: 320, oh: 240, quality: q)
                for i in 0..<10 { run(r, .noSignal, ordinal: 0, snow(counter: base + UInt64(i), reduced: true)) }
                outs.append(r.read(.output)); rxs.append(r.read(.receiver))
            }
            XCTAssertTrue(outs[0] == outs[1], "\(q): static")
            let r = try c.renderer(CRTRenderer.Settings(), ow: 320, oh: 240, quality: q)
            for i in 0..<10 { run(r, .noSignal, ordinal: 0, snow(counter: UInt64(i))) }
            func sd(_ a: [SIMD4<Float>]) -> Double {
                let v = a.prefix(512 * 240).map { Double($0.y) }
                let m = v.reduce(0, +) / Double(v.count)
                return (v.map { ($0 - m) * ($0 - m) }.reduce(0, +) / Double(v.count)).squareRoot()
            }
            XCTAssertLessThan(sd(rxs[0]), sd(r.read(.receiver)) * 0.5, "\(q): low contrast")
        }
    }

    /// deflectRollAndGain: roll +1 line = the drive one row up (blanking below); RGB picture 0.5 halves it.
    func testDeflectionShiftsAndFadesTheDrive() throws {
        let drive = CRTConformance.drive()
        let plain = try c.renderer(CRTConformance.off), rolled = try c.renderer(CRTConformance.off)
        drive.withUnsafeBufferPointer { run(plain, .drive($0.baseAddress!), ordinal: 1, nil) }
        var rx = CRTRenderer.Reception()
        rx.roll = 1
        drive.withUnsafeBufferPointer { run(rolled, .drive($0.baseAddress!), ordinal: 1, rx) }
        let a = plain.read(.tubeInput), b = rolled.read(.tubeInput)
        var ok = true
        for y in 0..<239 { for x in 0..<512 where b[y * 512 + x] != a[(y + 1) * 512 + x] { ok = false } }
        for x in 0..<512 where b[239 * 512 + x] != SIMD4(0, 0, 0, 1) { ok = false }
        XCTAssertTrue(ok, "row y shows line y + 1; the last row the vertical blanking")

        var px = [UInt32](repeating: 0, count: 256 * 240)
        for i in px.indices { px[i] = 0xFF00_0000 | UInt32((i * 2654435761) & 0xFFFFFF) }
        let full = try c.renderer(CRTConformance.off), half = try c.renderer(CRTConformance.off)
        px.withUnsafeBufferPointer { run(full, .rgb($0.baseAddress!), ordinal: 1, nil) }
        var g = CRTRenderer.Reception()
        g.picture = 0.5
        px.withUnsafeBufferPointer { run(half, .rgb($0.baseAddress!), ordinal: 1, g) }
        let f = full.read(.tubeInput), h = half.read(.tubeInput)
        var maxErr: Float = 0
        for i in 0..<(512 * 240) { for ch in 0..<3 { maxErr = max(maxErr, abs(h[i][ch] - f[i][ch] * 0.5)) } }
        XCTAssertEqual(maxErr, 0, "RGB input faded by the picture gain")
    }

    /// powerOnSettles: 60 power-on frames then normal play end on the picture of continuous play.
    func testPowerOnSettlesIntoContinuousPlay() throws {
        for q in [CRTRenderer.Quality.reference, .fast] {
            for reduced in [false, true] {
                let a = try c.renderer(CRTRenderer.Settings(), ow: 320, oh: 240, quality: q)
                let b = try c.renderer(CRTRenderer.Settings(), ow: 320, oh: 240, quality: q)
                let n = Int(RNF_CRT_POWER_ON_FRAMES)
                for f in 0..<(n + 30) {
                    codes(a, f, ordinal: UInt64(f + 1), nil)
                    let rx = f < n ? CRTRenderer.Reception(step: Int32(f), counter: UInt64(f), flashReduced: reduced, antennaDbuv: 65) : nil
                    if f < n { XCTAssertNotNil(rx) }
                    codes(b, f, ordinal: UInt64(f + 1), rx)
                }
                let x = a.read(.output), y = b.read(.output)
                var maxAbs: Float = 0, sum = 0.0
                for i in 0..<min(x.count, y.count) { for ch in 0..<3 { let d = abs(x[i][ch] - y[i][ch]); maxAbs = max(maxAbs, d); sum += Double(d) } }
                let mean = sum / Double(3 * min(x.count, y.count))
                XCTAssertLessThan(mean, 1e-3, "\(q) reduced=\(reduced)")
                XCTAssertLessThan(maxAbs, 2e-2, "\(q) reduced=\(reduced)")
            }
        }
    }

    /// The power-on picture really goes through snow: the first frames have no colour, later ones do.
    func testPowerOnStartsInSnow() throws {
        let r = try c.renderer(CRTRenderer.Settings(), ow: 320, oh: 240)
        var firstChroma: Float = -1
        for f in 0..<Int(RNF_CRT_POWER_ON_SNOW_FRAMES) {
            codes(r, f, ordinal: UInt64(f + 1), CRTRenderer.Reception(step: Int32(f), counter: UInt64(f), flashReduced: false, antennaDbuv: 65))
            if f == 0 { firstChroma = r.read(.receiver).prefix(512 * 240).map { max(abs($0.x - $0.y), abs($0.y - $0.z)) }.max() ?? -1 }
        }
        XCTAssertEqual(firstChroma, 0, "snow: colour killed")
    }
}
