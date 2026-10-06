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
    var id: String { url.path }
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

    func sha256(of rom: LibraryROM) -> String? {
        var hex = [CChar](repeating: 0, count: 65)
        guard rnf_rom_hash_cache_sha256(handle, rom.url.path, rom.size, rom.modified.timeIntervalSince1970, &hex) == RN_OK
        else { return nil }
        return String(cString: hex)
    }
}
