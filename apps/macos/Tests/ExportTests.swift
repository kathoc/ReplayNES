// Headless tests for the UI-free parts of the macOS frontend (exporter, geometry, audio ring).
// SPDX-License-Identifier: GPL-2.0-or-later
import AVFoundation
import XCTest

final class ExportTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tmp)
    }

    /// Records a short take on the generated test ROM (P1 inputs + a soft reset).
    private func recordTake(frames: Int) throws -> EngineSession {
        let rom = tmp.appendingPathComponent("test.nes")
        try Engine.writeTestROM(to: rom)
        let s = try EngineSession.create(rom: rom, projectDir: nil)
        try s.setMode(RN_MODE_RECORD)
        for f in 0..<frames {
            var p1: UInt8 = 0
            if f % 40 < 10 { p1 |= UInt8(RN_BTN_A) }
            if f % 25 > 15 { p1 |= UInt8(RN_BTN_RIGHT) }
            if f == 30 { p1 |= UInt8(RN_BTN_START) }
            let ev: UInt8 = f == frames / 2 ? UInt8(RN_EV_SOFT_RESET) : 0
            try s.step(p1: p1, p2: UInt8(f & 0x0F), events: ev)
        }
        return s
    }

    /// Synthetic pixel-art picture for rate-control checks (no ROM needed): flat 8x8 cells in a
    /// 16-colour palette, optionally scrolling 2 px per frame; a small sprite moves every 2 s.
    static func syntheticPatch(scroll: Bool) -> (UInt64, inout [UInt32]) -> Void {
        return { frame, px in
            let w = Int(RN_VIDEO_WIDTH), h = Int(RN_VIDEO_HEIGHT)
            let shift = scroll ? Int(frame) * 2 : 0
            for y in 0..<h {
                for x in 0..<w {
                    let cx = (x + shift) / 8, cy = y / 8
                    var hh = UInt32(truncatingIfNeeded: cx) &* 2654435761 ^ UInt32(truncatingIfNeeded: cy) &* 40503
                    hh ^= hh >> 13
                    let pal: UInt32 = cy > 22 ? 3 + UInt32((cx / 2) & 1) : (hh % 9 < 6 ? 0 : 1 + hh % 12)
                    let v = 0x20 + pal * 14
                    px[y * w + x] = 0xFF00_0000 | ((v * 3 / 2) & 0xFF) << 16 | ((v * 2 + 40 * (pal & 1)) % 256) << 8 | ((255 - v) & 0xFF)
                }
            }
            let sx = Int((frame / 120) % 8) * 32 + 24, sy = 96
            for y in sy..<min(h, sy + 16) { for x in sx..<min(w, sx + 16) { px[y * w + x] = 0xFFF0_E040 } }
        }
    }

    /// Exports `frames` frames and returns (predicted, actual) bytes.
    private func exportedSize(_ s: EngineSession, settings: ExportSettings, patch: ((UInt64, inout [UInt32]) -> Void)?, name: String) throws -> (predicted: Int64, actual: Int64, seconds: Double) {
        let out = tmp.appendingPathComponent(name)
        let t0 = Date()
        let exporter = MP4Exporter(renderer: try s.makeRenderer(), settings: settings, url: out)
        exporter.picturePatch = patch
        let result = try exporter.run(progress: { _, _ in }, isCancelled: { false })
        let actual = (try FileManager.default.attributesOfItem(atPath: out.path)[.size] as? Int64) ?? 0
        let predicted = ExportGeometry(settings).predictedBytes(hevc: settings.codec == .hevc, quality: settings.quality, frames: result.frames)
        if let keep = ProcessInfo.processInfo.environment["RN_PREDICT_KEEP"] {   // inspect the files with ffprobe
            try? FileManager.default.createDirectory(atPath: keep, withIntermediateDirectories: true)
            let dst = URL(fileURLWithPath: keep).appendingPathComponent("\(name)-\(settings.codec.rawValue)-q\(settings.quality.rawValue)-\(result.frames).mp4")
            try? FileManager.default.removeItem(at: dst)
            try? FileManager.default.copyItem(at: out, to: dst)
        }
        try? FileManager.default.removeItem(at: out)
        return (predicted, actual, Date().timeIntervalSince(t0))
    }

    /// The Export sheet's size estimate against the real file (the encoder uses an average bit rate
    /// capped at 1.5x, so simple pictures give smaller files). Only measured and printed (PREDICT
    /// lines): the estimate is no promise, a size mismatch never fails. Short default run;
    /// RN_PREDICT_FRAMES (e.g. 18000) turns it into a long measurement of the whole matrix
    /// (content x quality x codec), printed as PREDICT lines.
    func testPredictedSizeTracksTarget() throws {
        let env = ProcessInfo.processInfo.environment
        let frames = Int(env["RN_PREDICT_FRAMES"] ?? "") ?? 600
        let long = env["RN_PREDICT_FRAMES"] != nil
        let s = try recordTake(frames: frames)
        var settings = ExportSettings()
        settings.preset = .canvas(1920, 1080)
        // Default run: flat pixel art at Standard in both codecs (the rate limits are accepted by the
        // H.264 and HEVC encoders) and the test ROM's noise at High.
        typealias Patch = (UInt64, inout [UInt32]) -> Void
        let cases: [(String, Patch?, ExportSettings.Quality, ExportSettings.Codec)]
        if long {
            var all: [(String, Patch?, ExportSettings.Quality, ExportSettings.Codec)] = []
            for (n, patch) in [("noise", nil), ("still", Self.syntheticPatch(scroll: false)), ("scroll", Self.syntheticPatch(scroll: true))] as [(String, Patch?)] {
                for q in ExportSettings.Quality.allCases { for c in ExportSettings.Codec.allCases { all.append((n, patch, q, c)) } }
            }
            cases = all
        } else {
            cases = [("still", Self.syntheticPatch(scroll: false), .standard, .h264), ("still", Self.syntheticPatch(scroll: false), .standard, .hevc),
                     ("noise", nil, .high, .h264)]
        }
        // RN_PREDICT_ONLY="still:1:h264,noise:2:hevc" (content:quality:codec) picks cases of the matrix.
        let only = env["RN_PREDICT_ONLY"]?.split(separator: ",").map(String.init)
        for (cname, patch, q, codec) in cases {
            if let only, !only.contains("\(cname):\(q.rawValue):\(codec.rawValue)") { continue }
            settings.quality = q
            settings.codec = codec
            let r = try exportedSize(s, settings: settings, patch: patch, name: cname)
            let err = (Double(r.actual) - Double(r.predicted)) * 100 / Double(r.predicted)
            print("PREDICT frames=\(frames) \(cname) \(codec.rawValue) q=\(q.rawValue) predicted=\(r.predicted) actual=\(r.actual) error=\(String(format: "%.2f", err))% time=\(String(format: "%.1f", r.seconds))s")
        }
    }


    func testExportShortTakeH264() throws {
        let frames = 180
        let s = try recordTake(frames: frames)
        let lengthBefore = s.takeLength, frameBefore = s.frame, takeBefore = s.activeTake
        let hashBefore = s.stateHash()

        // Expected hash from a plain renderer run (no encoding) for comparison.
        let ref = try s.makeRenderer()
        var v: UnsafePointer<UInt32>?, a: UnsafePointer<Int16>?, n = 0, f: UInt64 = 0
        while rn_renderer_next(ref, &v, &a, &n, &f) == RN_OK {}
        let refHash = rn_renderer_hash(ref)
        rn_renderer_free(ref)

        var settings = ExportSettings()
        settings.preset = .canvas(1280, 960)
        settings.pixelAspect87 = true
        let envOut = ProcessInfo.processInfo.environment["RN_SMOKE_MP4"]
        let out = envOut.map { URL(fileURLWithPath: $0) } ?? tmp.appendingPathComponent("smoke.mp4")
        let exporter = MP4Exporter(renderer: try s.makeRenderer(), settings: settings, url: out)
        var lastProgress: (UInt64, UInt64) = (0, 0)
        let result = try exporter.run(progress: { lastProgress = ($0, $1) }, isCancelled: { false })

        XCTAssertEqual(result.frames, UInt64(frames))
        XCTAssertEqual(lastProgress.0, UInt64(frames))
        XCTAssertEqual(lastProgress.1, UInt64(frames))
        XCTAssertEqual(result.audioSamples, rn_audio_samples_before(UInt64(frames)))
        XCTAssertEqual(result.rendererHash, refHash, "export run must match a normal offline replay")
        XCTAssertFalse(FileManager.default.fileExists(atPath: out.path + ".part"), "no temporary file left")
        XCTAssertTrue(result.encoder.hasPrefix("VideoToolbox H.264"))
        print("export encoder: \(result.encoder)")

        // Export must not mutate the project/session.
        XCTAssertEqual(s.takeLength, lengthBefore)
        XCTAssertEqual(s.frame, frameBefore)
        XCTAssertEqual(s.activeTake, takeBefore)
        XCTAssertEqual(s.stateHash(), hashBefore)

        let asset = AVURLAsset(url: out)
        let expected = Double(frames) * Double(RN_FPS_DEN) / Double(RN_FPS_NUM)
        let exp = expectation(description: "load")
        Task {
            let duration = try await asset.load(.duration).seconds
            let vt = try await asset.loadTracks(withMediaType: .video)
            let at = try await asset.loadTracks(withMediaType: .audio)
            XCTAssertEqual(vt.count, 1)
            XCTAssertEqual(at.count, 1)
            let size = try await vt[0].load(.naturalSize)
            XCTAssertEqual(size, CGSize(width: 1280, height: 960))
            let vdur = try await vt[0].load(.timeRange).duration.seconds
            XCTAssertEqual(vdur, expected, accuracy: 0.001, "video duration comes from frame count")
            XCTAssertEqual(duration, expected, accuracy: 0.05)
            let nominal = try await vt[0].load(.nominalFrameRate)
            XCTAssertEqual(Double(nominal), 60.0988, accuracy: 0.05)
            exp.fulfill()
        }
        wait(for: [exp], timeout: 30)
    }

    /// Opt-in long-take check (RN_LONG_EXPORT_PROJECT = a .nesrec, RN_LONG_EXPORT_ROM = its ROM,
    /// RN_LONG_EXPORT_OUT = the .mp4 to write; via TEST_RUNNER_ variables): the whole take at
    /// 1920x1080, 8:7, flash reduction Standard - a file of several GB and over 109 s (the exact
    /// video timescale then needs 64-bit durations). AVFoundation must load it with the exact
    /// duration; the box layout (co64 past 4 GiB) is checked outside with ffprobe / mp4 tools.
    func testLongExportFromProject() throws {
        let env = ProcessInfo.processInfo.environment
        guard let project = env["RN_LONG_EXPORT_PROJECT"], let out = env["RN_LONG_EXPORT_OUT"] else {
            throw XCTSkip("RN_LONG_EXPORT_PROJECT / RN_LONG_EXPORT_OUT not set")
        }
        let s = try EngineSession.open(projectDir: URL(fileURLWithPath: project),
                                       romOverride: env["RN_LONG_EXPORT_ROM"].map { URL(fileURLWithPath: $0) },
                                       dropCorruptStates: false)
        var settings = ExportSettings()
        settings.preset = .canvas(1920, 1080)
        settings.pixelAspect87 = true
        settings.flashReduction = .standard
        let url = URL(fileURLWithPath: out)
        let t0 = Date()
        let result = try MP4Exporter(renderer: try s.makeRenderer(), settings: settings, url: url)
            .run(progress: { _, _ in }, isCancelled: { false })
        let seconds = Date().timeIntervalSince(t0)
        print(String(format: "long export: %llu frames in %.1f s (%.1f fps), %@", result.frames, seconds,
                     Double(result.frames) / seconds, result.encoder))
        XCTAssertEqual(result.frames, s.takeLength)
        let expected = Double(result.frames) * Double(RN_FPS_DEN) / Double(RN_FPS_NUM)
        let exp = expectation(description: "load")
        Task {
            let asset = AVURLAsset(url: url)
            let vt = try await asset.loadTracks(withMediaType: .video)
            XCTAssertEqual(vt.count, 1)
            let vdur = try await vt[0].load(.timeRange).duration.seconds
            XCTAssertEqual(vdur, expected, accuracy: 0.001)
            exp.fulfill()
        }
        wait(for: [exp], timeout: 120)
    }

    func testExportHEVCRangeAndCancel() throws {
        let s = try recordTake(frames: 120)
        var settings = ExportSettings()
        settings.codec = .hevc
        settings.preset = .native(2)
        settings.startFrame = 30
        settings.endFrame = 90
        let out = tmp.appendingPathComponent("range.mp4")
        let ex = MP4Exporter(renderer: try s.makeRenderer(start: 30, end: 90), settings: settings, url: out)
        let r = try ex.run(progress: { _, _ in }, isCancelled: { false })
        XCTAssertEqual(r.frames, 60)
        XCTAssertEqual(r.audioSamples, rn_audio_samples_before(90) - rn_audio_samples_before(30))

        let out2 = tmp.appendingPathComponent("cancel.mp4")
        let ex2 = MP4Exporter(renderer: try s.makeRenderer(), settings: ExportSettings(), url: out2)
        var calls = 0
        XCTAssertThrowsError(try ex2.run(progress: { _, _ in calls += 1 }, isCancelled: { calls >= 10 })) { e in
            guard case ExportError.cancelled = e else { return XCTFail("expected cancel, got \(e)") }
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: out2.path), "cancelled export leaves no file")
    }

    func testAudioRing() {
        let r = rn_ring_create(4096, 800, 3200)!
        defer { rn_ring_destroy(r) }
        var out = [Float](repeating: 1, count: 512)
        rn_ring_set_muted(r, 0)
        // Not primed yet: silence, no underrun.
        rn_ring_pull(r, &out, 512)
        var st = rn_audio_ring_stats()
        rn_ring_get_stats(r, &st)
        XCTAssertEqual(st.underruns, 0)
        let pcm = [Int16](repeating: 16384, count: 800)
        XCTAssertEqual(rn_ring_push(r, pcm, 800), 800)
        rn_ring_pull(r, &out, 512)
        XCTAssertEqual(out[0], 0.5, accuracy: 1e-6)
        rn_ring_pull(r, &out, 512) // only 288 left -> underrun
        rn_ring_get_stats(r, &st)
        XCTAssertEqual(st.underruns, 1)
        XCTAssertEqual(out[300], 0)
        // Mute flushes.
        _ = rn_ring_push(r, pcm, 800)
        rn_ring_set_muted(r, 1)
        rn_ring_pull(r, &out, 512)
        rn_ring_get_stats(r, &st)
        XCTAssertEqual(st.fill, 0)
        XCTAssertEqual(st.underruns, 1, "muted output never counts as underrun")
    }

    func testDefaultInputConfigLoads() throws {
        let inp = rn_input_new()!
        defer { rn_input_free(inp) }
        try rnCheck(rn_input_load_json(inp, InputCatalog.defaultConfigJSON()))
        rn_input_set_pressed(inp, "kb:7", 1) // X -> A
        var p1: UInt8 = 0, p2: UInt8 = 0
        rn_input_sample_game(inp, 0, &p1, &p2)
        XCTAssertEqual(p1, UInt8(RN_BTN_A))
        let saved = rn_input_save_json(inp)!
        defer { rn_string_free(saved) }
        let cfg = InputCatalog.parse(String(cString: saved))
        XCTAssertEqual(cfg?.turboPeriod, 4)
        XCTAssertEqual(Set(cfg?.inputs(for: "p1.a") ?? []), ["gc0:face.east", "kb:7"])
    }

    /// The documented default play keys/buttons reach P1 (Return/Menu = START etc.).
    func testDefaultBindingsDrivePlayer1() throws {
        let inp = rn_input_new()!
        defer { rn_input_free(inp) }
        try rnCheck(rn_input_load_json(inp, InputCatalog.defaultConfigJSON()))
        let cases: [(String, Int32)] = [
            ("kb:36", RN_BTN_START), ("kb:126", RN_BTN_UP), ("kb:125", RN_BTN_DOWN), ("kb:123", RN_BTN_LEFT),
            ("kb:124", RN_BTN_RIGHT), ("kb:7", RN_BTN_A), ("kb:6", RN_BTN_B), ("kb:60", RN_BTN_SELECT),
            ("gc0:menu", RN_BTN_START), ("gc0:options", RN_BTN_SELECT), ("gc0:face.east", RN_BTN_A), ("gc0:face.south", RN_BTN_B),
            ("gc0:dpad.up", RN_BTN_UP), ("gc0:dpad.right", RN_BTN_RIGHT),
        ]
        for (i, (id, bit)) in cases.enumerated() {
            rn_input_set_pressed(inp, id, 1)
            var p1: UInt8 = 0, p2: UInt8 = 0
            rn_input_sample_game(inp, UInt64(i * 2), &p1, &p2)
            XCTAssertEqual(p1, UInt8(bit), id)
            XCTAssertEqual(p2, 0, id)
            rn_input_set_pressed(inp, id, 0)
            rn_input_sample_game(inp, UInt64(i * 2 + 1), &p1, &p2)
        }
        rn_input_set_axis(inp, "gc0:lstick", 0, 1)
        var p1: UInt8 = 0, p2: UInt8 = 0
        rn_input_sample_game(inp, 100, &p1, &p2)
        XCTAssertEqual(p1, UInt8(RN_BTN_UP), "left stick up")
    }
}
