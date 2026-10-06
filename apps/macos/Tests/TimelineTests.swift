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

    func testThumbnailGrid() {
        // Never denser than one picture per 5 s (300 frames), then 10 s, 20 s, ...
        XCTAssertEqual(ThumbnailGrid.baseStep, 300)
        XCTAssertEqual(ThumbnailGrid.step(length: 100, maxCount: 50), 300)
        XCTAssertEqual(ThumbnailGrid.step(length: 10_000, maxCount: 50), 300)
        XCTAssertEqual(ThumbnailGrid.step(length: 30_000, maxCount: 50), 600)
        XCTAssertEqual(ThumbnailGrid.step(length: 60_000, maxCount: 50), 1_200)
        // 1 hour take, 60 grid frames at most.
        let hour: UInt64 = 216_000
        let q = ThumbnailGrid.step(length: hour, maxCount: 60)
        XCTAssertLessThanOrEqual(hour / q, 60)
        XCTAssertGreaterThan(hour / (q / 2), 60, "smallest power-of-two step")
        XCTAssertEqual(q % ThumbnailGrid.baseStep, 0)
        // Coarser grids are subsets of finer ones (growing takes reuse images).
        let fine = Set(ThumbnailGrid.frames(length: 100_000, step: 600))
        XCTAssertTrue(fine.isSuperset(of: ThumbnailGrid.frames(length: 100_000, step: 1_200)))
        XCTAssertEqual(ThumbnailGrid.frames(length: 3, step: 4), [])
        XCTAssertEqual(ThumbnailGrid.frames(length: 13, step: 4), [4, 8, 12])

    }

    // MARK: filmstrip layout (anchored tiles)

    func testFilmstripTilesAnchoredAndClipped() {
        // 400 pt strip, 1000 frames, 100 frames per tile -> 40 pt per tile.
        let tiles = ThumbnailGrid.tiles(width: 400, length: 1000, step: 100)
        XCTAssertEqual(tiles.count, 10, "anchor at 1000 == strip end has no width")
        XCTAssertEqual(tiles.map(\.frame), stride(from: 0, to: 1000, by: 100).map { UInt64($0) })
        for (k, t) in tiles.enumerated() {
            XCTAssertEqual(t.x, Double(k) * 40, accuracy: 1e-9, "x = g.x(k*F)")
            XCTAssertEqual(t.span, 40, accuracy: 1e-9)
        }
        // A 42.67 pt picture is clipped at the next anchor; the last one at the strip end.
        let tw = 256.0 / 240.0 * 40
        XCTAssertEqual(tiles[3].visibleWidth(tileWidth: tw), 40, accuracy: 1e-9)

        // Partial last tile: 1050 frames -> anchors 0..1000, the last one 1000..1050 is cut.
        let t2 = ThumbnailGrid.tiles(width: 420, length: 1050, step: 100)
        XCTAssertEqual(t2.count, 11)
        XCTAssertEqual(t2.last!.frame, 1000)
        XCTAssertEqual(t2.last!.x, 400, accuracy: 1e-9)
        XCTAssertEqual(t2.last!.span, 20, accuracy: 1e-9)
        XCTAssertEqual(t2.last!.visibleWidth(tileWidth: tw), 20, accuracy: 1e-9)
        // Tiles cover the strip exactly, without gaps or overlaps.
        var x = 0.0
        for t in t2 { XCTAssertEqual(t.x, x, accuracy: 1e-9); x += t.span }
        XCTAssertEqual(x, 420, accuracy: 1e-9)

        XCTAssertEqual(ThumbnailGrid.tiles(width: 400, length: 0, step: 4), [])
        XCTAssertEqual(ThumbnailGrid.tiles(width: 400, length: 3, step: 4).count, 1, "shorter than a step: one tile")
    }

    func testFilmstripTileStepHysteresis() {
        let w = 800.0, tw = 256.0 / 240.0 * 40
        func span(_ f: UInt64, _ len: UInt64) -> Double { ThumbnailGrid.span(step: f, width: w, length: len) }
        // Fresh choice: largest power-of-two step with span <= tileWidth (span in (0.5, 1] * tw).
        for len: UInt64 in [6_000, 36_000, 216_000, 1_000_000] {
            let f = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len)
            XCTAssertEqual(f % ThumbnailGrid.baseStep, 0)
            let m = f / ThumbnailGrid.baseStep
            XCTAssertEqual(m & (m - 1), 0, "power-of-two multiple of 5 s")
            XCTAssertLessThanOrEqual(span(f, len), tw)
            XCTAssertGreaterThan(span(f, len), tw / 2)
        }
        // Too short to fill the strip: baseStep (tiles wider than a picture repeat it).
        XCTAssertEqual(ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: 20), ThumbnailGrid.baseStep)
        XCTAssertEqual(ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: 3_000), ThumbnailGrid.baseStep)
        XCTAssertGreaterThan(span(ThumbnailGrid.baseStep, 3_000), tw)

        // Growing take: F is kept while span >= 0.4 tw, then doubles once (span <= 0.8 tw: no gap).
        var f = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: 6_000)
        var switches: [(len: UInt64, from: UInt64, to: UInt64)] = []
        for len in UInt64(6_000)...UInt64(120_000) {
            let n = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len, current: f)
            if n != f {
                switches.append((len, f, n))
                XCTAssertEqual(n, f * 2, "one doubling at a time")
                XCTAssertLessThan(span(f, len), ThumbnailGrid.minSpanRatio * tw)
                XCTAssertGreaterThanOrEqual(span(f, len - 1), ThumbnailGrid.minSpanRatio * tw)
            }
            XCTAssertLessThanOrEqual(span(n, len), tw)
            XCTAssertGreaterThanOrEqual(span(n, len), ThumbnailGrid.minSpanRatio * tw)
            f = n
        }
        XCTAssertEqual(switches.count, 4, "6000 -> 120000 frames at 800 pt: F 5 s -> 80 s")

        // Hysteresis: in the overlap band both F and 2F are kept (no flip-flop on small changes).
        let len: UInt64 = 100_000
        let fresh = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len)
        // span(fresh) in (0.5, 1] tw; span(fresh/2) in (0.25, 0.5] tw. Pick a length where
        // span(fresh/2) is in [0.4, 0.5] so the finer step is still acceptable.
        let half = fresh / 2
        if span(half, len) >= ThumbnailGrid.minSpanRatio * tw {
            XCTAssertEqual(ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len, current: half), half)
        }
        XCTAssertEqual(ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len, current: fresh), fresh)
        // Shrinking (shorter take / wider window): span > tw halves the step.
        let shrunk = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len / 3, current: fresh)
        XCTAssertLessThan(shrunk, fresh)
        XCTAssertLessThanOrEqual(span(shrunk, len / 3), tw)
        // Garbage hints are ignored.
        XCTAssertEqual(ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len, current: 12), fresh)
        XCTAssertEqual(ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len, current: 450), fresh)
        XCTAssertEqual(ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len, current: fresh * 64), fresh)
    }

    func testShortTakeTilesFillTheStrip() {
        // 20 s take on an 800 pt strip: 5 s tiles (200 pt) are wider than a picture (42.7 pt);
        // they still cover the strip without gaps (the view repeats the picture inside a tile).
        let w = 800.0, tw = 256.0 / 240.0 * 40
        for len: UInt64 in [1, 299, 300, 301, 1_200, 3_000] {
            let f = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len)
            XCTAssertEqual(f, ThumbnailGrid.baseStep)
            let tiles = ThumbnailGrid.tiles(width: w, length: len, step: f)
            XCTAssertEqual(tiles.first?.x, 0)
            var x = 0.0
            for t in tiles { XCTAssertEqual(t.x, x, accuracy: 1e-9); x += t.span }
            XCTAssertEqual(x, w, accuracy: 1e-6, "no gap at the end (\(len) frames)")
        }
    }

    func testFilmstripPositionsMoveSmoothlyAsTakeGrows() {
        // Recording: one frame at a time. Every tile anchored at the same frame only moves left,
        // by much less than a tile per frame; the visible end of the strip is always covered.
        let w = 800.0, tw = 256.0 / 240.0 * 40
        var f = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: 6_000)
        var prev: [UInt64: Double] = [:]
        for len in UInt64(6_000)...UInt64(60_000) {
            f = ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: len, current: f)
            let tiles = ThumbnailGrid.tiles(width: w, length: len, step: f)
            XCTAssertEqual(tiles.first?.x, 0)
            XCTAssertEqual((tiles.last?.x ?? 0) + (tiles.last?.span ?? 0), w, accuracy: 1e-6, "no gap at the end")
            XCTAssertTrue(tiles.allSatisfy { $0.span <= tw + 1e-9 }, "no gap between pictures")
            var cur: [UInt64: Double] = [:]
            for t in tiles {
                cur[t.frame] = t.x
                if let px = prev[t.frame] {
                    XCTAssertLessThanOrEqual(t.x, px + 1e-9, "anchors only move left")
                    XCTAssertLessThan(px - t.x, 2, "smooth: well under a tile per frame")
                }
            }
            prev = cur
        }
    }

    func testThumbnailCacheInvalidation() {
        let c = ThumbnailCache()
        c.reset(take: 1)
        c.setStep(10)
        XCTAssertTrue(c.wants(frame: 10, take: 1))
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

        // Nearest lookup.
        XCTAssertNotNil(c.image(near: 30, tolerance: 0))
        XCTAssertNotNil(c.image(near: 33, tolerance: 5))
        XCTAssertNil(c.image(near: 47, tolerance: 2))
        XCTAssertNotNil(c.image(near: 47, tolerance: 3), "50 is 3 away")
        XCTAssertNil(c.image(near: 500, tolerance: 100))

        // Session change.
        c.reset(take: 7)
        XCTAssertEqual(c.count, 0)
        XCTAssertNil(c.image(near: 30, tolerance: 1000))

        // Capacity: entries off the current grid are evicted when over capacity.
        c.capacity = 5
        for f in stride(from: UInt64(4), through: 40, by: 4) { c.insert(frame: f, take: 7, image: image()) }
        c.setStep(8)
        XCTAssertEqual(c.missing([8, 16, 24, 32, 40]), [])
        XCTAssertEqual(c.count, 5)
    }

    func testDownscaler() {
        var px = [UInt32](repeating: 0, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT))
        for y in 0..<Int(RN_VIDEO_HEIGHT) {
            for x in 0..<Int(RN_VIDEO_WIDTH) {
                // Left half red, right half: 4x4 checkerboard of white/black (averages to gray).
                px[y * Int(RN_VIDEO_WIDTH) + x] = x < 128 ? 0xFFFF_0000 : ((x + y) % 2 == 0 ? 0xFFFF_FFFF : 0xFF00_0000)
            }
        }
        let out = px.withUnsafeBufferPointer { ThumbnailScaler.downscale($0.baseAddress!) }
        XCTAssertEqual(out.count, 64 * 60)
        XCTAssertEqual(out[0], 0xFFFF_0000)
        XCTAssertEqual(out[59 * 64 + 31], 0xFFFF_0000)
        XCTAssertEqual(out[10 * 64 + 40], 0xFF7F_7F7F)
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
