// Physical CRT port (Sources/Core/CRT): conformance with nesterm's CPU reference models, setup
// math, determinism, and the CRT MP4 export path.
// SPDX-License-Identifier: GPL-2.0-or-later
import AVFoundation
import XCTest

final class CRTTests: XCTestCase {
    private static var conformance: CRTConformance?
    private var c: CRTConformance! { Self.conformance }

    override class func setUp() {
        super.setUp()
        conformance = CRTConformance()
    }

    override func setUpWithError() throws {
        try XCTSkipIf(MTLCreateSystemDefaultDevice() == nil, "no Metal device")
        XCTAssertNotNil(c, "missing tests/fixtures/crt/reference.json (tools/crt-reference/generate-fixtures.mjs)")
    }

    // Float32 GPU vs float64 CPU reference. Bounds are ~10-100x the observed error on M1 Max.
    func testReceiverMatchesAGCReference() throws {
        let d = try c.receiver(noise: false)
        XCTAssertLessThan(d.maxAbs, 1e-4, "\(d)")
        XCTAssertGreaterThan(d.count, 10_000)
    }

    func testReceiverWithAntennaNoiseMatchesReference() throws {
        let d = try c.receiver(noise: true)
        XCTAssertLessThan(d.maxAbs, 1e-4, "\(d)")
    }

    func testTubeFixedSpotMatchesRenderTube() throws {
        let d = try c.tube(growth: false)
        XCTAssertLessThan(d.maxAbs, 1e-5, "\(d)")
    }

    func testTubeBeamGrowthMatchesGrowthPlan() throws {
        let d = try c.tube(growth: true)
        XCTAssertLessThan(d.maxAbs, 1e-5, "\(d)")
    }

    func testReducedRasterMatchesRowWeights() throws {
        let d = try c.raster160()
        XCTAssertLessThan(d.maxAbs, 1e-5, "\(d)")
    }

    func testSupplyABLMatchesSupplyState() throws {
        let d = try c.supply()
        XCTAssertLessThan(d.maxAbs, 1e-3, "\(d)")   // V ~ 25 kV recurrence in float32
    }

    func testHorizontalSpotMatchesApplySpotH() throws {
        let d = try c.spotH()
        XCTAssertLessThan(d.maxAbs, 1e-5, "\(d)")
    }

    func testPersistenceMatchesFrameFractions() throws {
        let d = try c.persistence()
        XCTAssertLessThan(d.maxAbs, 2e-3, "\(d)")   // half-float ring, as in tube-webgl.mjs
    }

    // Regression: after a seek / rewind / load the persistence history was empty, so a still showed
    // only the first frame's share of each phosphor's light (green/blue ~10% darker: purple tint).
    func testStillAfterSeekMatchesContinuousPlay() throws {
        let still = try c.stillAfterSeek(phases: [UInt32](repeating: 1, count: 12))
        XCTAssertLessThan(still.maxAbs, 3e-3, "same picture held: \(still)")
        // Real play alternates the burst phase every frame (two-frame artifact pattern): the
        // picture differs per pixel there, but its colour must not.
        let alternating = try c.stillAfterSeek(phases: (0..<12).map { UInt32($0 % 2 == 0 ? 2 : 0) })
        XCTAssertLessThan(alternating.meanAbs, 2e-3, "\(alternating)")
    }

    func testSameFrameSequenceIsBitIdentical() throws {
        XCTAssertTrue(try c.deterministic())
    }

    func testOptimizedKernelsAreBitIdenticalToDirectPort() throws {
        XCTAssertTrue(try c.fastPathsMatchDirectPort())
    }

    // Fast path (live display): bounded against the reference on moving pictures. Observed on
    // M1 Max: 60-75 dB; the bound (45 dB, an 8-bit display step is ~48 dB) leaves room for
    // other GPUs' transcendental functions without letting a structural error through.
    func testFastPathMatchesReference() throws {
        for (w, h) in [(320, 240), (640, 480)] {
            let q = try c.fastVsReference(ow: w, oh: h)
            XCTAssertGreaterThan(q.psnr, 45, "\(w)x\(h): \(q)")
            XCTAssertLessThan(q.meanAbs, 2e-3, "\(w)x\(h): \(q)")
        }
    }

    func testFastPathMatchesReferenceWithEffectsOff() throws {
        let q = try c.fastVsReference(CRTConformance.off)
        XCTAssertGreaterThan(q.psnr, 45, "\(q)")
        var s = CRTRenderer.Settings()
        s.lines = 160
        let r = try c.fastVsReference(s)
        XCTAssertGreaterThan(r.psnr, 45, "160 lines: \(r)")
        let g = try c.fastVsReference(rgb: true)
        XCTAssertGreaterThan(g.psnr, 45, "RGB input: \(g)")
    }

    func testFastPathStillAfterSeekMatchesContinuousPlay() throws {
        let still = try c.stillAfterSeek(phases: [UInt32](repeating: 1, count: 12), quality: .fast)
        XCTAssertLessThan(still.maxAbs, 3e-3, "\(still)")
    }

    func testFastPathIsDeterministic() throws {
        XCTAssertTrue(try c.deterministic(quality: .fast))
    }

    func testNoiseHashAndKeyMatchReference() throws {
        guard let meta = c.cases["noiseHash"]?.meta else { return XCTFail("noiseHash fixture") }
        let hashes = (meta["hash"] as? [NSNumber])?.map { $0.uint32Value } ?? []
        XCTAssertEqual([0, 1, 7, 654_719].map { CRTRF.hash32(UInt32($0)) }, hashes)
        XCTAssertEqual(Int(CRTRF.noiseKey(seed: 1, frame: 7)), (meta["key"] as? NSNumber)?.intValue)
    }

    func testSetupModel() {
        // designIF: unity complex carrier gain (validateFilter in rf.mjs).
        let f = CRTRF.filter
        XCTAssertEqual(f.real.reduce(0, +), 1, accuracy: 1e-10)
        XCTAssertEqual(f.imag.reduce(0, +), 0, accuracy: 1e-10)
        XCTAssertEqual(f.delaySamples, 512)
        XCTAssertEqual(CRTRF.blocks, 214)
        // Phosphor frame fractions sum to the energy inside the 0.1 s cutoff (= 1).
        for ch in 0..<3 { XCTAssertEqual(CRTPhosphor.fractions[ch].reduce(0, +), 1, accuracy: 1e-12) }
        XCTAssertEqual(CRTPhosphor.depth, 7)
        // Nestopia burst phase -> carrier phase of row 0 (4*b) and +4 per 2728-sample line.
        XCTAssertEqual(CRTComposite.rowPhases(burstPhase: 2)[0], 8)
        XCTAssertEqual(CRTComposite.rowPhases(burstPhase: 2)[1], 0)
        // 4:3 tube sizes are bounded like nesterm's OUTPUT-RESOLUTION-SPEC.
        XCTAssertEqual(CRTRenderer.tubeSize(forDestination: CGSize(width: 3000, height: 2200), cropFraction: 0, maxWidth: 1600).0, 1600)
        XCTAssertEqual(CRTRenderer.tubeSize(forDestination: CGSize(width: 800, height: 600), cropFraction: 0, maxWidth: 1600).1, 600)
    }

    /// Smoke: a short CRT export of the test ROM writes the expected frame count and differs from
    /// the plain export (the CRT picture really is in the file); the renderer hash is unchanged.
    func testCRTExport() throws {
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-crt-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tmp) }
        let rom = tmp.appendingPathComponent("test.nes")
        try Engine.writeTestROM(to: rom)
        let session = try EngineSession.create(rom: rom, projectDir: nil)
        try session.setMode(RN_MODE_RECORD)
        for f in 0..<30 { _ = try session.step(p1: UInt8(f % 5 == 0 ? RN_BTN_A : 0), p2: 0, events: 0) }
        var hashes: [UInt64] = []
        for crt in [false, true] {
            var s = ExportSettings()
            s.preset = .canvas(640, 480)
            s.crt = crt ? CRTRenderer.Settings() : nil
            let url = tmp.appendingPathComponent(crt ? "crt.mp4" : "plain.mp4")
            let res = try MP4Exporter(renderer: session.makeRenderer(), settings: s, url: url).run(progress: { _, _ in }, isCancelled: { false })
            XCTAssertEqual(res.frames, 30)
            hashes.append(res.rendererHash)
            XCTAssertGreaterThan((try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int) ?? 0, 1000)
        }
        XCTAssertEqual(hashes[0], hashes[1], "CRT is display-only")
        // Keep the CRT file next to the smoke export for inspection (scripts/test-macos.sh).
        if let smoke = ProcessInfo.processInfo.environment["RN_SMOKE_MP4"] {
            let keep = URL(fileURLWithPath: smoke).deletingLastPathComponent().appendingPathComponent("smoke-export-crt.mp4")
            try? FileManager.default.removeItem(at: keep)
            try? FileManager.default.copyItem(at: tmp.appendingPathComponent("crt.mp4"), to: keep)
        }
    }
}
