// ROM library state: folder creation at launch, background scanning (ROM hashes, project
// manifests), folder watching (DispatchSource) and refresh on app activation. Main thread API.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Foundation

final class LibraryModel: ObservableObject {
    @Published private(set) var roms: [LibraryROM] = []
    @Published private(set) var projectsBySHA: [String: [LibraryProject]] = [:]
    @Published private(set) var scanning = false
    @Published private(set) var folderError: String?
    @Published private(set) var scanError: String?
    @Published private(set) var lastScan: Date?

    /// Bumped when favourites / history / the order change (views redraw).
    @Published private(set) var catalogRevision = 0

    private(set) var paths = LibraryPaths.standard
    /// Favourites, play history, sort / filter (library.json; see setCatalogFile).
    let catalog = LibraryCatalog()
    private var arrangedKey = ""
    private var arrangedCache: [LibraryROM] = []
    private let queue = DispatchQueue(label: "replaynes.library", qos: .utility)
    private let hashes = ROMHashCache()
    private var watchers: [String: DispatchSourceFileSystemObject] = [:]
    private var pendingRefresh: DispatchWorkItem?
    private var scanGeneration = 0
    private var activationObserver: NSObjectProtocol?
    private var started = false
    private var scanSerial = 0

    /// Test / scripting override (`--library-root <dir>`); must be called before start().
    func setRoot(_ url: URL) { paths = LibraryPaths(root: url) }

    /// Creates the folders (reporting failures; never silently uses another place), starts watching
    /// and scans. Returns the folder error, if any, so the caller can show it.
    @discardableResult
    func start() -> String? {
        if !started {
            started = true
            activationObserver = NotificationCenter.default.addObserver(forName: NSApplication.didBecomeActiveNotification,
                                                                        object: nil, queue: .main) { [weak self] _ in
                self?.refresh()
            }
        }
        ensureFolders()
        refresh()
        return folderError
    }

    @discardableResult
    func ensureFolders() -> Bool {
        do {
            try paths.ensure()
            folderError = nil
            return true
        } catch {
            folderError = (error as? LocalizedError)?.errorDescription ?? "\(error)"
            return false
        }
    }

    // MARK: favourites / history / order

    func setCatalogFile(_ url: URL) {
        catalog.load(url)
        catalogRevision += 1
    }

    func isFavorite(_ rom: LibraryROM) -> Bool { catalog.isFavorite(rom.sha256) }

    @discardableResult
    func toggleFavorite(_ rom: LibraryROM) -> Bool {
        let on = catalog.toggleFavorite(rom.sha256)
        catalogRevision += 1
        return on
    }

    func recordPlay(_ sha: String, start: Date, seconds: Double) {
        catalog.recordPlay(sha, start: start, seconds: seconds)
        catalogRevision += 1
    }

    var sort: rnf_library_sort {
        get { catalog.sort }
        set { catalog.sort = newValue; catalogRevision += 1 }
    }
    var filter: rnf_library_filter {
        get { catalog.filter }
        set { catalog.filter = newValue; catalogRevision += 1 }
    }

    func rom(sha256: String?) -> LibraryROM? {
        guard let sha256, !sha256.isEmpty else { return nil }
        return roms.first { $0.sha256 == sha256 }
    }

    /// The ROMs to show in order (the chosen filter and sort, then the search query); cached.
    func arranged(query: String) -> [LibraryROM] {
        let key = "\(roms.count)/\(roms.first?.id ?? "")/\(scanSerial)/\(catalogRevision)/\(String(cString: rnf_l10n_language()))/\(query)"
        if key != arrangedKey {
            arrangedCache = catalog.arrange(roms, query: query)
            arrangedKey = key
        }
        return arrangedCache
    }

    func projects(for rom: LibraryROM) -> [LibraryProject] {
        guard let sha = rom.sha256 else { return [] }
        return projectsBySHA[sha] ?? []
    }

    /// Rescans in the background (coalesced). Safe to call often.
    func refresh() {
        pendingRefresh?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.scanNow() }
        pendingRefresh = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: work)
    }

    private func scanNow() {
        if folderError != nil { ensureFolders() }
        scanGeneration += 1
        let gen = scanGeneration
        let paths = self.paths, hashes = self.hashes
        scanning = true
        queue.async { [weak self] in
            var roms: [LibraryROM] = []
            var projects: [LibraryProject] = []
            var errors: [String] = []
            do { roms = try LibraryScanner.scanROMs(in: paths.roms) } catch {
                errors.append((error as? LocalizedError)?.errorDescription ?? "\(error)")
            }
            do { projects = try LibraryScanner.scanProjects(in: paths.projects) } catch {
                errors.append((error as? LocalizedError)?.errorDescription ?? "\(error)")
            }
            for i in roms.indices {
                roms[i].sha256 = hashes.sha256(of: roms[i])
                roms[i].game = hashes.game(of: roms[i])
            }
            let grouped = Dictionary(grouping: projects, by: { $0.romSHA256 })
            let subdirs = Set(roms.compactMap { r -> String? in
                let d = r.url.deletingLastPathComponent()
                return d.standardizedFileURL.path == paths.roms.standardizedFileURL.path ? nil : d.path
            })
            DispatchQueue.main.async {
                guard let self, gen == self.scanGeneration else { return }
                if self.roms != roms { self.roms = roms; self.scanSerial += 1 }
                if self.projectsBySHA != grouped { self.projectsBySHA = grouped }
                self.scanError = errors.isEmpty ? nil : errors.joined(separator: "\n")
                self.scanning = false
                self.lastScan = Date()
                self.updateWatchers([paths.roms.path, paths.projects.path] + subdirs.sorted())
            }
        }
    }

    // MARK: folder watching

    private func updateWatchers(_ dirs: [String]) {
        let wanted = Set(dirs)
        for (path, src) in watchers where !wanted.contains(path) {
            src.cancel()
            watchers[path] = nil
        }
        for path in dirs where watchers[path] == nil {
            let fd = open(path, O_EVTONLY)
            guard fd >= 0 else { continue }
            let src = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename, .delete, .link, .extend],
                                                                queue: .main)
            src.setEventHandler { [weak self] in self?.refresh() }
            src.setCancelHandler { close(fd) }
            src.resume()
            watchers[path] = src
        }
    }

    func revealROMFolder() { reveal(paths.roms) }
    func revealProjectsFolder() { reveal(paths.projects) }

    private func reveal(_ url: URL) {
        if !FileManager.default.fileExists(atPath: url.path) { ensureFolders() }
        NSWorkspace.shared.open(url)
    }
}
