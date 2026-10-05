// Thin Swift wrapper over the engine C API (replaynes.h). No engine semantics live here.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// An engine error with the status code and the thread-local message captured right after the call.
struct RNError: Error, LocalizedError, Equatable {
    let status: rn_status
    let message: String

    init(_ status: rn_status, message: String? = nil) {
        self.status = status
        self.message = message ?? String(cString: rn_last_error())
    }

    var statusName: String { String(cString: rn_status_name(status)) }
    var errorDescription: String? { "\(statusName): \(message)" }
}

@inline(__always)
func rnCheck(_ s: rn_status) throws {
    if s != RN_OK { throw RNError(s) }
}

enum Engine {
    static var version: String { String(cString: rn_version()) }
    static var coreCompatID: String { String(cString: rn_core_compat_id()) }
    static var coreBuildID: String { String(cString: rn_core_build_id()) }

    static func writeTestROM(to url: URL) throws {
        try rnCheck(rn_write_test_rom(url.path))
    }

    static func sha256(of url: URL) throws -> String {
        var buf = [CChar](repeating: 0, count: 65)
        try rnCheck(rn_sha256_file(url.path, &buf))
        return String(cString: buf)
    }

    static func manifestJSON(projectDir: URL) throws -> [String: Any] {
        var out: UnsafeMutablePointer<CChar>?
        try rnCheck(rn_project_manifest_json(projectDir.path, &out))
        guard let out else { return [:] }
        defer { rn_string_free(out) }
        let data = Data(String(cString: out).utf8)
        return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
    }

    /// Exact media time of frame f (NTSC 39375000/655171 Hz).
    static func seconds(forFrame f: UInt64) -> Double {
        Double(f) * Double(RN_FPS_DEN) / Double(RN_FPS_NUM)
    }

    static func timecode(forFrame f: UInt64) -> String {
        let t = seconds(forFrame: f)
        let m = Int(t) / 60
        let s = t - Double(m * 60)
        return String(format: "%02d:%05.2f", m, s)
    }
}

struct TakeInfo: Identifiable, Equatable {
    let id: UInt64
    let parentID: UInt64
    let branchFrame: UInt64
    let length: UInt64
    let createdSeq: UInt64
    let isActive: Bool
    let childCount: UInt32
}

struct BookmarkInfo: Identifiable, Equatable {
    let id: UInt64
    let frame: UInt64
    let takeID: UInt64
    let name: String
    let onActiveTake: Bool
}

/// Owns an rn_session. NOT thread-safe: all calls must come from one thread (the emulation thread).
final class EngineSession {
    let handle: OpaquePointer

    private init(handle: OpaquePointer) { self.handle = handle }
    deinit { rn_session_close(handle) }

    static func defaultOptions(dropCorruptStates: Bool = false) -> rn_session_options {
        var o = rn_session_options()
        rn_session_options_init(&o)
        if dropCorruptStates { o.open_flags |= UInt32(RN_OPEN_DROP_CORRUPT_STATES) }
        return o
    }

    /// New session for a ROM. projectDir == nil -> in-memory (save later with saveAs).
    static func create(rom: URL, projectDir: URL?) throws -> EngineSession {
        var opt = defaultOptions()
        var out: OpaquePointer?
        let st = rn_session_new(rom.path, projectDir?.path, &opt, &out)
        if st != RN_OK { throw RNError(st) }
        return EngineSession(handle: out!)
    }

    static func open(projectDir: URL, romOverride: URL?, dropCorruptStates: Bool) throws -> EngineSession {
        var opt = defaultOptions(dropCorruptStates: dropCorruptStates)
        var out: OpaquePointer?
        let st = rn_session_open(projectDir.path, romOverride?.path, &opt, &out)
        if st != RN_OK { throw RNError(st) }
        return EngineSession(handle: out!)
    }

    // MARK: persistence
    func save() throws { try rnCheck(rn_session_save(handle)) }
    func autosave() throws { try rnCheck(rn_session_autosave(handle)) }
    func saveAs(_ dir: URL) throws { try rnCheck(rn_session_save_as(handle, dir.path)) }
    var recovered: Bool { rn_session_recovered(handle) != 0 }
    var hasUnsavedChanges: Bool { rn_session_has_unsaved_changes(handle) != 0 }
    var romSHA256: String { String(cString: rn_session_rom_sha256(handle)) }
    var romPath: String { String(cString: rn_session_rom_path(handle)) }
    var projectDir: String { String(cString: rn_session_project_dir(handle)) }

    // MARK: play / record
    var mode: rn_mode { rn_get_mode(handle) }
    func setMode(_ m: rn_mode) throws { try rnCheck(rn_set_mode(handle, m)) }

    @discardableResult
    func step(p1: UInt8, p2: UInt8, events: UInt8) throws -> rn_step_info {
        var info = rn_step_info()
        try rnCheck(rn_step(handle, p1, p2, events, &info))
        return info
    }

    var video: UnsafePointer<UInt32>? { rn_video(handle) }
    func audio() -> UnsafeBufferPointer<Int16> {
        var n = 0
        guard let p = rn_audio(handle, &n), n > 0 else { return UnsafeBufferPointer(start: nil, count: 0) }
        return UnsafeBufferPointer(start: p, count: n)
    }

    var frame: UInt64 { rn_frame(handle) }
    var takeLength: UInt64 { rn_take_length(handle) }
    func seek(_ f: UInt64) throws { try rnCheck(rn_seek(handle, f)) }
    func rewind(_ n: UInt64) throws { try rnCheck(rn_rewind(handle, n)) }
    func stateHash() -> UInt64 { rn_state_hash(handle) }
    var videoHash: UInt64 { rn_video_hash(handle) }

    // MARK: bookmarks
    @discardableResult
    func addBookmark(name: String) throws -> UInt64 {
        var id: UInt64 = 0
        try rnCheck(rn_bookmark_add(handle, name, &id))
        return id
    }
    func removeBookmark(_ id: UInt64) throws { try rnCheck(rn_bookmark_remove(handle, id)) }
    func renameBookmark(_ id: UInt64, name: String) throws { try rnCheck(rn_bookmark_rename(handle, id, name)) }
    func gotoBookmark(_ id: UInt64) throws { try rnCheck(rn_bookmark_goto(handle, id)) }
    func bookmarks() -> [BookmarkInfo] {
        let n = rn_bookmark_count(handle)
        var out: [BookmarkInfo] = []
        out.reserveCapacity(n)
        for i in 0..<n {
            var b = rn_bookmark_info()
            guard rn_bookmark_get(handle, i, &b) == RN_OK else { continue }
            out.append(BookmarkInfo(id: b.id, frame: b.frame, takeID: b.take_id,
                                    name: b.name.map { String(cString: $0) } ?? "",
                                    onActiveTake: b.on_active_take != 0))
        }
        return out
    }

    // MARK: takes
    var activeTake: UInt64 { rn_active_take(handle) }
    var undoDepth: Int { Int(rn_undo_depth(handle)) }
    func activateTake(_ id: UInt64) throws { try rnCheck(rn_take_activate(handle, id)) }
    func undoTakeSwitch() throws { try rnCheck(rn_undo_take_switch(handle)) }
    func takes() -> [TakeInfo] {
        let n = rn_take_count(handle)
        var out: [TakeInfo] = []
        for i in 0..<n {
            var t = rn_take_info()
            guard rn_take_get(handle, i, &t) == RN_OK else { continue }
            out.append(TakeInfo(id: t.id, parentID: t.parent_id, branchFrame: t.branch_frame, length: t.length,
                                createdSeq: t.created_seq, isActive: t.is_active != 0, childCount: t.child_count))
        }
        return out
    }

    /// Snapshot of the active take for offline rendering. Must be called on the session's thread;
    /// the returned renderer is independent and may be driven on any thread.
    func makeRenderer(start: UInt64 = 0, end: UInt64 = 0) throws -> OpaquePointer {
        var r: OpaquePointer?
        try rnCheck(rn_renderer_new(handle, start, end, &r))
        return r!
    }
}
