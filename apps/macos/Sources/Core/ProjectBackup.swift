// Backup before "Reset Project": a copy of the saved project folder is moved to the Trash, so the
// previous recording can be recovered from there ("Put Back" restores it next to the project).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum ProjectBackup {
    /// "<name> (Before Reset 2026-10-06 20.15).nesrec" next to `project`; " 2", " 3" ... if taken.
    static func backupURL(for project: URL, date: Date = Date(), exists: (URL) -> Bool) -> URL {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH.mm"
        let stamp = f.string(from: date)
        let name = project.deletingPathExtension().lastPathComponent
        let ext = project.pathExtension.isEmpty ? "nesrec" : project.pathExtension
        let dir = project.deletingLastPathComponent()
        let base = String(localized: "\(name) (Before Reset \(stamp))")
        var n = 1
        while true {
            let candidate = dir.appendingPathComponent((n == 1 ? base : "\(base) \(n)") + "." + ext, isDirectory: true)
            if !exists(candidate) { return candidate }
            n += 1
        }
    }

    /// Copies `project` (next to it, or into the temporary folder when its folder is not
    /// writable) and moves the copy to the Trash (`trash`; default FileManager.trashItem, or the
    /// folder given by the `resetBackupFolder` default for scripted checks). Returns where the
    /// copy ended up. Throws if no backup could be made: the caller must not reset then.
    @discardableResult
    static func makeBackup(of project: URL, fileManager fm: FileManager = .default,
                           trash: ((URL) throws -> URL?)? = nil) throws -> URL? {
        var copy = backupURL(for: project) { fm.fileExists(atPath: $0.path) }
        do {
            try fm.copyItem(at: project, to: copy)
        } catch {
            try? fm.removeItem(at: copy)  // a partial copy
            let tmp = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
            try fm.createDirectory(at: tmp, withIntermediateDirectories: true)
            copy = tmp.appendingPathComponent(copy.lastPathComponent, isDirectory: true)
            try fm.copyItem(at: project, to: copy)
        }
        let move = trash ?? defaultTrash(fm)
        do {
            return try move(copy)
        } catch {
            try? fm.removeItem(at: copy)
            throw error
        }
    }

    private static func defaultTrash(_ fm: FileManager) -> (URL) throws -> URL? {
        if let folder = UserDefaults.standard.string(forKey: "resetBackupFolder"), !folder.isEmpty {
            return { url in
                let dest = URL(fileURLWithPath: folder, isDirectory: true).appendingPathComponent(url.lastPathComponent)
                try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
                try fm.moveItem(at: url, to: dest)
                return dest
            }
        }
        return { url in
            var out: NSURL?
            try fm.trashItem(at: url, resultingItemURL: &out)
            return out as URL?
        }
    }
}
