// ROM library: ROMs in ~/Documents/ReplayNES/ROM with their projects (matched by SHA-256).
// Shown on the start screen and in the "Library" window (⇧⌘L).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct LibraryView: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var library: LibraryModel
    /// Called after a ROM / project was chosen (the library window closes itself).
    var onChoose: () -> Void = {}
    @State private var search = ""
    @State private var selection: LibraryROM.ID?

    private var filtered: [LibraryROM] {
        let q = search.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return library.roms }
        return library.roms.filter { $0.name.localizedStandardContains(q) || $0.relativePath.localizedStandardContains(q) }
    }

    private var selected: LibraryROM? {
        guard let selection else { return nil }
        return library.roms.first { $0.id == selection }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            if let err = library.folderError {
                banner(err, retry: true)
            } else if let err = library.scanError {
                banner(err, retry: false)
            }
            HStack(spacing: 0) {
                romList.frame(minWidth: 280, idealWidth: 360)
                Divider()
                detail.frame(minWidth: 300, maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            }
            .background(Color(nsColor: .textBackgroundColor).opacity(0.6))
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.secondary.opacity(0.25)))
            Text("ROMs: \(displayPath(library.paths.roms))   Projects (autosaved): \(displayPath(library.paths.projects))")
                .font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                .textSelection(.enabled)
        }
        .onAppear { library.refresh(); selectFirstIfNeeded() }
        .onChange(of: library.roms) { _, _ in selectFirstIfNeeded() }
        .onChange(of: search) { _, _ in
            if let sel = selection, !filtered.contains(where: { $0.id == sel }) { selection = filtered.first?.id }
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Text("Library").font(.title2.bold())
            if library.scanning { ProgressView().controlSize(.small) }
            Spacer()
            HStack(spacing: 4) {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                TextField("Search ROMs", text: $search).textFieldStyle(.plain).frame(width: 180)
                if !search.isEmpty {
                    Button { search = "" } label: { Image(systemName: "xmark.circle.fill") }.buttonStyle(.borderless)
                }
            }
            .padding(.horizontal, 8).padding(.vertical, 4)
            .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.secondary.opacity(0.3)))
            Button { library.refresh() } label: { Label("Reload", systemImage: "arrow.clockwise") }
                .help("Rescan the ROM folder")
            Menu {
                Button("ROM Folder") { library.revealROMFolder() }
                Button("Projects Folder") { library.revealProjectsFolder() }
            } label: {
                Label("Open in Finder", systemImage: "folder")
            } primaryAction: {
                library.revealROMFolder()
            }
            .fixedSize()
            .help("Open the ROM folder in Finder (▾ for the projects folder)")
        }
    }

    private func banner(_ text: String, retry: Bool) -> some View {
        HStack(alignment: .top) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            Text(text).font(.callout).textSelection(.enabled)
            Spacer()
            if retry { Button("Retry") { library.start() } }
        }
        .padding(8)
        .background(Color.orange.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
    }

    @ViewBuilder private var romList: some View {
        if library.roms.isEmpty {
            VStack(spacing: 10) {
                Image(systemName: "tray").font(.system(size: 30)).foregroundStyle(.secondary)
                Text(library.scanning ? String(localized: "Loading…") : String(localized: "No ROMs")).font(.headline)
                Text("Put .nes files in the ROM folder and they appear here.").font(.caption).foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                Button("Open ROM Folder in Finder") { library.revealROMFolder() }
            }
            .padding()
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            List(selection: $selection) {
                ForEach(filtered) { rom in
                    ROMRow(rom: rom, projectCount: library.projects(for: rom).count).tag(rom.id)
                }
            }
            .listStyle(.inset)
            .contextMenu(forSelectionType: LibraryROM.ID.self) { ids in
                if let rom = rom(ids.first) {
                    Button("Play (New Project)") { play(rom) }
                    if let p = library.projects(for: rom).first { Button("Continue: \(p.name)") { resume(p) } }
                    Divider()
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([rom.url]) }
                }
            } primaryAction: { ids in
                if let rom = rom(ids.first) { play(rom) }  // double-click / Return
            }
            .overlay {
                if filtered.isEmpty { Text("No ROMs match “\(search)”").foregroundStyle(.secondary) }
            }
        }
    }

    @ViewBuilder private var detail: some View {
        if let rom = selected {
            let projects = library.projects(for: rom)
            VStack(alignment: .leading, spacing: 10) {
                Text(rom.name).font(.title3.bold()).lineLimit(2)
                Text(rom.relativePath).font(.caption).foregroundStyle(.secondary)
                HStack {
                    Text(ByteCountFormatter.string(fromByteCount: rom.size, countStyle: .file))
                    if let sha = rom.sha256 { Text("SHA-256 \(sha.prefix(12))…").monospaced() }
                    else { Text("Can’t compute SHA-256").foregroundStyle(.orange) }
                }
                .font(.caption).foregroundStyle(.secondary)
                Button { play(rom) } label: { Label("Play", systemImage: "play.fill").frame(minWidth: 120) }
                    .controlSize(.large).buttonStyle(.borderedProminent)
                    .help("Creates a new project and starts right away (autosaved in the Projects folder)")
                Divider()
                Text("Projects for This ROM").font(.headline)
                if projects.isEmpty {
                    Text("None yet. Start with “Play” and it is saved automatically.").font(.caption).foregroundStyle(.secondary)
                } else {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 6) {
                            ForEach(projects) { p in
                                HStack {
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(p.name).lineLimit(1).truncationMode(.middle)
                                        Text("Last saved \(p.modified.formatted(date: .abbreviated, time: .shortened))")
                                            .font(.caption).foregroundStyle(.secondary)
                                    }
                                    Spacer()
                                    Button("Continue") { resume(p) }
                                    Button { NSWorkspace.shared.activateFileViewerSelecting([p.url]) } label: { Image(systemName: "folder") }
                                        .buttonStyle(.borderless).help("Show in Finder")
                                }
                                .padding(.vertical, 2)
                            }
                        }
                    }
                }
                Spacer(minLength: 0)
            }
            .padding(14)
        } else {
            VStack(spacing: 8) {
                Image(systemName: "gamecontroller").font(.system(size: 30)).foregroundStyle(.secondary)
                Text(library.roms.isEmpty ? String(localized: "Add some ROMs") : String(localized: "Choose a ROM")).font(.headline)
                Text("Double-click or press Return to start playing right away.\nOpen earlier projects with “Continue” on the right.")
                    .font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    /// Keeps a valid selection so the detail pane and Return work right away.
    private func selectFirstIfNeeded() {
        if let sel = selection, library.roms.contains(where: { $0.id == sel }) { return }
        selection = filtered.first?.id
    }

    private func rom(_ id: LibraryROM.ID?) -> LibraryROM? {
        guard let id else { return nil }
        return library.roms.first { $0.id == id }
    }

    private func play(_ rom: LibraryROM) {
        if model.playFromLibrary(rom) { onChoose() }
    }

    private func resume(_ p: LibraryProject) {
        if model.continueProject(p.url) { onChoose() }
    }

    private func displayPath(_ url: URL) -> String {
        let home = NSHomeDirectory()
        return url.path.hasPrefix(home) ? "~" + url.path.dropFirst(home.count) : url.path
    }
}

private struct ROMRow: View {
    let rom: LibraryROM
    let projectCount: Int
    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(rom.name).lineLimit(1)
                if rom.relativePath.contains("/") {
                    Text(rom.relativePath).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            Spacer()
            if projectCount > 0 {
                Text("\(projectCount)").font(.caption.monospacedDigit())
                    .padding(.horizontal, 6).padding(.vertical, 1)
                    .background(Color.accentColor.opacity(0.2), in: Capsule())
                    .help("\(projectCount) projects")
            }
        }
        .padding(.vertical, 2)
    }
}

/// Content of the "Library" window.
struct LibraryWindow: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismissWindow) private var dismissWindow
    var body: some View {
        LibraryView(library: model.library) {
            dismissWindow(id: "library")
            model.mainWindow?.makeKeyAndOrderFront(nil)
        }
        .padding(16)
        .frame(minWidth: 720, minHeight: 420)
    }
}
