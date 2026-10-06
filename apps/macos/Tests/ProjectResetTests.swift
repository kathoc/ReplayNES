// "Reset Project": engine reset through the Swift wrapper (saved project reopens empty, A/B kept)
// and the backup copy made before it (naming, copy moved away; a fake trash keeps the real Trash
// untouched).
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class ProjectResetTests: XCTestCase {
    private var tmp: URL!

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-reset-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws { try? FileManager.default.removeItem(at: tmp) }

    func testResetSavedProjectReopensEmptyKeepingSections() throws {
        let rom = tmp.appendingPathComponent("test.nes")
        try Engine.writeTestROM(to: rom)
        let dir = tmp.appendingPathComponent("p.nesrec")
        let s = try EngineSession.create(rom: rom, projectDir: dir)
        let power = s.stateHash()
        for i in 0..<400 { try s.step(p1: UInt8(i % 5 == 0 ? RN_BTN_A : 0), p2: 0, events: 0) }
        try s.practiceSetRange(1, a: 100, b: 200)
        _ = try s.addBookmark(name: "x")
        try s.seek(150)
        try s.step(p1: UInt8(RN_BTN_B), p2: 0, events: 0)  // branch
        XCTAssertGreaterThan(s.takes().count, 1)
        try s.reset(keepPracticeSlots: true)
        XCTAssertEqual(s.frame, 0)
        XCTAssertEqual(s.takeLength, 0)
        XCTAssertTrue(s.takes().isEmpty)
        XCTAssertTrue(s.bookmarks().isEmpty)
        XCTAssertEqual(s.undoDepth, 0)
        XCTAssertEqual(s.mode, RN_MODE_RECORD)
        XCTAssertFalse(s.hasUnsavedChanges)
        XCTAssertEqual(s.stateHash(), power)
        XCTAssertTrue(s.practiceSlot(1).hasA)
        XCTAssertFalse(s.practiceSlot(1).hasTakeFrame)
        let r = try EngineSession.open(projectDir: dir, romOverride: nil, dropCorruptStates: false)
        XCTAssertEqual(r.takeLength, 0)
        XCTAssertTrue(r.bookmarks().isEmpty)
        XCTAssertTrue(r.practiceSlot(1).hasA)
        try s.reset(keepPracticeSlots: false)
        XCTAssertFalse(s.practiceSlot(1).hasA)
    }

    func testBackupNameNextToProject() {
        let p = URL(fileURLWithPath: "/x/Games/Mario.nesrec")
        var c = DateComponents()
        c.year = 2026; c.month = 10; c.day = 6; c.hour = 20; c.minute = 5
        let d = Calendar.current.date(from: c)!
        let first = ProjectBackup.backupURL(for: p, date: d) { _ in false }
        XCTAssertEqual(first.deletingLastPathComponent().path, "/x/Games")
        XCTAssertEqual(first.pathExtension, "nesrec")
        XCTAssertTrue(first.lastPathComponent.hasPrefix("Mario"))
        XCTAssertTrue(first.lastPathComponent.contains("2026-10-06 20.05"))
        let taken = ProjectBackup.backupURL(for: p, date: d) { $0 == first }
        XCTAssertNotEqual(taken, first)
        XCTAssertEqual(taken.lastPathComponent, first.deletingPathExtension().lastPathComponent + " 2.nesrec")
    }

    func testBackupCopiesProjectAndMovesTheCopyAway() throws {
        let project = tmp.appendingPathComponent("Game.nesrec")
        try FileManager.default.createDirectory(at: project.appendingPathComponent("timeline"), withIntermediateDirectories: true)
        try Data("m".utf8).write(to: project.appendingPathComponent("manifest.json"))
        try Data("i".utf8).write(to: project.appendingPathComponent("timeline/index.json"))
        let trash = tmp.appendingPathComponent("FakeTrash")
        try FileManager.default.createDirectory(at: trash, withIntermediateDirectories: true)
        let moved = try ProjectBackup.makeBackup(of: project) { url in
            let dest = trash.appendingPathComponent(url.lastPathComponent)
            try FileManager.default.moveItem(at: url, to: dest)
            return dest
        }
        let dest = try XCTUnwrap(moved)
        XCTAssertEqual(try String(contentsOf: dest.appendingPathComponent("timeline/index.json"), encoding: .utf8), "i")
        XCTAssertTrue(FileManager.default.fileExists(atPath: project.appendingPathComponent("manifest.json").path), "original untouched")
        let left = try FileManager.default.contentsOfDirectory(atPath: tmp.path).filter { $0.contains("Before") }
        XCTAssertTrue(left.isEmpty, "no copy left next to the project")
        // A failing trash leaves nothing behind and is reported (the caller then does not reset).
        struct Refused: Error {}
        XCTAssertThrowsError(try ProjectBackup.makeBackup(of: project) { _ in throw Refused() })
        let after = try FileManager.default.contentsOfDirectory(atPath: tmp.path)
        XCTAssertEqual(Set(after), ["Game.nesrec", "FakeTrash"])
    }
}
