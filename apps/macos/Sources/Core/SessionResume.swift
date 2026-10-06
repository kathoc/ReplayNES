// Always-on session persistence (UI-free part, unit tested): where the temporary project of a
// session without a project lives, the resume record, the single-instance lock and the
// launch-time resume decision.
//
//   ~/Library/Application Support/ReplayNES/Session/
//     current.nesrec   temporary project of a session that has no project yet (quick play / --rom).
//                      A normal .nesrec written by the engine (full save + autosave journal), so a
//                      crash or force quit is recovered like any project.
//     resume.json      what to reopen at the next launch (atomic write, updated ~1/s and at quit)
//     .lock            flock()ed by the instance that owns this folder (a second instance neither
//                      resumes nor touches the temporary project)
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct SessionPaths: Equatable {
    let root: URL
    var tempProject: URL { root.appendingPathComponent("current.nesrec", isDirectory: true) }
    var resumeFile: URL { root.appendingPathComponent("resume.json") }
    var lockFile: URL { root.appendingPathComponent(".lock") }

    /// ~/Library/Application Support/ReplayNES/Session
    static var standard: SessionPaths {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return SessionPaths(root: base.appendingPathComponent("ReplayNES/Session", isDirectory: true))
    }

    func ensure() throws { try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true) }

    /// True if `path` (an engine project dir) is the temporary project.
    func isTempProject(_ path: String) -> Bool {
        guard !path.isEmpty else { return false }
        return Self.normalized(URL(fileURLWithPath: path)) == Self.normalized(tempProject)
    }

    var tempProjectExists: Bool { FileManager.default.fileExists(atPath: tempProject.path) }

    private static func normalized(_ u: URL) -> String { u.resolvingSymlinksInPath().standardizedFileURL.path }
}

/// What was open when the app last ran. Written while running, so it survives a force quit.
struct ResumeRecord: Codable, Equatable {
    enum Mode: String, Codable { case record, replay }

    static let currentVersion = 1
    var version = ResumeRecord.currentVersion
    var projectPath: String
    var isTemp: Bool
    var romPath: String = ""
    var romSHA256: String = ""
    /// Take position; nil = keep the project's own cursor.
    var frame: UInt64?
    /// The position was the take end (e.g. recording): resume at the end of what the project
    /// holds, even if the journal got further than this record.
    var atTakeEnd: Bool?
    /// Take mode; nil = keep the project's own (e.g. while practicing).
    var mode: Mode?
    /// Practice slot being looped (-1 = free practice); non-nil = the practice panel is reopened.
    var practiceSlot: Int?
    /// Anything recorded (take frames, bookmarks, A/B slots): an empty temp session is discarded silently.
    var hasContent = true
    var updated = Date()

    /// Equal apart from the timestamp (used to skip redundant writes).
    func sameState(as o: ResumeRecord) -> Bool {
        var a = self, b = o
        a.updated = .distantPast
        b.updated = .distantPast
        return a == b
    }
}

enum ResumeStore {
    /// nil if there is no record. Throws if it exists but cannot be read.
    static func read(_ url: URL) throws -> ResumeRecord? {
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        let data = try Data(contentsOf: url)
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        let r = try dec.decode(ResumeRecord.self, from: data)
        guard r.version <= ResumeRecord.currentVersion, !r.projectPath.isEmpty else {
            throw CocoaError(.fileReadCorruptFile)
        }
        return r
    }

    /// Atomic (temp file + rename): a crash never leaves a half-written record.
    static func write(_ r: ResumeRecord, to url: URL) throws {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        enc.dateEncodingStrategy = .iso8601
        try enc.encode(r).write(to: url, options: .atomic)
    }

    static func clear(_ url: URL) { try? FileManager.default.removeItem(at: url) }
}

/// Exclusive, non-blocking flock() held for the life of the object (released by the kernel if the
/// process dies, so a force quit never leaves a stale lock).
final class SessionLock {
    private let fd: Int32

    init?(url: URL) {
        let fd = open(url.path, O_CREAT | O_RDWR | O_CLOEXEC, 0o644)
        guard fd >= 0 else { return nil }
        if flock(fd, LOCK_EX | LOCK_NB) != 0 {
            close(fd)
            return nil
        }
        self.fd = fd
    }

    deinit {
        flock(fd, LOCK_UN)
        close(fd)
    }
}

enum ResumeDecision: Equatable {
    case none
    case resume(ResumeRecord)
    /// The recorded (non-temporary) project is gone: tell the user, nothing to reopen.
    case projectMissing(ResumeRecord)
}

enum SessionResume {
    /// Launch-time decision. `explicitOpen`: the launch opens something itself (--rom, --project,
    /// a document): no resume. `legacyProjectPath`: crash marker of versions before resume.json.
    static func decide(paths: SessionPaths, explicitOpen: Bool, legacyProjectPath: String = "") -> ResumeDecision {
        if explicitOpen { return .none }
        let fm = FileManager.default
        var record: ResumeRecord?
        do { record = try ResumeStore.read(paths.resumeFile) } catch { record = nil }
        if let r = record {
            if r.isTemp {
                // Always the temporary project of this folder (never a path from the record).
                guard paths.tempProjectExists else { return .none }
                var t = r
                t.projectPath = paths.tempProject.path
                return .resume(t)
            }
            return fm.fileExists(atPath: r.projectPath) ? .resume(r) : .projectMissing(r)
        }
        // No (or unreadable) record: a temporary project left behind is still resumed, at its own cursor.
        if paths.tempProjectExists {
            return .resume(ResumeRecord(projectPath: paths.tempProject.path, isTemp: true))
        }
        if !legacyProjectPath.isEmpty, fm.fileExists(atPath: legacyProjectPath) {
            return .resume(ResumeRecord(projectPath: legacyProjectPath, isTemp: false))
        }
        return .none
    }

    /// Take position to go to: the recorded frame clamped to what the project holds (a force quit
    /// loses the frames after the last autosave).
    static func targetFrame(_ r: ResumeRecord, takeLength: UInt64) -> UInt64? {
        if r.atTakeEnd == true { return takeLength }
        return r.frame.map { min($0, takeLength) }
    }

    /// Restores take mode and position on a freshly opened session (no-op when already there).
    static func apply(_ r: ResumeRecord, to s: EngineSession) throws {
        if let m = r.mode, s.mode != RN_MODE_PRACTICE {
            let want = m == .record ? RN_MODE_RECORD : RN_MODE_REPLAY
            if s.mode != want { try s.setMode(want) }
        }
        if let f = targetFrame(r, takeLength: s.takeLength), f != s.frame { try s.seek(f) }
    }

    /// Something worth asking about before the temporary project is discarded.
    static func hasRecordedContent(_ s: EngineSession) -> Bool {
        s.takes().contains { $0.length > 0 } || !s.bookmarks().isEmpty || s.practiceSlots().contains { $0.hasA }
    }

    /// Moves a (fully saved, closed) temporary project to `dest`, which becomes a normal project.
    static func moveTempProject(_ paths: SessionPaths, to dest: URL) throws {
        try FileManager.default.moveItem(at: paths.tempProject, to: dest)
    }
}
