// Filmstrip timeline logic: frame <-> x mapping, A/B range gestures -> frames, take lineage,
// thumbnail grid / cache invalidation, downscaler, and rn_practice_set_range through Swift.
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

    private func take(_ id: UInt64, parent: UInt64, branch: UInt64, length: UInt64) -> TakeInfo {
        TakeInfo(id: id, parentID: parent, branchFrame: branch, length: length, createdSeq: id, isActive: false, childCount: 0)
    }

    // MARK: geometry + gestures

    func testGeometryMapping() {
        let g = TimelineGeometry(width: 600, length: 1200)
        XCTAssertEqual(g.frame(atX: 0), 0)
        XCTAssertEqual(g.frame(atX: 300), 600)
        XCTAssertEqual(g.frame(atX: 600), 1200)
        XCTAssertEqual(g.frame(atX: -50), 0, "clamped left")
        XCTAssertEqual(g.frame(atX: 9999), 1200, "clamped right")
        XCTAssertEqual(g.x(forFrame: 600), 300, accuracy: 1e-9)
        XCTAssertEqual(g.x(forFrame: 5000), 600, accuracy: 1e-9)
        for f: UInt64 in [0, 1, 2, 599, 1199, 1200] { XCTAssertEqual(g.frame(atX: g.x(forFrame: f)), f) }
        XCTAssertEqual(TimelineGeometry(width: 600, length: 0).frame(atX: 300), 0)
        XCTAssertEqual(TimelineGeometry(width: 0, length: 100).x(forFrame: 50), 0)
    }

    func testRangeGestureToFrames() {
        let g = TimelineGeometry(width: 500, length: 1000)
        XCTAssertTrue(TimelineEditing.range(fromX: 100, toX: 250, in: g)! == (200, 500))
        XCTAssertTrue(TimelineEditing.range(fromX: 250, toX: 100, in: g)! == (200, 500), "right-to-left drag")
        XCTAssertTrue(TimelineEditing.range(fromX: -20, toX: 900, in: g)! == (0, 1000), "clamped")
        XCTAssertNil(TimelineEditing.range(fromX: 100, toX: 100.2, in: g), "a click is not a range")

        // Handle drags keep a < b and stay inside the take.
        XCTAssertTrue(TimelineEditing.drag(.a, of: (200, 500), toX: 50, in: g) == (100, 500))
        XCTAssertTrue(TimelineEditing.drag(.a, of: (200, 500), toX: 400, in: g) == (499, 500), "A cannot pass B")
        XCTAssertTrue(TimelineEditing.drag(.b, of: (200, 500), toX: 450, in: g) == (200, 900))
        XCTAssertTrue(TimelineEditing.drag(.b, of: (200, 500), toX: 10, in: g) == (200, 201), "B cannot pass A")
        XCTAssertTrue(TimelineEditing.drag(.b, of: (200, 500), toX: 800, in: g) == (200, 1000))
    }

    func testHitTest() {
        let g = TimelineGeometry(width: 1000, length: 1000)
        let ranges = [TimelineRange(slot: 0, a: 100, b: 300), TimelineRange(slot: 1, a: 280, b: 600),
                      TimelineRange(slot: 2, a: 800, b: nil)]
        XCTAssertEqual(TimelineEditing.hitTest(x: 102, ranges: ranges, in: g), .handle(slot: 0, .a))
        XCTAssertEqual(TimelineEditing.hitTest(x: 150, ranges: ranges, in: g), .body(slot: 0))
        XCTAssertEqual(TimelineEditing.hitTest(x: 298, ranges: ranges, in: g), .handle(slot: 0, .b), "closest edge wins")
        XCTAssertEqual(TimelineEditing.hitTest(x: 283, ranges: ranges, in: g), .handle(slot: 1, .a))
        XCTAssertEqual(TimelineEditing.hitTest(x: 290, ranges: ranges, in: g, tolerance: 10), .handle(slot: 1, .a),
                       "tie -> later range")
        XCTAssertEqual(TimelineEditing.hitTest(x: 290, ranges: ranges, in: g, tolerance: 10, preferred: 0), .handle(slot: 0, .b),
                       "tie -> selected slot")
        XCTAssertEqual(TimelineEditing.hitTest(x: 290.5, ranges: ranges, in: g, tolerance: 1), .body(slot: 1),
                       "overlap: top-most body")
        XCTAssertEqual(TimelineEditing.hitTest(x: 290.5, ranges: ranges, in: g, tolerance: 1, preferred: 0), .body(slot: 0))
        XCTAssertEqual(TimelineEditing.hitTest(x: 801, ranges: ranges, in: g), .handle(slot: 2, .a), "A-only flag")
        XCTAssertEqual(TimelineEditing.hitTest(x: 850, ranges: ranges, in: g), .none)
        XCTAssertEqual(TimelineEditing.hitTest(x: 700, ranges: ranges, in: g), .none)
    }

    func testMarkAtPlayhead() {
        let r = TimelineRange(slot: 0, a: 100, b: 300)
        XCTAssertEqual(TimelineEditing.markA(at: 50, existing: r), .setRange(a: 50, b: 300))
        XCTAssertEqual(TimelineEditing.markA(at: 299, existing: r), .setRange(a: 299, b: 300))
        XCTAssertEqual(TimelineEditing.markA(at: 300, existing: r), .setAOnly, "A at/after B: B is cleared")
        XCTAssertEqual(TimelineEditing.markA(at: 10, existing: nil), .setAOnly)
        XCTAssertEqual(TimelineEditing.markA(at: 10, existing: TimelineRange(slot: 0, a: 5, b: nil)), .setAOnly)
        XCTAssertEqual(TimelineEditing.markB(at: 400, existing: r), .setRange(a: 100, b: 400))
        XCTAssertEqual(TimelineEditing.markB(at: 150, existing: TimelineRange(slot: 0, a: 100, b: nil)), .setRange(a: 100, b: 150))
        guard case .invalid = TimelineEditing.markB(at: 100, existing: r) else { return XCTFail("B == A") }
        guard case .invalid = TimelineEditing.markB(at: 100, existing: nil) else { return XCTFail("no A") }
    }

    // MARK: take lineage + visible ranges

    func testSharedPrefix() {
        // 1: root 0..1000; 2 branches from 1 at 400 (length 900); 3 from 2 at 600 (len 700); 4 from 1 at 700.
        let takes = [take(1, parent: 0, branch: 0, length: 1000), take(2, parent: 1, branch: 400, length: 900),
                     take(3, parent: 2, branch: 600, length: 700), take(4, parent: 1, branch: 700, length: 750)]
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 1, 1), 1000)
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 1, 2), 400)
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 2, 1), 400)
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 2, 3), 600)
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 3, 4), 400)
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 4, 1), 700)
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 2, 4), 400)
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 0, 1), 0, "unknown take shares nothing")
        XCTAssertEqual(TakeLineage.sharedPrefix(takes, 1, 99), 0)
    }

    func testVisibleRanges() {
        let takes = [take(1, parent: 0, branch: 0, length: 1000), take(2, parent: 1, branch: 400, length: 900)]
        var s0 = PracticeSlotInfo(index: 0); s0.hasA = true; s0.hasB = true; s0.hasTakeFrame = true
        s0.takeFrame = 100; s0.length = 200; s0.takeID = 1          // 100..300 on the shared prefix
        var s1 = PracticeSlotInfo(index: 1); s1.hasA = true; s1.hasB = true; s1.hasTakeFrame = true
        s1.takeFrame = 350; s1.length = 100; s1.takeID = 1          // 350..450 crosses the branch at 400
        var s2 = PracticeSlotInfo(index: 2); s2.hasA = true  // set inside practice
        var s3 = PracticeSlotInfo(index: 3); s3.hasA = true; s3.hasTakeFrame = true; s3.takeFrame = 500; s3.takeID = 2
        let slots = [s0, s1, s2, s3, PracticeSlotInfo(index: 4)]
        XCTAssertEqual(TimelineEditing.visibleRanges(slots: slots, takes: takes, activeTake: 2, takeLength: 900),
                       [TimelineRange(slot: 0, a: 100, b: 300), TimelineRange(slot: 3, a: 500, b: nil)])
        XCTAssertEqual(TimelineEditing.visibleRanges(slots: slots, takes: takes, activeTake: 1, takeLength: 1000),
                       [TimelineRange(slot: 0, a: 100, b: 300), TimelineRange(slot: 1, a: 350, b: 450)])
    }

    // MARK: thumbnails

    private let tw = 256.0 / 240.0 * 40

    func testThumbnailSteps() {
        XCTAssertEqual(ThumbnailGrid.baseStep, 300)
        XCTAssertEqual(ThumbnailGrid.minStep, 300, "never more than one picture per 5 s")
        XCTAssertTrue(ThumbnailGrid.isStep(300))
        XCTAssertTrue(ThumbnailGrid.isStep(2_400))
        XCTAssertFalse(ThumbnailGrid.isStep(150))
        XCTAssertFalse(ThumbnailGrid.isStep(450))
        XCTAssertFalse(ThumbnailGrid.isStep(0))
        // Coarser grids are subsets of finer ones (growing takes / narrower windows reuse images).
        let fine = Set(ThumbnailGrid.frames(length: 100_000, step: 600))
        XCTAssertTrue(fine.isSuperset(of: ThumbnailGrid.frames(length: 100_000, step: 1_200)))
        XCTAssertEqual(ThumbnailGrid.frames(length: 299, step: 300), [])
        XCTAssertEqual(ThumbnailGrid.frames(length: 1_000, step: 300), [300, 600, 900])
        XCTAssertTrue(ThumbnailGrid.isGridFrame(900, step: 300))
        XCTAssertFalse(ThumbnailGrid.isGridFrame(901, step: 300))
        XCTAssertFalse(ThumbnailGrid.isGridFrame(0, step: 300))
        // One picture per tile: tile 0 shows the screen 1 s in (not a later tile's picture).
        XCTAssertEqual(ThumbnailGrid.startPicture(step: 300), 60)
        XCTAssertEqual(ThumbnailGrid.targets(length: 59, step: 300), [])
        XCTAssertEqual(ThumbnailGrid.targets(length: 60, step: 300), [60])
        XCTAssertEqual(ThumbnailGrid.targets(length: 1_000, step: 300), [60, 300, 600, 900])
        XCTAssertEqual(ThumbnailGrid.targets(length: 2_400, step: 1_200), [60, 1_200, 2_400])
    }

    func testScaleIsSmallestStepThatFitsTheTake() {
        for w in [300.0, 575.0, 800.0, 1_400.0] {
            for len in stride(from: UInt64(0), through: 400_000, by: 97) {
                let f = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len)
                XCTAssertGreaterThanOrEqual(f, 300, "never more than one picture per 5 s")
                XCTAssertTrue(ThumbnailGrid.isStep(f))
                XCTAssertLessThanOrEqual(ThumbnailGrid.extent(length: len, step: f, tileWidth: tw), w + 1e-9, "the take fits")
                if f > 300 {
                    XCTAssertGreaterThan(ThumbnailGrid.extent(length: len, step: f / 2, tileWidth: tw), w, "smallest such step")
                }
                let tiles = ThumbnailGrid.tiles(length: len, step: f, tileWidth: tw)
                XCTAssertEqual(tiles.count, Int((Double(len) / f).rounded(.up)), "tile count == ceil(length / F)")
                XCTAssertEqual(Set(tiles.map(\.picture)).count, tiles.count, "one distinct picture per tile")
            }
        }
        // 575 pt strip: 5 s tiles until 13 tiles fill it (about 67 s), then 10 s, 20 s ...
        XCTAssertEqual(ThumbnailGrid.tileStep(width: 575, tileWidth: tw, length: 120), 300)
        XCTAssertEqual(ThumbnailGrid.tileStep(width: 575, tileWidth: tw, length: 4_000), 300)
        XCTAssertEqual(ThumbnailGrid.tileStep(width: 575, tileWidth: tw, length: 4_100), 600)
        XCTAssertEqual(ThumbnailGrid.tileStep(width: 575, tileWidth: tw, length: 36_000), 4_800)
    }

    func testTilesNaturalWidthRevealedUpToTheExtent() {
        // 7 s at 5 s per tile: tile 0 full (natural width), tile 1 revealed for 2 s of its 5 s.
        let tiles = ThumbnailGrid.tiles(length: 420, step: 300, tileWidth: tw)
        XCTAssertEqual(tiles.count, 2)
        XCTAssertEqual(tiles.map(\.frame), [0, 300])
        XCTAssertEqual(tiles.map(\.picture), [60, 300])
        XCTAssertEqual(tiles[0].x, 0)
        XCTAssertEqual(tiles[0].visible, tw, accuracy: 1e-9)
        XCTAssertEqual(tiles[1].x, tw, accuracy: 1e-9)
        XCTAssertEqual(tiles[1].visible, Double(420 - 300) * tw / 300, accuracy: 1e-9, "(length - tile start) * px per frame")
        // Nothing is drawn beyond the recorded extent; every full tile has the natural width.
        for len in UInt64(1)...UInt64(5_000) {
            let f = ThumbnailGrid.tileStep(width: 575, tileWidth: tw, length: len)
            let end = ThumbnailGrid.extent(length: len, step: f, tileWidth: tw)
            let t = ThumbnailGrid.tiles(length: len, step: f, tileWidth: tw)
            for x in t.dropLast() { XCTAssertEqual(x.visible, tw, accuracy: 1e-9) }
            XCTAssertEqual(t.last!.x + t.last!.visible, end, accuracy: 1e-9)
            XCTAssertGreaterThan(t.last!.visible, 0)
            XCTAssertLessThanOrEqual(t.last!.visible, tw + 1e-9)
        }
        // A new tile appears right when its interval starts.
        XCTAssertEqual(ThumbnailGrid.tiles(length: 300, step: 300, tileWidth: tw).count, 1)
        XCTAssertEqual(ThumbnailGrid.tiles(length: 301, step: 300, tileWidth: tw).count, 2)
        XCTAssertEqual(ThumbnailGrid.tiles(length: 0, step: 300, tileWidth: tw), [])
    }

    func testFrameMappingAtFixedScale() {
        // The bar maps through TimelineGeometry(width: extent): x = frame * tileWidth / F.
        let len: UInt64 = 1_000, f = 300.0
        let g = TimelineGeometry(width: ThumbnailGrid.extent(length: len, step: f, tileWidth: tw), length: len)
        XCTAssertEqual(g.x(forFrame: 300), tw, accuracy: 1e-9)
        XCTAssertEqual(g.x(forFrame: 600), 2 * tw, accuracy: 1e-9)
        XCTAssertEqual(g.frame(atX: tw), 300)
        XCTAssertEqual(g.frame(atX: 10_000), len, "beyond the recorded extent clamps to the end")
        XCTAssertEqual(g.frame(atX: -5), 0)
        XCTAssertEqual(g.x(forFrame: 5_000), g.width, "the playhead never passes the end")
    }

    func testPlayheadSnapsToWholePixels() {
        XCTAssertEqual(ThumbnailGrid.snapToPixel(10.74, scale: 2), 10.5)
        XCTAssertEqual(ThumbnailGrid.snapToPixel(10.49, scale: 2), 10)
        XCTAssertEqual(ThumbnailGrid.snapToPixel(10.5, scale: 2), 10.5)
        XCTAssertEqual(ThumbnailGrid.snapToPixel(3.99, scale: 1), 3)
        // Recording at 5 s per tile: the marker moves forward by 0 or 1 pixel per frame, never more.
        let px = tw / 300
        var last = 0.0
        for len in UInt64(0)...UInt64(3_000) {
            let x = ThumbnailGrid.snapToPixel(Double(len) * px, scale: 2)
            XCTAssertTrue(x == last || abs(x - last - 0.5) < 1e-9, "1-dot steps")
            last = x
        }
    }

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
