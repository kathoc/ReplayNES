// Sidebar: takes summary (simple vocabulary), bookmarks, controllers.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SidebarView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    @Environment(\.openSettings) private var openSettings
    @State private var editing: UInt64?
    @State private var editText = ""

    var body: some View {
        let st = model.status
        List {
            Section("Take") {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Current take #\(st.activeTake)").font(.headline)
                    ClockView { st in
                        Text("Length \(Engine.timecode(forFrame: st.takeLength)) (\(st.takeLength) frames) · Takes: \(st.takeCount)")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                Button { model.undoTake() } label: { Label("Back to Previous Take", systemImage: "arrow.uturn.backward") }
                    .disabled(st.undoDepth == 0)
                Button { openWindow(id: "takes") } label: { Label("Takes (Details)…", systemImage: "list.bullet.indent") }
            }
            Section {
                if model.bookmarks.isEmpty {
                    Text("No bookmarks yet. Press B to add one.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                ForEach(model.bookmarks) { b in
                    VStack(alignment: .leading, spacing: 3) {
                        if editing == b.id {
                            TextField("Name", text: $editText, onCommit: {
                                model.renameBookmark(b.id, editText); editing = nil
                            })
                        } else {
                            Text(b.name).lineLimit(1)
                                .onTapGesture(count: 2) { editText = b.name; editing = b.id }
                        }
                        HStack {
                            Text(verbatim: "\(Engine.timecode(forFrame: b.frame)) · f\(b.frame)" + (b.onActiveTake ? "" : String(localized: " · other take")))
                                .font(.caption).foregroundStyle(.secondary)
                            Spacer()
                            Button("Rewind to Here") { model.gotoBookmark(b.id) }
                                .controlSize(.small)
                        }
                    }
                    .contextMenu {
                        Button("Rewind to Here") { model.gotoBookmark(b.id) }
                        Button("Rename") { editText = b.name; editing = b.id }
                        Divider()
                        Button("Delete", role: .destructive) { model.removeBookmark(b.id) }
                    }
                }
            } header: {
                HStack {
                    Text("Bookmarks")
                    Spacer()
                    Button { model.addBookmark() } label: { Image(systemName: "plus") }
                        .buttonStyle(.borderless).help("Add a bookmark at the current position")
                }
            }
            Section("Controllers") {
                if model.controllers.isEmpty {
                    Text("None connected (you can use the keyboard)").font(.caption).foregroundStyle(.secondary)
                }
                ForEach(model.controllers, id: \.self) { Text($0).font(.caption) }
                Button {
                    UserDefaults.standard.set("controller", forKey: SettingsView.tabKey)
                    openSettings()
                } label: { Label("Button Layout…", systemImage: "gamecontroller") }
                    .help("View and change button assignments on a controller diagram")
            }
            Section("ROM") {
                Text(URL(fileURLWithPath: st.romPath).lastPathComponent).font(.caption)
                    .help(st.romPath)
            }
        }
        .listStyle(.sidebar)
    }
}

/// Advanced take management (separate window): full take list with parent/branch info.
struct TakesPanel: View {
    @EnvironmentObject var model: AppModel

    private func depth(_ t: TakeInfo) -> Int {
        var d = 0
        var p = t.parentID
        let byID = Dictionary(uniqueKeysWithValues: model.takes.map { ($0.id, $0) })
        while p != 0, let parent = byID[p], d < 64 { d += 1; p = parent.parentID }
        return d
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Takes").font(.title2.bold())
            Text("Each time you re-record, a new take is created and the old continuation is kept. Usually “Back to Previous Take” is all you need.")
                .font(.caption).foregroundStyle(.secondary)
            Table(model.takes) {
                TableColumn("Take") { t in
                    HStack(spacing: 4) {
                        Text(String(repeating: "  ", count: depth(t)) + (t.parentID == 0 ? "" : "└ "))
                        Text("#\(t.id)").bold(t.isActive)
                        if t.isActive { Text("Active").font(.caption).foregroundStyle(.green) }
                    }
                }
                TableColumn("Branched From") { t in Text(t.parentID == 0 ? "—" : "#\(t.parentID) @ \(Engine.timecode(forFrame: t.branchFrame))") }
                TableColumn("Length") { t in Text("\(Engine.timecode(forFrame: t.length)) (\(t.length)f)") }
                TableColumn("Created") { t in Text("\(t.createdSeq)") }
                TableColumn("") { t in
                    Button("Switch to This Take") { model.activateTake(t.id) }
                        .disabled(t.isActive).controlSize(.small)
                }
            }
            HStack {
                Button { model.undoTake() } label: { Label("Back to Previous Take", systemImage: "arrow.uturn.backward") }
                    .disabled(model.status.undoDepth == 0)
                Text("Undo steps available: \(model.status.undoDepth)").font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(minWidth: 640, minHeight: 360)
    }
}
