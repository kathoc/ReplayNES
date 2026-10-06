// Headless tests for the ROM library (folder layout, scanning, SHA-256 matching, naming) and
// the Swift side of the flash reduction filter (display-only; export hash unchanged).
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class LibraryTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-lib-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tmp)
    }

    func testEnsureCreatesFoldersAndReportsConflicts() throws {
        let paths = LibraryPaths(root: tmp.appendingPathComponent("ReplayNES"))
        try paths.ensure()
        var isDir: ObjCBool = false
        XCTAssertTrue(FileManager.default.fileExists(atPath: paths.roms.path, isDirectory: &isDir) && isDir.boolValue)
        XCTAssertTrue(FileManager.default.fileExists(atPath: paths.projects.path, isDirectory: &isDir) && isDir.boolValue)
        try paths.ensure() // idempotent

        let bad = LibraryPaths(root: tmp.appendingPathComponent("Bad"))
        try FileManager.default.createDirectory(at: bad.root, withIntermediateDirectories: true)
        try Data().write(to: bad.roms) // a file where ROM/ should be
        XCTAssertThrowsError(try bad.ensure()) { e in
            XCTAssertEqual(e as? LibraryError, .notADirectory(bad.roms.path))
        }
    }

    func testScanFindsNesFilesOneLevelDeepSortedByName() throws {
        let paths = LibraryPaths(root: tmp)
        try paths.ensure()
        let fm = FileManager.default
        try fm.createDirectory(at: paths.roms.appendingPathComponent("Sub/Deeper"), withIntermediateDirectories: true)
        for name in ["b game.NES", "A Game.nes", "Sub/c game.Nes", "Sub/Deeper/too deep.nes", "readme.txt", "notes.nes.txt", "game10.nes", "game9.nes"] {
            try Data([0x4E, 0x45, 0x53, 0x1A]).write(to: paths.roms.appendingPathComponent(name))
        }
        let roms = try LibraryScanner.scanROMs(in: paths.roms)
        XCTAssertEqual(roms.map(\.name), ["A Game", "b game", "c game", "game9", "game10"])
        XCTAssertEqual(roms.first { $0.name == "c game" }?.relativePath, "Sub/c game.Nes")
        XCTAssertThrowsError(try LibraryScanner.scanROMs(in: tmp.appendingPathComponent("missing")))
    }

    func testProjectsAreMatchedBySHA256NotByName() throws {
        let paths = LibraryPaths(root: tmp)
        try paths.ensure()
        let rom = paths.roms.appendingPathComponent("Test ROM.nes")
        try Engine.writeTestROM(to: rom)
        let date = Date(timeIntervalSince1970: 1_790_000_000)
        let dir = LibraryScanner.newProjectURL(projectsDir: paths.projects, romName: "Test ROM", date: date)
        XCTAssertEqual(dir.lastPathComponent, "Test ROM \(LibraryScanner.timestamp(date)).nesrec")
        do {
            let s = try EngineSession.create(rom: rom, projectDir: dir)
            try s.setMode(RN_MODE_RECORD)
            for _ in 0..<30 { try s.step(p1: 0, p2: 0, events: 0) }
            try s.save()
        }
        // Same name again -> unique suffix.
        let dir2 = LibraryScanner.newProjectURL(projectsDir: paths.projects, romName: "Test ROM", date: date)
        XCTAssertEqual(dir2.lastPathComponent, "Test ROM \(LibraryScanner.timestamp(date)) 2.nesrec")

        // Rename the ROM: the project still belongs to it (SHA-256), a different ROM gets none.
        let renamed = paths.roms.appendingPathComponent("Renamed.nes")
        try FileManager.default.moveItem(at: rom, to: renamed)
        var other = try Data(contentsOf: renamed)
        other[other.count - 1] ^= 0xFF
        try other.write(to: paths.roms.appendingPathComponent("Other.nes"))
        try Data("junk".utf8).write(to: paths.projects.appendingPathComponent("not a project.txt"))

        let projects = try LibraryScanner.scanProjects(in: paths.projects)
        XCTAssertEqual(projects.count, 1)
        let cache = ROMHashCache()
        var roms = try LibraryScanner.scanROMs(in: paths.roms)
        for i in roms.indices { roms[i].sha256 = cache.sha256(of: roms[i]) }
        let byName = Dictionary(uniqueKeysWithValues: roms.map { ($0.name, $0) })
        XCTAssertEqual(byName["Renamed"]?.sha256, projects[0].romSHA256)
        XCTAssertNotEqual(byName["Other"]?.sha256, projects[0].romSHA256)
        XCTAssertEqual(projects[0].name, "Test ROM \(LibraryScanner.timestamp(date))")
    }

    // MARK: flash reduction (Swift wrapper + export)

    func testFlashFilterLimitsFullScreenFlashingAndOffIsIdentity() {
        let n = Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT)
        let black = [UInt32](repeating: 0xFF00_0000, count: n), white = [UInt32](repeating: 0xFFFF_FFFF, count: n)
        var out = [UInt32](repeating: 0, count: n)
        let f = FlashFilter(level: .standard)
        var altered = 0
        for i in 0..<60 {
            let src = i % 2 == 0 ? black : white
            if src.withUnsafeBufferPointer({ s in out.withUnsafeMutableBufferPointer { f.process(s.baseAddress!, into: $0.baseAddress!) } }) {
                altered += 1
            }
        }
        XCTAssertGreaterThan(altered, 20)
        f.setLevel(.off)
        white.withUnsafeBufferPointer { s in out.withUnsafeMutableBufferPointer { _ = f.process(s.baseAddress!, into: $0.baseAddress!) } }
        XCTAssertEqual(out, white)
    }

    func testExportWithFlashReductionKeepsRendererHash() throws {
        let rom = tmp.appendingPathComponent("t.nes")
        try Engine.writeTestROM(to: rom)
        let s = try EngineSession.create(rom: rom, projectDir: nil)
        try s.setMode(RN_MODE_RECORD)
        for f in 0..<90 { try s.step(p1: UInt8(f % 7 == 0 ? RN_BTN_A : 0), p2: 0, events: 0) }
        let hashBefore = s.stateHash()
        var settings = ExportSettings()
        settings.preset = .native(1)
        let plain = try MP4Exporter(renderer: s.makeRenderer(), settings: settings, url: tmp.appendingPathComponent("a.mp4"))
            .run(progress: { _, _ in }, isCancelled: { false })
        settings.flashReduction = .high
        let filtered = try MP4Exporter(renderer: s.makeRenderer(), settings: settings, url: tmp.appendingPathComponent("b.mp4"))
            .run(progress: { _, _ in }, isCancelled: { false })
        XCTAssertEqual(plain.frames, 90)
        XCTAssertEqual(filtered.frames, 90)
        XCTAssertEqual(plain.rendererHash, filtered.rendererHash, "flash reduction must not touch emulation")
        XCTAssertEqual(s.stateHash(), hashBefore)
    }
}
