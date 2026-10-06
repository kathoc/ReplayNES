// Backup before "Reset Project": a copy of the saved project folder is moved to the Trash, so the
// previous recording can be recovered from there ("Put Back" restores it next to the project).
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum ProjectBackup {
    /// "<name> (Before Reset 2026-10-06 20.15).nesrec" next to `project`; " 2", " 3" ... if taken
    /// (naming by the shared frontend core, rnf_backup_path).
    static func backupURL(for project: URL, date: Date = Date(), exists: (URL) -> Bool) -> URL {
        withoutActuallyEscaping(exists) { exists in
            var check = exists
            let path = withUnsafeMutablePointer(to: &check) { ctx in
                rnfString(rnf_backup_path(project.path, date.timeIntervalSince1970, { p, ctx in
                    let fn = ctx!.assumingMemoryBound(to: ((URL) -> Bool).self).pointee
                    return fn(URL(fileURLWithPath: String(cString: p!), isDirectory: true)) ? 1 : 0
                }, ctx))
            }
            return URL(fileURLWithPath: path, isDirectory: true)
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
