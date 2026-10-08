// ROM library in ~/Documents/ReplayNES:
//   ROM/       the user's .nes files (top level and one level of sub-folders)
//   Projects/  projects started from the library, "<ROM name> <yyyy-MM-dd HHmm>.nesrec"
// Projects are matched to ROMs by the ROM SHA-256 stored in their manifest.json, not by name.
// Scanning, matching and naming live in the shared frontend core (frontend/src/library.cpp);
// this file adds the macOS folder, Finder-style name collation and Foundation error texts.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct LibraryPaths: Equatable {
    let root: URL
    var roms: URL { root.appendingPathComponent(RNF_LIBRARY_ROM_DIR, isDirectory: true) }
    var projects: URL { root.appendingPathComponent(RNF_LIBRARY_PROJECTS_DIR, isDirectory: true) }

    /// ~/Documents/ReplayNES
    static var standard: LibraryPaths {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Documents")
        return LibraryPaths(root: docs.appendingPathComponent("ReplayNES", isDirectory: true))
    }

    /// Creates ROM/ and Projects/ if missing. Throws a user-readable error otherwise (no fallback).
    func ensure() throws {
        var failed: UnsafeMutablePointer<CChar>?
        let st = rnf_library_ensure(root.path, &failed)
        let path = rnfString(failed)
        switch st {
        case RN_OK: return
        case RN_ERR_ALREADY_EXISTS: throw LibraryError.notADirectory(path)
        default:
            // The system's own wording for why the folder could not be created.
            let message = String(cString: rnf_last_error())
            do {
                try FileManager.default.createDirectory(at: URL(fileURLWithPath: path, isDirectory: true),
                                                        withIntermediateDirectories: true)
            } catch {
                throw LibraryError.cannotCreate(path, (error as NSError).localizedDescription)
            }
            if rnf_library_ensure(root.path, nil) == RN_OK { return }  // created meanwhile
            throw LibraryError.cannotCreate(path, message)
        }
    }
}

enum LibraryError: Error, LocalizedError, Equatable {
    case notADirectory(String)
    case cannotCreate(String, String)
    case cannotRead(String, String)
    var errorDescription: String? {
        switch self {
        case .notADirectory(let p): return String(localized: "A file with the same name is where the folder should be: \(p)")
        case .cannotCreate(let p, let m): return String(localized: "Can’t create the folder: \(p)\n\(m)")
        case .cannotRead(let p, let m): return String(localized: "Can’t read the folder: \(p)\n\(m)")
        }
    }

    /// The folder could not be listed: the system's wording for why.
    static func unreadable(_ dir: URL) -> LibraryError {
        let fallback = String(cString: rnf_last_error())
        do {
            _ = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])
        } catch {
            return .cannotRead(dir.path, (error as NSError).localizedDescription)
        }
        return .cannotRead(dir.path, fallback)
    }
}

struct LibraryROM: Identifiable, Hashable {
    let url: URL
    let name: String          // file name without extension
    let relativePath: String  // relative to ROM/ ("Sub/Game.nes" for sub-folders)
    let size: Int64
    let modified: Date
    var sha256: String?       // nil until hashed (or unreadable)
    var game: GameInfo?       // the game database entry (nil: unknown ROM)
    var id: String { url.path }

    /// Display strings in the UI language (the core's game database; the file name when unknown).
    var title: String { game.map { $0.title(fileName: name) } ?? GameInfo.title(fileName: name) }
    var byline: String { game?.byline ?? "" }    // "Konami · 1986"
    var details: String { game?.details ?? "" }  // "Konami · 1986 · Shooter"
}

/// A game of the core's database (frontend/data/nesdb.tsv; its strings are static C data).
struct GameInfo: Hashable {
    let info: rnf_game_info
    var id: String { String(cString: info.id) }
    var year: Int { Int(info.year) }

    static func == (a: GameInfo, b: GameInfo) -> Bool { a.id == b.id }
    func hash(into h: inout Hasher) { h.combine(id) }

    func title(fileName: String) -> String { withUnsafePointer(to: info) { rnfString(rnf_game_title($0, fileName)) } }
    static func title(fileName: String) -> String { rnfString(rnf_game_title(nil, fileName)) }
    var byline: String { withUnsafePointer(to: info) { rnfString(rnf_game_byline($0)) } }
    var details: String { withUnsafePointer(to: info) { rnfString(rnf_game_details($0)) } }

    static func find(id: String) -> GameInfo? {
        var i = rnf_game_info()
        return rnf_gamedb_find_id(id, &i) != 0 ? GameInfo(info: i) : nil
    }
    static func find(fileName: String) -> GameInfo? {
        var i = rnf_game_info()
        return rnf_gamedb_find_name(fileName, &i) != 0 ? GameInfo(info: i) : nil
    }
    static func find(crc32: UInt32) -> GameInfo? {
        var i = rnf_game_info()
        return rnf_gamedb_find_crc32(crc32, &i) != 0 ? GameInfo(info: i) : nil
    }
}

struct LibraryProject: Identifiable, Hashable {
    let url: URL
    let name: String          // without .nesrec
    let romSHA256: String
    let romName: String
    let modified: Date
    var id: String { url.path }
}

enum LibraryScanner {
    /// Finder order (localizedStandardCompare) for ROM names.
    private static let finderOrder: rnf_name_compare = { a, b, _ in
        switch String(cString: a!).localizedStandardCompare(String(cString: b!)) {
        case .orderedAscending: return -1
        case .orderedSame: return 0
        case .orderedDescending: return 1
        }
    }

    /// .nes files (case-insensitive) in `dir` and its immediate sub-folders, sorted by name.
    static func scanROMs(in dir: URL) throws -> [LibraryROM] {
        var l: OpaquePointer?
        guard rnf_library_scan_roms(dir.path, finderOrder, nil, &l) == RN_OK, let l else { throw LibraryError.unreadable(dir) }
        defer { rnf_rom_list_free(l) }
        return (0..<rnf_rom_list_count(l)).compactMap { i in
            var e = rnf_rom_entry()
            guard rnf_rom_list_get(l, i, &e) != 0 else { return nil }
            return LibraryROM(url: URL(fileURLWithPath: String(cString: e.path), isDirectory: false), name: String(cString: e.name),
                              relativePath: String(cString: e.relative_path), size: e.size,
                              modified: Date(timeIntervalSince1970: e.modified), sha256: nil)
        }
    }

    /// *.nesrec packages directly in `dir` with a readable manifest, newest first.
    static func scanProjects(in dir: URL) throws -> [LibraryProject] {
        var l: OpaquePointer?
        guard rnf_library_scan_projects(dir.path, &l) == RN_OK, let l else { throw LibraryError.unreadable(dir) }
        defer { rnf_project_list_free(l) }
        return (0..<rnf_project_list_count(l)).compactMap { i in
            var e = rnf_project_entry()
            guard rnf_project_list_get(l, i, &e) != 0 else { return nil }
            return LibraryProject(url: URL(fileURLWithPath: String(cString: e.path), isDirectory: true),
                                  name: String(cString: e.name), romSHA256: String(cString: e.rom_sha256),
                                  romName: String(cString: e.rom_name), modified: Date(timeIntervalSince1970: e.modified))
        }
    }

    /// File-name-safe ROM name (no path separators / colons / control characters, trimmed).
    static func sanitize(_ name: String) -> String { rnfString(rnf_library_sanitize(name)) }

    /// "yyyy-MM-dd HHmm" in local time.
    static func timestamp(_ date: Date) -> String { rnfString(rnf_library_timestamp(date.timeIntervalSince1970)) }

    /// Projects/<ROM name> <yyyy-MM-dd HHmm>.nesrec, with " 2", " 3", ... appended if taken.
    static func newProjectURL(projectsDir: URL, romName: String, date: Date) -> URL {
        URL(fileURLWithPath: rnfString(rnf_library_new_project_path(projectsDir.path, romName, date.timeIntervalSince1970)),
            isDirectory: true)
    }
}

/// SHA-256 of ROM files, cached by path + size + modification date. Thread-safe.
final class ROMHashCache {
    private let handle: OpaquePointer = rnf_rom_hash_cache_new()!

    deinit { rnf_rom_hash_cache_free(handle) }

    /// The game database entry of a ROM (by its PRG+CHR hashes, then its file name).
    func game(of rom: LibraryROM) -> GameInfo? {
        var i = rnf_game_info()
        let m = rnf_rom_hash_cache_identify(handle, rom.url.path, rom.size, rom.modified.timeIntervalSince1970, &i)
        return m == RNF_GAME_MATCH_NONE ? nil : GameInfo(info: i)
    }

    func sha256(of rom: LibraryROM) -> String? {
        var hex = [CChar](repeating: 0, count: 65)
        guard rnf_rom_hash_cache_sha256(handle, rom.url.path, rom.size, rom.modified.timeIntervalSince1970, &hex) == RN_OK
        else { return nil }
        return String(cString: hex)
    }
}

/// Favourites, play history and the chosen order of the library (the core's library catalog,
/// persisted as library.json next to the session folder). ROMs are keyed by SHA-256. Main thread.
final class LibraryCatalog {
    private let handle: OpaquePointer = rnf_library_prefs_new()!
    private(set) var file: URL?

    deinit { rnf_library_prefs_free(handle) }

    /// Loads `url` (a missing file = empty); later changes are saved there.
    func load(_ url: URL) {
        file = url
        if rnf_library_prefs_load(handle, url.path) != RN_OK { NSLog("ReplayNES: library prefs: \(String(cString: rnf_last_error()))") }
    }

    private func save() {
        guard let file else { return }
        if rnf_library_prefs_save(handle, file.path) != RN_OK { NSLog("ReplayNES: library prefs: \(String(cString: rnf_last_error()))") }
    }

    func isFavorite(_ sha: String?) -> Bool { sha.map { rnf_library_prefs_is_favorite(handle, $0) != 0 } ?? false }

    /// Returns the new state (false for a ROM without a hash).
    @discardableResult
    func toggleFavorite(_ sha: String?) -> Bool {
        guard let sha, !sha.isEmpty else { return false }
        let on = rnf_library_prefs_toggle_favorite(handle, sha) != 0
        save()
        return on
    }

    /// A session of the ROM started at `start`; adds `seconds` of play time.
    func recordPlay(_ sha: String, start: Date, seconds: Double) {
        guard !sha.isEmpty else { return }
        rnf_library_prefs_record_play(handle, sha, start.timeIntervalSince1970, seconds)
        save()
    }

    struct History: Equatable { let lastPlayed: Date; let playSeconds: Double; let plays: Int }
    func history(_ sha: String?) -> History? {
        guard let sha else { return nil }
        var e = rnf_library_history_entry()
        guard rnf_library_prefs_history_find(handle, sha, &e) != 0 else { return nil }
        return History(lastPlayed: Date(timeIntervalSince1970: e.last_played), playSeconds: e.play_seconds, plays: Int(e.plays))
    }

    var sort: rnf_library_sort {
        get { rnf_library_prefs_sort(handle) }
        set { rnf_library_prefs_set_sort(handle, newValue); save() }
    }
    var filter: rnf_library_filter {
        get { rnf_library_prefs_filter(handle) }
        set { rnf_library_prefs_set_filter(handle, newValue); save() }
    }

    static func name(_ s: rnf_library_sort) -> String { String(cString: rnf_library_sort_name(s)) }
    static func name(_ f: rnf_library_filter) -> String { String(cString: rnf_library_filter_name(f)) }
    static let sorts: [rnf_library_sort] = (0..<RNF_LIBRARY_SORT_COUNT.rawValue).map { rnf_library_sort(rawValue: $0) }
    static let filters: [rnf_library_filter] = (0..<RNF_LIBRARY_FILTER_COUNT.rawValue).map { rnf_library_filter(rawValue: $0) }

    /// The ROMs to show, in order: the filter, the search query, the sort (UI language).
    func arrange(_ roms: [LibraryROM], query: String) -> [LibraryROM] {
        var keep: [UnsafeMutablePointer<CChar>] = []
        defer { keep.forEach { free($0) } }
        func c(_ s: String) -> UnsafePointer<CChar> {
            let p = strdup(s)!
            keep.append(p)
            return UnsafePointer(p)
        }
        let infos = UnsafeMutablePointer<rnf_game_info>.allocate(capacity: max(1, roms.count))
        defer { infos.deallocate() }
        var items: [rnf_library_item] = []
        items.reserveCapacity(roms.count)
        for (i, r) in roms.enumerated() {
            var item = rnf_library_item(key: c(r.sha256 ?? ""), file_name: c(r.name), relative_path: c(r.relativePath), game: nil)
            if let g = r.game {
                (infos + i).initialize(to: g.info)
                item.game = UnsafePointer(infos + i)
            }
            items.append(item)
        }
        let n = rnf_library_arrange(items, items.count, sort, filter, query, handle, nil, 0)
        var order = [Int](repeating: 0, count: n)
        _ = order.withUnsafeMutableBufferPointer { b in
            rnf_library_arrange(items, items.count, sort, filter, query, handle, b.baseAddress, n)
        }
        return order.map { roms[$0] }
    }
}
