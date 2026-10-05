// Sidebar: takes summary (simple vocabulary), bookmarks, controllers.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SidebarView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    @State private var editing: UInt64?
    @State private var editText = ""

    var body: some View {
        let st = model.status
        List {
            Section("テイク") {
                VStack(alignment: .leading, spacing: 4) {
                    Text("現在のテイク #\(st.activeTake)").font(.headline)
                    Text("長さ \(Engine.timecode(forFrame: st.takeLength))（\(st.takeLength) フレーム）・全\(st.takeCount)テイク")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Button { model.rerecordHere() } label: { Label("ここから録り直す", systemImage: "record.circle") }
                Button { model.undoTake() } label: { Label("前の試行へ戻す", systemImage: "arrow.uturn.backward") }
                    .disabled(st.undoDepth == 0)
                Button { openWindow(id: "takes") } label: { Label("テイク一覧（詳細）…", systemImage: "list.bullet.indent") }
            }
            Section {
                if model.bookmarks.isEmpty {
                    Text("ブックマークはありません。B キーで現在位置に追加できます。")
                        .font(.caption).foregroundStyle(.secondary)
                }
                ForEach(model.bookmarks) { b in
                    VStack(alignment: .leading, spacing: 3) {
                        if editing == b.id {
                            TextField("名前", text: $editText, onCommit: {
                                model.renameBookmark(b.id, editText); editing = nil
                            })
                        } else {
                            Text(b.name).lineLimit(1)
                                .onTapGesture(count: 2) { editText = b.name; editing = b.id }
                        }
                        HStack {
                            Text("\(Engine.timecode(forFrame: b.frame)) · f\(b.frame)\(b.onActiveTake ? "" : " · 別テイク")")
                                .font(.caption).foregroundStyle(.secondary)
                            Spacer()
                            Button("ここまで戻る") { model.gotoBookmark(b.id) }
                                .controlSize(.small)
                        }
                    }
                    .contextMenu {
                        Button("ここまで戻る") { model.gotoBookmark(b.id) }
                        Button("名前を変更") { editText = b.name; editing = b.id }
                        Divider()
                        Button("削除", role: .destructive) { model.removeBookmark(b.id) }
                    }
                }
            } header: {
                HStack {
                    Text("ブックマーク")
                    Spacer()
                    Button { model.addBookmark() } label: { Image(systemName: "plus") }
                        .buttonStyle(.borderless).help("現在位置にブックマークを追加")
                }
            }
            Section("コントローラー") {
                if model.controllers.isEmpty {
                    Text("未接続（キーボードで操作できます）").font(.caption).foregroundStyle(.secondary)
                }
                ForEach(model.controllers, id: \.self) { Text($0).font(.caption) }
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
            Text("テイク一覧").font(.title2.bold())
            Text("録り直すたびに新しいテイクが作られ、以前の続きは消さずに残ります。通常は「前の試行へ戻す」だけで十分です。")
                .font(.caption).foregroundStyle(.secondary)
            Table(model.takes) {
                TableColumn("テイク") { t in
                    HStack(spacing: 4) {
                        Text(String(repeating: "  ", count: depth(t)) + (t.parentID == 0 ? "" : "└ "))
                        Text("#\(t.id)").bold(t.isActive)
                        if t.isActive { Text("使用中").font(.caption).foregroundStyle(.green) }
                    }
                }
                TableColumn("分岐元") { t in Text(t.parentID == 0 ? "—" : "#\(t.parentID) @ \(Engine.timecode(forFrame: t.branchFrame))") }
                TableColumn("長さ") { t in Text("\(Engine.timecode(forFrame: t.length)) (\(t.length)f)") }
                TableColumn("作成順") { t in Text("\(t.createdSeq)") }
                TableColumn("") { t in
                    Button("このテイクに切り替え") { model.activateTake(t.id) }
                        .disabled(t.isActive).controlSize(.small)
                }
            }
            HStack {
                Button { model.undoTake() } label: { Label("前の試行へ戻す", systemImage: "arrow.uturn.backward") }
                    .disabled(model.status.undoDepth == 0)
                Text("取り消せる操作: \(model.status.undoDepth)").font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(minWidth: 640, minHeight: 360)
    }
}
