// Headless tests for always-on session persistence (SessionResume.swift): temporary project,
// resume record, single-instance lock, launch decision and the force-quit / clean-quit resume
// paths with the real engine (a dropped session = the process was killed: nothing but the
// autosave journal reached the disk).
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class SessionResumeTests: XCTestCase {
    private var tmp: URL!
    private var paths: SessionPaths!
    private var rom: URL!

    override func setUpWithError() throws {
        tmp = FileManager.default.temporaryDirectory.appendingPathComponent("rn-resume-\(UUID().uuidString)")
        paths = SessionPaths(root: tmp.appendingPathComponent("Session"))
        try paths.ensure()
        rom = tmp.appendingPathComponent("Test ROM.nes")
        try Engine.writeTestROM(to: rom)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tmp)
    }

    /// Records `frames` frames (varying input) on a new temporary project; returns the state hash
    /// after each frame (index f = state at frame f+1).
    private func recordTemp(frames: Int) throws -> (EngineSession, [UInt64]) {
        let s = try EngineSession.create(rom: rom, projectDir: paths.tempProject)
        try s.setMode(RN_MODE_RECORD)
        var hashes: [UInt64] = []
        for f in 0..<frames {
            try s.step(p1: UInt8(f % 11 == 0 ? RN_BTN_A : 0) | UInt8(f % 5 == 0 ? RN_BTN_RIGHT : 0), p2: 0, events: 0)
            hashes.append(s.stateHash())
        }
        return (s, hashes)
    }

    func testPathsAndTempDetection() {
        XCTAssertEqual(paths.tempProject.lastPathComponent, "current.nesrec")
        XCTAssertEqual(paths.resumeFile.lastPathComponent, "resume.json")
        XCTAssertTrue(paths.isTempProject(paths.tempProject.path))
        XCTAssertTrue(paths.isTempProject(paths.tempProject.path + "/"))
        XCTAssertFalse(paths.isTempProject(""))
        XCTAssertFalse(paths.isTempProject(tmp.appendingPathComponent("Other.nesrec").path))
        XCTAssertTrue(SessionPaths.standard.root.path.hasSuffix("Application Support/ReplayNES/Session"))
    }

    func testRecordRoundTripIsAtomicAndCorruptIsRejected() throws {
        var r = ResumeRecord(projectPath: "/x/y.nesrec", isTemp: false, romPath: "/r.nes", romSHA256: "ab", frame: 42,
                             mode: .replay, practiceSlot: 3, hasContent: true)
        r.updated = Date(timeIntervalSince1970: 1_800_000_000)
        try ResumeStore.write(r, to: paths.resumeFile)
        XCTAssertEqual(try ResumeStore.read(paths.resumeFile), r)
        var later = r
        later.updated = Date()
        XCTAssertTrue(later.sameState(as: r))
        later.frame = 43
        XCTAssertFalse(later.sameState(as: r))

        try Data("{ not json".utf8).write(to: paths.resumeFile)
        XCTAssertThrowsError(try ResumeStore.read(paths.resumeFile))
        ResumeStore.clear(paths.resumeFile)
        XCTAssertNil(try ResumeStore.read(paths.resumeFile))
    }

    func testDecide() throws {
        // Nothing there.
        XCTAssertEqual(SessionResume.decide(paths: paths, explicitOpen: false), .none)

        // A project record: resumed while the project exists, reported when it is gone.
        let project = tmp.appendingPathComponent("P.nesrec")
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        // Whole seconds: the record stores ISO 8601 timestamps.
        let r = ResumeRecord(projectPath: project.path, isTemp: false, frame: 10, mode: .record,
                             updated: Date(timeIntervalSince1970: 1_800_000_000))
        try ResumeStore.write(r, to: paths.resumeFile)
        XCTAssertEqual(SessionResume.decide(paths: paths, explicitOpen: false), .resume(r))
        XCTAssertEqual(SessionResume.decide(paths: paths, explicitOpen: true), .none, "--rom / --project / documents win")
        try FileManager.default.removeItem(at: project)
        XCTAssertEqual(SessionResume.decide(paths: paths, explicitOpen: false), .projectMissing(r))

        // A temp record always means this folder's temporary project.
        try FileManager.default.createDirectory(at: paths.tempProject, withIntermediateDirectories: true)
        let t = ResumeRecord(projectPath: "/elsewhere/current.nesrec", isTemp: true, frame: 5)
        try ResumeStore.write(t, to: paths.resumeFile)
        guard case .resume(let got) = SessionResume.decide(paths: paths, explicitOpen: false) else { return XCTFail() }
        XCTAssertEqual(got.projectPath, paths.tempProject.path)
        XCTAssertEqual(got.frame, 5)

        // Unreadable record but a temporary project left: still resumed (at its own cursor), never dropped.
        try Data("garbage".utf8).write(to: paths.resumeFile)
        guard case .resume(let fallback) = SessionResume.decide(paths: paths, explicitOpen: false) else { return XCTFail() }
        XCTAssertTrue(fallback.isTemp)
        XCTAssertEqual(fallback.projectPath, paths.tempProject.path)
        XCTAssertNil(fallback.frame)

        // Legacy crash marker (pre-resume.json versions).
        try FileManager.default.removeItem(at: paths.tempProject)
        ResumeStore.clear(paths.resumeFile)
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        guard case .resume(let legacy) = SessionResume.decide(paths: paths, explicitOpen: false, legacyProjectPath: project.path)
        else { return XCTFail() }
        XCTAssertEqual(legacy.projectPath, project.path)
        XCTAssertNil(legacy.frame)
        XCTAssertFalse(legacy.isTemp)
    }

    func testLockIsExclusiveAndReleasedWithOwner() throws {
        var first = SessionLock(url: paths.lockFile)
        XCTAssertNotNil(first)
        XCTAssertNil(SessionLock(url: paths.lockFile), "a second instance must not own the session folder")
        first = nil
        XCTAssertNotNil(SessionLock(url: paths.lockFile), "released with its owner (or the process)")
    }

    func testTargetFrameClamps() {
        let r = ResumeRecord(projectPath: "/p", isTemp: true, frame: 500)
        XCTAssertEqual(SessionResume.targetFrame(r, takeLength: 300), 300)
        XCTAssertEqual(SessionResume.targetFrame(r, takeLength: 900), 500)
        XCTAssertNil(SessionResume.targetFrame(ResumeRecord(projectPath: "/p", isTemp: true), takeLength: 9))
        // Recording at the take end: the end of what was persisted, even past the recorded frame.
        let end = ResumeRecord(projectPath: "/p", isTemp: true, frame: 716, atTakeEnd: true, mode: .record)
        XCTAssertEqual(SessionResume.targetFrame(end, takeLength: 727), 727)
        XCTAssertEqual(SessionResume.targetFrame(end, takeLength: 600), 600)
    }

    /// Force quit while recording: the take survives up to the last autosave and the session
    /// resumes there (resume.json may be newer than the journal: clamped).
    func testForceQuitResumesNearLastAutosave() throws {
        var hashes: [UInt64] = []
        do {
            let (s, h) = try recordTemp(frames: 300)
            hashes = h
            XCTAssertTrue(SessionResume.hasRecordedContent(s))
            // Autosave happened at frame 300 ...
            try s.autosave()
            // ... 60 more frames were recorded, and the resume timer saw frame 360, then SIGKILL.
            for _ in 0..<60 { try s.step(p1: 0, p2: 0, events: 0) }
            try ResumeStore.write(ResumeRecord(projectPath: s.projectDir, isTemp: true, romPath: s.romPath,
                                               romSHA256: s.romSHA256, frame: s.frame, mode: .record), to: paths.resumeFile)
        } // session dropped without saving = killed

        guard case .resume(let r) = SessionResume.decide(paths: paths, explicitOpen: false) else { return XCTFail("no resume") }
        XCTAssertTrue(r.isTemp)
        let s = try EngineSession.open(projectDir: URL(fileURLWithPath: r.projectPath), romOverride: nil, dropCorruptStates: false)
        XCTAssertTrue(s.recovered, "journal replayed")
        XCTAssertEqual(s.takeLength, 300, "everything up to the last autosave is kept")
        try SessionResume.apply(r, to: s)
        XCTAssertEqual(s.frame, 300)
        XCTAssertEqual(s.mode, RN_MODE_RECORD)
        XCTAssertEqual(s.stateHash(), hashes[299], "same machine state as when it was recorded")
        // Earlier positions replay identically too.
        try s.seek(120)
        XCTAssertEqual(s.stateHash(), hashes[119])
    }

    /// Clean quit: full save of the temporary project; resumes at the exact take position and mode.
    func testCleanQuitResumesExactPositionAndMode() throws {
        var hashes: [UInt64] = []
        do {
            let (s, h) = try recordTemp(frames: 240)
            hashes = h
            try s.seek(150)
            try s.setMode(RN_MODE_REPLAY)
            try s.save() // what prepareForQuit does for a temporary session
            try ResumeStore.write(ResumeRecord(projectPath: s.projectDir, isTemp: true, frame: 150, mode: .replay,
                                               practiceSlot: -1), to: paths.resumeFile)
        }
        guard case .resume(let r) = SessionResume.decide(paths: paths, explicitOpen: false) else { return XCTFail() }
        XCTAssertEqual(r.practiceSlot, -1, "practice panel is reopened")
        let s = try EngineSession.open(projectDir: paths.tempProject, romOverride: nil, dropCorruptStates: false)
        XCTAssertFalse(s.recovered)
        XCTAssertEqual(s.takeLength, 240)
        XCTAssertEqual(s.frame, 150)
        XCTAssertEqual(s.mode, RN_MODE_REPLAY)
        try SessionResume.apply(r, to: s)
        XCTAssertFalse(s.hasUnsavedChanges, "resuming where it already is changes nothing")
        XCTAssertEqual(s.stateHash(), hashes[149])

        // A record newer than the project (frame / mode changed after the last save) is applied.
        try SessionResume.apply(ResumeRecord(projectPath: r.projectPath, isTemp: true, frame: 60, mode: .record), to: s)
        XCTAssertEqual(s.frame, 60)
        XCTAssertEqual(s.mode, RN_MODE_RECORD)
        XCTAssertEqual(s.stateHash(), hashes[59])
    }

    /// 保存… on a temporary session: full save, close, move; it opens as a normal project and the
    /// temporary project is gone.
    func testSaveTempAsMovesProject() throws {
        do {
            let (s, _) = try recordTemp(frames: 90)
            try s.addBookmark(name: "here")
            try s.save()
        }
        let dest = tmp.appendingPathComponent("Saved.nesrec")
        try SessionResume.moveTempProject(paths, to: dest)
        XCTAssertFalse(paths.tempProjectExists)
        let s = try EngineSession.open(projectDir: dest, romOverride: nil, dropCorruptStates: false)
        XCTAssertEqual(s.takeLength, 90)
        XCTAssertEqual(s.bookmarks().map(\.name), ["here"])
        XCTAssertFalse(paths.isTempProject(s.projectDir))
        // The slot is free for the next temporary session.
        XCTAssertNoThrow(try EngineSession.create(rom: rom, projectDir: paths.tempProject))
    }

    func testEmptySessionHasNoContent() throws {
        let s = try EngineSession.create(rom: rom, projectDir: paths.tempProject)
        XCTAssertFalse(SessionResume.hasRecordedContent(s))
    }
}
