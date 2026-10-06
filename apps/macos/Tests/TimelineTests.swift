// Swift glue of the filmstrip timeline: the thumbnail cache holding CGImages, the downscaler to
// CGImage and rn_practice_set_range / visible ranges through the Swift wrappers. The timeline and
// grid math is tested in tests/test_frontend_timeline.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import XCTest

final class TimelineTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-timeline-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws { try? FileManager.default.removeItem(at: tmp) }

    private func image(_ color: UInt32 = 0xFF11_2233) -> CGImage {
        ThumbnailScaler.image([UInt32](repeating: color, count: ThumbnailScaler.width * ThumbnailScaler.height))!
    }

    // MARK: thumbnails (CGImage payloads in the core's cache)

    func testThumbnailCacheInvalidation() {
        let c = ThumbnailCache()
        c.reset(take: 1)
        c.setStep(10)
        XCTAssertTrue(c.wants(frame: 10, take: 1))
        XCTAssertTrue(c.wants(frame: ThumbnailGrid.startPicture(step: 10), take: 1), "tile 0's picture")
        XCTAssertFalse(c.wants(frame: 15, take: 1), "off grid")
        XCTAssertFalse(c.wants(frame: 0, take: 1), "frame 0 is never a thumbnail")
        XCTAssertFalse(c.wants(frame: 10, take: 2), "other take")
        for f in stride(from: UInt64(10), through: 100, by: 10) { XCTAssertTrue(c.insert(frame: f, take: 1, image: image())) }
        XCTAssertFalse(c.wants(frame: 10, take: 1), "cached")
        XCTAssertEqual(c.count, 10)
        XCTAssertFalse(c.insert(frame: 110, take: 2, image: image()), "stale take rejected")

        // Branch at frame 40 (take 2 shares 40 frames with take 1): only frames > 40 are dropped.
        let gen = c.generation
        c.rebase(toTake: 2, keepThrough: 40)
        XCTAssertEqual(c.takeID, 2)
        XCTAssertNotEqual(c.generation, gen)
        XCTAssertEqual(c.missing([10, 20, 30, 40, 50, 60]), [50, 60])
        XCTAssertFalse(c.insert(frame: 50, take: 2, generation: gen, image: image()), "job from before the switch")
        XCTAssertTrue(c.insert(frame: 50, take: 2, generation: c.generation, image: image()))
        c.rebase(toTake: 2, keepThrough: 0)
        XCTAssertEqual(c.count, 5, "same take: no-op")

        // Exact lookup; fallback only to an EARLIER picture within the window.
        XCTAssertNotNil(c.image(at: 30))
        XCTAssertNil(c.image(at: 33))
        XCTAssertNotNil(c.image(before: 45, within: 5), "40 is 5 earlier")
        XCTAssertNil(c.image(before: 45, within: 4))
        XCTAssertNil(c.image(before: 10, within: 100), "nothing earlier than 10")
        XCTAssertNil(c.image(before: 500, within: 100))
        // A batch is one change, rejected when stale.
        let v = c.version
        XCTAssertTrue(c.insert(batch: [(60, image()), (70, image())], take: 2, generation: c.generation))
        XCTAssertEqual(c.version, v + 1)
        XCTAssertFalse(c.insert(batch: [(80, image())], take: 2, generation: c.generation &- 1))
        XCTAssertEqual(c.count, 7)

        // Session change.
        c.reset(take: 7)
        XCTAssertEqual(c.count, 0)
        XCTAssertNil(c.image(at: 30))

        // Capacity: entries off the current grid are evicted when over capacity.
        c.capacity = 5
        for f in stride(from: UInt64(4), through: 40, by: 4) { c.insert(frame: f, take: 7, image: image()) }
        c.setStep(8)
        XCTAssertEqual(c.missing([8, 16, 24, 32, 40]), [])
        XCTAssertEqual(c.count, 6, "grid + tile 0's picture (frame 4 at this step)")
    }

    func testDownscaler() {
        var px = [UInt32](repeating: 0, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
        for y in 0..<Int(RN_VIDEO_HEIGHT) {
            for x in 0..<Int(RN_VIDEO_WIDTH) {
                // Left half red, right half: 1-pixel checkerboard of white/black (averages to gray).
                px[y * Int(RN_VIDEO_WIDTH) + x] = x < 128 ? 0xFFFF_0000 : ((x + y) % 2 == 0 ? 0xFFFF_FFFF : 0xFF00_0000)
            }
        }
        let out = px.withUnsafeBufferPointer { ThumbnailScaler.downscale($0.baseAddress!) }
        XCTAssertEqual(out.count, 128 * 120)
        XCTAssertEqual(out[0], 0xFFFF_0000)
        XCTAssertEqual(out[119 * 128 + 63], 0xFFFF_0000)
        XCTAssertEqual(out[10 * 128 + 80], 0xFF7F_7F7F)
        XCTAssertNotNil(ThumbnailScaler.image(out))
    }

    // MARK: engine (Swift wrapper)

    func testPracticeSetRangeKeepsCursor() throws {
        let rom = tmp.appendingPathComponent("test.nes")
        try Engine.writeTestROM(to: rom)
        let s = try EngineSession.create(rom: rom, projectDir: nil)
        for i in 0..<300 { try s.step(p1: UInt8(i % 7 == 0 ? RN_BTN_A : 0), p2: 0, events: 0) }
        try s.setMode(RN_MODE_REPLAY)
        try s.seek(250)
        let h = s.stateHash(), v = s.videoHash
        try s.practiceSetRange(2, a: 60, b: 180)
        XCTAssertEqual(s.frame, 250)
        XCTAssertEqual(s.stateHash(), h)
        XCTAssertEqual(s.videoHash, v)
        let info = s.practiceSlot(2)
        XCTAssertTrue(info.hasA && info.hasB && info.hasTakeFrame)
        XCTAssertEqual(info.takeFrame, 60)
        XCTAssertEqual(info.length, 120)
        XCTAssertEqual(info.takeID, s.activeTake)
        XCTAssertEqual(TimelineEditing.visibleRanges(slots: s.practiceSlots(), takes: s.takes(), activeTake: s.activeTake,
                                                     takeLength: s.takeLength),
                       [TimelineRange(slot: 2, a: 60, b: 180)])
        XCTAssertThrowsError(try s.practiceSetRange(2, a: 180, b: 180))

        // A branch before B hides the range on the new take (it lives on the old one).
        try s.setMode(RN_MODE_RECORD)
        try s.seek(100)
        try s.step(p1: UInt8(RN_BTN_B), p2: 0, events: 0)
        XCTAssertEqual(TimelineEditing.visibleRanges(slots: s.practiceSlots(), takes: s.takes(), activeTake: s.activeTake,
                                                     takeLength: s.takeLength), [])
        try s.undoTakeSwitch()
        XCTAssertEqual(TimelineEditing.visibleRanges(slots: s.practiceSlots(), takes: s.takes(), activeTake: s.activeTake,
                                                     takeLength: s.takeLength).map(\.slot), [2])

        // The renderer used by the thumbnail generator agrees with the live picture at a cursor frame.
        try s.seek(120)
        let live = s.video.map { ThumbnailScaler.downscale($0) }
        let r = try s.makeRenderer(start: 119, end: 120)
        defer { rn_renderer_free(r) }
        var video: UnsafePointer<UInt32>?
        var audio: UnsafePointer<Int16>?
        var n = 0
        var idx: UInt64 = 0
        XCTAssertEqual(rn_renderer_next(r, &video, &audio, &n, &idx), RN_OK)
        XCTAssertEqual(idx + 1, 120, "renderer index i = picture at cursor i+1")
        XCTAssertEqual(video.map { ThumbnailScaler.downscale($0) }, live)
    }
}
