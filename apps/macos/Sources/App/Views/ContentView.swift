// Main window: viewport, sidebar (takes / bookmarks / controllers), transport + timeline.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct WindowAccessor: NSViewRepresentable {
    let onWindow: (NSWindow) -> Void
    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        DispatchQueue.main.async { if let w = v.window { onWindow(w) } }
        return v
    }
    func updateNSView(_ nsView: NSView, context: Context) {
        DispatchQueue.main.async { if let w = nsView.window { onWindow(w) } }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                viewport
                if model.showSidebar && model.status.hasSession {
                    Divider()
                    SidebarView().frame(width: 270)
                }
            }
            if model.status.hasSession {
                Divider()
                TransportBar()
            }
        }
        .frame(minWidth: 1060, minHeight: 640)
        .background(WindowAccessor { w in
            model.mainWindow = w
            w.title = windowTitle
            w.representedURL = model.status.projectPath.isEmpty ? nil : URL(fileURLWithPath: model.status.projectPath)
            w.isDocumentEdited = model.status.unsaved
        })
        .sheet(isPresented: $model.showExport) { ExportSheet().environmentObject(model) }
        .toolbar {
            ToolbarItemGroup(placement: .navigation) {
                Button { model.newProject() } label: { Label("新規", systemImage: "doc.badge.plus") }.help("新規プロジェクト (⌘N)")
                Button { model.openProjectPanel() } label: { Label("開く", systemImage: "folder") }.help("プロジェクトを開く (⌘O)")
                LibraryToolbarButton()
                Button { model.saveSync() } label: { Label("保存", systemImage: "square.and.arrow.down") }
                    .disabled(!model.status.hasSession).help("保存 (⌘S)")
            }
            ToolbarItemGroup(placement: .primaryAction) {
                Button { model.showExport = true } label: { Label("MP4書き出し", systemImage: "film") }
                    .disabled(!model.status.hasSession || model.status.takeLength == 0).help("MP4に書き出す (⌘E)")
                Button { model.showLatency.toggle() } label: { Label("レイテンシ", systemImage: "gauge.with.dots.needle.33percent") }
                    .help("レイテンシ表示 (⌘L)")
                Button { model.showSidebar.toggle() } label: { Label("サイドバー", systemImage: "sidebar.right") }
            }
        }
    }

    private var windowTitle: String {
        if !model.status.projectPath.isEmpty { return URL(fileURLWithPath: model.status.projectPath).lastPathComponent }
        if model.status.hasSession { return URL(fileURLWithPath: model.status.romPath).lastPathComponent + "（未保存）" }
        return "ReplayNES"
    }

    @ViewBuilder private var viewport: some View {
        ZStack(alignment: .topLeading) {
            Color.black
            if model.status.hasSession {
                MetalGameView(emu: model.emu, options: model.displayOptions)
                StatusBadges().padding(10)
                if model.showLatency {
                    VStack { Spacer(); HStack { LatencyOverlay(); Spacer() } }.padding(10)
                }
            } else {
                WelcomeView().frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            if let n = model.notice {
                VStack {
                    Spacer()
                    Text(n)
                        .font(.callout)
                        .padding(.horizontal, 14).padding(.vertical, 8)
                        .background(.ultraThinMaterial, in: Capsule())
                        .padding(.bottom, 16)
                }
                .frame(maxWidth: .infinity)
                .transition(.opacity)
                .allowsHitTesting(false)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

struct StatusBadges: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        let st = model.status
        HStack(spacing: 6) {
            badge(st.recording ? "● 録画" : "▶︎ 再生", st.recording ? .red : .green)
            if st.rewinding { badge("◀◀ 巻き戻し中", .orange) }
            else if st.paused { badge("❚❚ 一時停止", .gray) }
            else if st.fastForward { badge("▶▶ 早送り", .blue) }
            else if st.slow != .normal { badge("スロー \(st.slow.label)", .purple) }
            if st.endOfTake { badge("テイク終端", .yellow) }
            if st.flashActive && model.showFlashIndicator { badge("フラッシュ低減中", .teal) }
        }
        .allowsHitTesting(false)
    }
    private func badge(_ t: String, _ c: Color) -> some View {
        Text(t).font(.system(size: 12, weight: .semibold)).foregroundStyle(.white)
            .padding(.horizontal, 8).padding(.vertical, 3)
            .background(c.opacity(0.75), in: RoundedRectangle(cornerRadius: 5))
    }
}

struct WelcomeView: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .firstTextBaseline, spacing: 14) {
                Text("ReplayNES").font(.system(size: 30, weight: .bold))
                Text("ミスしたら戻って録り直す。最後に通しのプレイ動画を書き出す。").foregroundStyle(.secondary)
                Spacer()
            }
            HStack(spacing: 12) {
                Button { model.newProject() } label: { Label("新規プロジェクト…", systemImage: "doc.badge.plus") }
                    .keyboardShortcut("n")
                Button { model.openProjectPanel() } label: { Label("プロジェクトを開く…", systemImage: "folder") }
                Button("ROMを開いて試す（保存しない）…") { model.quickPlay() }.buttonStyle(.link)
                Spacer()
                Text("ROMはプロジェクトにコピーされません（パスと SHA-256 のみ記録）。")
                    .font(.caption).foregroundStyle(.secondary)
            }
            LibraryView(library: model.library)
        }
        .padding(24)
        .background(Color(nsColor: .windowBackgroundColor))
    }
}

/// Toolbar button opening the library window.
struct LibraryToolbarButton: View {
    @Environment(\.openWindow) private var openWindow
    var body: some View {
        Button { openWindow(id: "library") } label: { Label("ライブラリ", systemImage: "books.vertical") }
            .help("ライブラリ (⇧⌘L)")
    }
}

struct LatencyOverlay: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        let s = model.stats
        VStack(alignment: .leading, spacing: 2) {
            Text("レイテンシ計測").bold()
            row("入力→表示 (平均/直近)", String(format: "%.1f / %.1f ms", s.inputToPresentMs, s.lastInputToPresentMs))
            row("サンプル→エミュ完了", String(format: "%.2f ms", s.sampleToEmulatedMs))
            row("エミュ完了→表示", String(format: "%.1f ms", s.emulatedToPresentMs))
            row("表示 fps", String(format: "%.1f", s.presentedFPS))
            row("音声バッファ / 出力遅延", String(format: "%.1f ms / %.1f ms", s.audioFillMs, s.audioOutputLatencyMs))
            row("音声 IO フレーム", "\(s.audioCallbackFrames)")
            row("アンダーラン / 破棄サンプル", "\(s.audioUnderruns) / \(s.audioDropped)")
            row("自動保存 (直近)", String(format: "%.1f ms", s.lastAutosaveMs))
            row("遅延ティック", "\(s.lateTicks)")
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(.white)
        .padding(8)
        .background(.black.opacity(0.65), in: RoundedRectangle(cornerRadius: 6))
        .allowsHitTesting(false)
    }
    private func row(_ k: String, _ v: String) -> some View {
        HStack { Text(k); Spacer(minLength: 12); Text(v) }.frame(width: 300)
    }
}
