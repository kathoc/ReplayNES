// Always-on session persistence: where the temporary project of a session without a project
// lives, the resume record, the single-instance lock and the launch-time resume decision. The
// record format, the lock and the decision live in the shared frontend core
// (frontend/src/resume.cpp); this file adds the macOS folder and Swift types.
//
//   ~/Library/Application Support/ReplayNES/Session/
//     current.nesrec   temporary project of a session that has no project yet (quick play / --rom)
//     resume.json      what to reopen at the next launch (atomic write, updated ~1/s and at quit)
//     .lock            locked by the instance that owns this folder
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct SessionPaths: Equatable {
    let root: URL
    var tempProject: URL { root.appendingPathComponent(RNF_SESSION_TEMP_PROJECT, isDirectory: true) }
    var resumeFile: URL { root.appendingPathComponent(RNF_SESSION_RESUME_FILE) }
    var lockFile: URL { root.appendingPathComponent(RNF_SESSION_LOCK_FILE) }

    /// ~/Library/Application Support/ReplayNES/Session
    static var standard: SessionPaths {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return SessionPaths(root: base.appendingPathComponent("ReplayNES/Session", isDirectory: true))
    }

    func ensure() throws { try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true) }

    /// True if `path` (an engine project dir) is the temporary project.
    func isTempProject(_ path: String) -> Bool { rnf_paths_equal(path, tempProject.path) != 0 }

    var tempProjectExists: Bool { FileManager.default.fileExists(atPath: tempProject.path) }
}

/// What was open when the app last ran. Written while running, so it survives a force quit.
struct ResumeRecord: Equatable {
    enum Mode: String { case record, replay }

    static let currentVersion = Int(RNF_RESUME_VERSION)
    var version = ResumeRecord.currentVersion
    var projectPath: String
    var isTemp: Bool
    var romPath: String = ""
    var romSHA256: String = ""
    /// Take position; nil = keep the project's own cursor.
    var frame: UInt64?
    /// The position was the take end: resume at the end of what the project holds.
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
        withC { a in o.withC { b in rnf_resume_same_state(a, b) != 0 } }
    }

    /// The record as the core's C struct (strings valid for the call).
    func withC<R>(_ body: (UnsafePointer<rnf_resume_record>) -> R) -> R {
        withCStrings([projectPath, romPath, romSHA256]) { s in
            var r = rnf_resume_record()
            r.version = Int32(version)
            r.project_path = s[0]
            r.is_temp = isTemp ? 1 : 0
            r.rom_path = s[1]
            r.rom_sha256 = s[2]
            if let frame { r.has_frame = 1; r.frame = frame }
            if let atTakeEnd { r.has_at_take_end = 1; r.at_take_end = atTakeEnd ? 1 : 0 }
            r.mode = mode == .record ? RNF_RESUME_MODE_RECORD : mode == .replay ? RNF_RESUME_MODE_REPLAY : RNF_RESUME_MODE_NONE
            if let practiceSlot { r.has_practice_slot = 1; r.practice_slot = Int32(practiceSlot) }
            r.has_content = hasContent ? 1 : 0
            r.updated = updated.timeIntervalSince1970
            return body(&r)
        }
    }

    /// From a record filled by the core (which still owns its strings).
    init(_ r: rnf_resume_record) {
        version = Int(r.version)
        projectPath = r.project_path.map { String(cString: $0) } ?? ""
        isTemp = r.is_temp != 0
        romPath = r.rom_path.map { String(cString: $0) } ?? ""
        romSHA256 = r.rom_sha256.map { String(cString: $0) } ?? ""
        frame = r.has_frame != 0 ? r.frame : nil
        atTakeEnd = r.has_at_take_end != 0 ? r.at_take_end != 0 : nil
        mode = r.mode == RNF_RESUME_MODE_RECORD ? .record : r.mode == RNF_RESUME_MODE_REPLAY ? .replay : nil
        practiceSlot = r.has_practice_slot != 0 ? Int(r.practice_slot) : nil
        hasContent = r.has_content != 0
        updated = Date(timeIntervalSince1970: r.updated)
    }

    init(version: Int = ResumeRecord.currentVersion, projectPath: String, isTemp: Bool, romPath: String = "",
         romSHA256: String = "", frame: UInt64? = nil, atTakeEnd: Bool? = nil, mode: Mode? = nil, practiceSlot: Int? = nil,
         hasContent: Bool = true, updated: Date = Date()) {
        self.version = version
        self.projectPath = projectPath
        self.isTemp = isTemp
        self.romPath = romPath
        self.romSHA256 = romSHA256
        self.frame = frame
        self.atTakeEnd = atTakeEnd
        self.mode = mode
        self.practiceSlot = practiceSlot
        self.hasContent = hasContent
        self.updated = updated
    }
}

enum ResumeStore {
    /// nil if there is no record. Throws if it exists but cannot be read.
    static func read(_ url: URL) throws -> ResumeRecord? {
        var r = rnf_resume_record()
        var found: Int32 = 0
        let st = rnf_resume_read(url.path, &r, &found)
        defer { rnf_resume_record_clear(&r) }
        if st != RN_OK { throw RNError(st, message: String(cString: rnf_last_error())) }
        return found != 0 ? ResumeRecord(r) : nil
    }

    /// Atomic (temp file + rename): a crash never leaves a half-written record.
    static func write(_ r: ResumeRecord, to url: URL) throws {
        let st = r.withC { rnf_resume_write(url.path, $0) }
        if st != RN_OK { throw RNError(st, message: String(cString: rnf_last_error())) }
    }

    static func clear(_ url: URL) { rnf_resume_clear(url.path) }
}

/// Exclusive, non-blocking lock held for the life of the object (released by the kernel if the
/// process dies, so a force quit never leaves a stale lock).
final class SessionLock {
    private let lock: OpaquePointer

    init?(url: URL) {
        guard let l = rnf_session_lock_acquire(url.path) else { return nil }
        lock = l
    }

    deinit { rnf_session_lock_release(lock) }
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
        var r = rnf_resume_record()
        defer { rnf_resume_record_clear(&r) }
        switch rnf_resume_decide(paths.root.path, explicitOpen ? 1 : 0, legacyProjectPath, &r) {
        case RNF_RESUME_RESUME: return .resume(ResumeRecord(r))
        case RNF_RESUME_PROJECT_MISSING: return .projectMissing(ResumeRecord(r))
        default: return .none
        }
    }

    /// Take position to go to: the recorded frame clamped to what the project holds.
    static func targetFrame(_ r: ResumeRecord, takeLength: UInt64) -> UInt64? {
        var f: UInt64 = 0
        return r.withC { rnf_resume_target_frame($0, takeLength, &f) } != 0 ? f : nil
    }

    /// Restores take mode and position on a freshly opened session (no-op when already there).
    static func apply(_ r: ResumeRecord, to s: EngineSession) throws {
        try rnCheck(r.withC { rnf_resume_apply($0, s.handle) })
    }

    /// Something worth asking about before the temporary project is discarded.
    static func hasRecordedContent(_ s: EngineSession) -> Bool { rnf_session_has_recorded_content(s.handle) != 0 }

    /// Moves a (fully saved, closed) temporary project to `dest`, which becomes a normal project.
    static func moveTempProject(_ paths: SessionPaths, to dest: URL) throws {
        try FileManager.default.moveItem(at: paths.tempProject, to: dest)
    }
}
