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
    @Environment(\.openWindow) private var openWindow
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                viewport
                if model.showSidebar && model.status.hasSession && !model.immersive {
                    Divider()
                    SidebarView().frame(width: 270)
                }
            }
            if model.status.hasSession && !model.immersive {
                Divider()
                TransportBar()
            }
        }
        .frame(minWidth: 820, minHeight: 560)
        // Immersive full screen: also under the (hidden) title bar, so the game layer covers the
        // whole screen; otherwise macOS keeps compositing it (measured: 3024x1794 layer on a
        // 3024x1898 full-screen window stays composited, +1 refresh).
        .ignoresSafeArea(.container, edges: model.immersive ? .all : [])
        .background(WindowAccessor { w in
            // Only real changes: setting these (even to the same value) makes AppKit redo the
            // title bar, drag regions and cursor rects.
            if model.mainWindow !== w { model.mainWindow = w }
            let title = windowTitle
            if w.title != title { w.title = title }
            let path = model.status.projectPath
            let url = path.isEmpty || model.isTempSession(path) ? nil : URL(fileURLWithPath: path)
            if w.representedURL != url { w.representedURL = url }
            if w.isDocumentEdited != model.status.unsaved { w.isDocumentEdited = model.status.unsaved }
        })
        // Full screen, playing, pointer resting: no toolbar either, so the game layer covers the
        // whole screen (direct-to-display; FullScreenChrome.swift).
        .toolbar(model.immersive ? .hidden : .automatic, for: .windowToolbar)
        .sheet(isPresented: $model.showExport) { ExportSheet().environmentObject(model) }
        // Scripted checks (--test-actions open:<window>, TestHooks.swift).
        .onReceive(NotificationCenter.default.publisher(for: AppModel.testOpenWindow)) { n in
            guard let id = n.object as? String else { return }
            if id == "settings" { openSettings() } else { openWindow(id: id) }
        }
        .toolbar {
            ToolbarItemGroup(placement: .navigation) {
                LibraryToolbarButton()
                Button { model.saveSync() } label: { Label("Save", systemImage: "square.and.arrow.down") }
                    .disabled(!model.status.hasSession).help("Save (⌘S)")
            }
            ToolbarItemGroup(placement: .primaryAction) {
                Button { model.showExport = true } label: { Label("Export MP4", systemImage: "film") }
                    .disabled(!model.status.hasSession || model.status.takeLength == 0).help("Export to MP4 (⌘E)")
                Button { model.showSidebar.toggle() } label: { Label("Sidebar", systemImage: "sidebar.right") }
                    .help("Sidebar (takes, bookmarks, controllers) (⌥⌘S)")
            }
        }
    }

    private var windowTitle: String {
        let path = model.status.projectPath
        if !path.isEmpty && !model.isTempSession(path) { return URL(fileURLWithPath: path).lastPathComponent }
        if model.status.hasSession { return URL(fileURLWithPath: model.status.romPath).lastPathComponent + String(localized: " (Unsaved)") }
        return "ReplayNES"
    }

    @ViewBuilder private var viewport: some View {
        ZStack(alignment: .topLeading) {
            if !model.immersive { Color.black }
            if model.status.hasSession {
                MetalGameView(emu: model.emu, options: model.displayOptions)
                if let img = model.snapshotFrame { SnapshotFrameView(image: img, options: model.displayOptions) }
                // Full-screen play hides everything drawn over the game (FullScreenChrome.swift):
                // any view above the layer forces a compositor pass.
                if !model.immersive {
                    StatusBadges().padding(10)
                    if model.showLatency {
                        VStack { Spacer(); HStack { LatencyOverlay(); Spacer() } }.padding(10)
                    }
                    if model.showPracticePanel || model.status.practicing {
                        VStack { Spacer(); HStack { PracticeOverlay(); Spacer() } }
                            .padding(.leading, 12).padding(.bottom, 12)
                    }
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
            if st.practicing { badge(String(localized: "Practicing (not recording)"), .orange) }
            else { badge(st.recording ? String(localized: "● REC") : String(localized: "▶︎ PLAY"), st.recording ? .red : .green) }
            if st.rewinding { badge(String(localized: "◀◀ Rewinding"), .orange) }
            else if st.paused { badge(String(localized: "❚❚ Paused"), .gray) }
            else if st.fastForward { badge(String(localized: "▶▶ Fast-Forward"), .blue) }
            else if st.slow != .normal { badge(String(localized: "Slow \(st.slow.label)"), .purple) }
            if st.endOfTake && !st.practicing { badge(String(localized: "End of Take"), .yellow) }
            if st.flashActive && model.showFlashIndicator { badge(String(localized: "Flash Reduction Active"), .teal) }
            StreamOutputBadge()
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
                Text("Made a mistake? Go back and record it again. At the end, export one continuous play video.").foregroundStyle(.secondary)
                Spacer()
            }
            HStack(spacing: 12) {
                Button { model.newProject() } label: { Label("New Project…", systemImage: "doc.badge.plus") }
                    .keyboardShortcut("n")
                Button { model.openProjectPanel() } label: { Label("Open Project…", systemImage: "folder") }
                Button("Try a ROM (No Project)…") { model.quickPlay() }.buttonStyle(.link)
                Spacer()
                Text("ROMs are not copied into projects (only the path and SHA-256 are stored).")
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
        Button { openWindow(id: "library") } label: { Label("Library", systemImage: "books.vertical") }
            .help("Library (⇧⌘L)")
    }
}

struct LatencyOverlay: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        let s = model.stats
        VStack(alignment: .leading, spacing: 2) {
            Text("Latency").bold()
            row(String(localized: "Input→display (avg/last)"), String(format: "%.1f / %.1f ms", s.inputToPresentMs, s.lastInputToPresentMs))
            row(String(localized: "Input sample→display"), String(format: "%.1f ms", s.sampleToPresentMs))
            row(String(localized: "Sample→emulated"), String(format: "%.2f ms", s.sampleToEmulatedMs))
            row(String(localized: "Emulated→display"), String(format: "%.1f ms", s.emulatedToPresentMs))
            row(String(localized: "Display fps"), String(format: "%.1f", s.presentedFPS))
            row(String(localized: "Pacing"), s.pacing)
            row(String(localized: "Input sampled before deadline"), String(format: "%.1f ms", s.inputLeadMs))
            row(String(localized: "Missed refreshes / judder"), "\(s.missedRefreshes + s.droppedFrames) / \(s.offCadence)")
            row(String(localized: "Display GPU (avg/max)"), String(format: "%.2f / %.2f ms", s.displayGPUMs, s.displayGPUMaxMs))
            if !s.crtInfo.isEmpty { row(String(localized: "CRT"), s.crtInfo) }
            row(String(localized: "Audio buffer / output"), String(format: "%.1f ms / %.1f ms", s.audioFillMs, s.audioOutputLatencyMs))
            row(String(localized: "Audio IO frames"), "\(s.audioCallbackFrames)")
            row(String(localized: "Underruns / dropped"), "\(s.audioUnderruns) / \(s.audioDropped)")
            row(String(localized: "Autosave (last)"), String(format: "%.1f ms", s.lastAutosaveMs))
            row(String(localized: "Late ticks"), "\(s.lateTicks)")
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(.white)
        .padding(8)
        .background(.black.opacity(0.65), in: RoundedRectangle(cornerRadius: 6))
        .allowsHitTesting(false)
    }
    private func row(_ k: String, _ v: String) -> some View {
        HStack { Text(k).lineLimit(1); Spacer(minLength: 12); Text(v).lineLimit(1) }.frame(width: 330)
    }
}

/// --snapshot only: the current frame drawn by SwiftUI (Metal content is not captured by
/// cacheDisplay), placed exactly like GameRenderer.viewport so overlays stay on top of it.
struct SnapshotFrameView: View {
    let image: CGImage
    let options: DisplayOptions
    var body: some View {
        GeometryReader { geo in
            let scale = NSScreen.main?.backingScaleFactor ?? 2
            let px = CGSize(width: geo.size.width * scale, height: geo.size.height * scale)
            let (r, crop) = GameRenderer.viewport(drawableSize: px, options: options)
            let cropped = image.cropping(to: CGRect(x: 0, y: crop, width: image.width, height: image.height - 2 * crop)) ?? image
            Image(decorative: cropped, scale: 1)
                .interpolation(.none)
                .resizable()
                .frame(width: r.width / scale, height: r.height / scale)
                // viewport() is bottom-left based; SwiftUI is top-left.
                .position(x: (r.minX + r.width / 2) / scale, y: geo.size.height - (r.minY + r.height / 2) / scale)
        }
        .allowsHitTesting(false)
    }
}
