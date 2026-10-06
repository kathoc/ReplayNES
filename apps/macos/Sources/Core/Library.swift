// ROM library in ~/Documents/ReplayNES (UI-free part, unit tested):
//   ROM/       the user's .nes files (top level and one level of sub-folders)
//   Projects/  projects started from the library, "<ROM name> <yyyy-MM-dd HHmm>.nesrec"
// Projects are matched to ROMs by the ROM SHA-256 stored in their manifest.json, not by name.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct LibraryPaths: Equatable {
    let root: URL
    var roms: URL { root.appendingPathComponent("ROM", isDirectory: true) }
    var projects: URL { root.appendingPathComponent("Projects", isDirectory: true) }

    /// ~/Documents/ReplayNES
    static var standard: LibraryPaths {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Documents")
        return LibraryPaths(root: docs.appendingPathComponent("ReplayNES", isDirectory: true))
    }

    /// Creates ROM/ and Projects/ if missing. Throws a user-readable error otherwise (no fallback).
    func ensure() throws {
        for dir in [roms, projects] {
            var isDir: ObjCBool = false
            if FileManager.default.fileExists(atPath: dir.path, isDirectory: &isDir) {
                if !isDir.boolValue { throw LibraryError.notADirectory(dir.path) }
                continue
            }
            do {
                try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            } catch {
                throw LibraryError.cannotCreate(dir.path, (error as NSError).localizedDescription)
            }
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
    /// .nes files (case-insensitive) in `dir` and its immediate sub-folders, sorted by name.
    static func scanROMs(in dir: URL) throws -> [LibraryROM] {
        let fm = FileManager.default
        let keys: [URLResourceKey] = [.isDirectoryKey, .isRegularFileKey, .fileSizeKey, .contentModificationDateKey]
        func list(_ d: URL) throws -> [URL] {
            do {
                return try fm.contentsOfDirectory(at: d, includingPropertiesForKeys: keys, options: [.skipsHiddenFiles])
            } catch {
                throw LibraryError.cannotRead(d.path, (error as NSError).localizedDescription)
            }
        }
        var out: [LibraryROM] = []
        func add(_ url: URL, relative: String) {
            guard url.pathExtension.lowercased() == "nes" else { return }
            let v = try? url.resourceValues(forKeys: Set(keys))
            guard v?.isRegularFile ?? false else { return }
            out.append(LibraryROM(url: url, name: url.deletingPathExtension().lastPathComponent, relativePath: relative,
                                  size: Int64(v?.fileSize ?? 0), modified: v?.contentModificationDate ?? .distantPast, sha256: nil))
        }
        for url in try list(dir) {
            let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            if isDir {
                // One level of sub-folders; an unreadable sub-folder is skipped, not fatal.
                for sub in (try? list(url)) ?? [] { add(sub, relative: url.lastPathComponent + "/" + sub.lastPathComponent) }
            } else {
                add(url, relative: url.lastPathComponent)
            }
        }
        return out.sorted {
            let c = $0.name.localizedStandardCompare($1.name)
            return c == .orderedSame ? $0.relativePath < $1.relativePath : c == .orderedAscending
        }
    }

    /// *.nesrec packages directly in `dir` with a readable manifest, newest first.
    static func scanProjects(in dir: URL) throws -> [LibraryProject] {
        let urls: [URL]
        do {
            urls = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey],
                                                               options: [.skipsHiddenFiles])
        } catch {
            throw LibraryError.cannotRead(dir.path, (error as NSError).localizedDescription)
        }
        var out: [LibraryProject] = []
        for url in urls where url.pathExtension.lowercased() == "nesrec" {
            guard let manifest = try? Engine.manifestJSON(projectDir: url),
                  let rom = manifest["rom"] as? [String: Any], let sha = rom["sha256"] as? String else { continue }
            // The manifest is rewritten on every save: its date is the project's last save.
            let mdate = (try? url.appendingPathComponent("manifest.json").resourceValues(forKeys: [.contentModificationDateKey]))?
                .contentModificationDate
            let date = mdate ?? (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate ?? .distantPast
            out.append(LibraryProject(url: url, name: url.deletingPathExtension().lastPathComponent, romSHA256: sha.lowercased(),
                                      romName: rom["name"] as? String ?? "", modified: date))
        }
        return out.sorted { $0.modified > $1.modified }
    }

    /// File-name-safe ROM name (no path separators / colons, trimmed).
    static func sanitize(_ name: String) -> String {
        let bad = CharacterSet(charactersIn: "/:\\").union(.controlCharacters)
        let cleaned = name.components(separatedBy: bad).joined(separator: "_").trimmingCharacters(in: .whitespaces)
        let trimmed = cleaned.hasPrefix(".") ? "_" + cleaned.dropFirst() : cleaned
        return trimmed.isEmpty ? "ROM" : trimmed
    }

    static func timestamp(_ date: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.calendar = Calendar(identifier: .gregorian)
        f.dateFormat = "yyyy-MM-dd HHmm"
        return f.string(from: date)
    }

    /// Projects/<ROM name> <yyyy-MM-dd HHmm>.nesrec, with " 2", " 3", ... appended if taken.
    static func newProjectURL(projectsDir: URL, romName: String, date: Date) -> URL {
        let base = sanitize(romName) + " " + timestamp(date)
        var n = 1
        while true {
            let name = n == 1 ? base : "\(base) \(n)"
            let url = projectsDir.appendingPathComponent(name + ".nesrec", isDirectory: true)
            if !FileManager.default.fileExists(atPath: url.path) { return url }
            n += 1
        }
    }
}

/// SHA-256 of ROM files, cached by path + size + modification date. Thread-safe.
final class ROMHashCache {
    private struct Key: Hashable { let path: String; let size: Int64; let modified: Date }
    private var cache: [Key: String] = [:]
    private let lock = NSLock()

    func sha256(of rom: LibraryROM) -> String? {
        let key = Key(path: rom.url.path, size: rom.size, modified: rom.modified)
        lock.lock()
        if let h = cache[key] { lock.unlock(); return h }
        lock.unlock()
        guard let h = try? Engine.sha256(of: rom.url) else { return nil }
        lock.lock(); cache[key] = h.lowercased(); lock.unlock()
        return h.lowercased()
    }
}
