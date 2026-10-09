// Main window: the game (nothing else while playing but the menu pill), the seek bar when paused,
// the Quick Menu over the game, and the library (start screen) when no game is open
// (docs/design/UI_REDESIGN.md).
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
    @ObservedObject private var menu = AppModel.shared.quickMenu
    @ObservedObject private var monitor = AppModel.shared.input.controllerMonitor
    @ObservedObject private var dialogs = AppModel.shared.dialogs

    var body: some View {
        viewport
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
            .toolbar(model.immersive ? .hidden : .automatic, for: .windowToolbar)
            .sheet(isPresented: $model.showExport) { ExportSheet().environmentObject(model) }
            // The game, its menus and the library are dark; so is the window around them.
            .preferredColorScheme(.dark)
    }

    private var windowTitle: String {
        let path = model.status.projectPath
        if !path.isEmpty && !model.isTempSession(path) { return URL(fileURLWithPath: path).lastPathComponent }
        if model.status.hasSession { return URL(fileURLWithPath: model.status.romPath).lastPathComponent + String(localized: " (Unsaved)") }
        return "ReplayNES"
    }

    private var libraryShown: Bool { !model.status.hasSession || model.showLibrary }

    @ViewBuilder private var viewport: some View {
        let st = model.status
        ZStack(alignment: .topLeading) {
            if !model.immersive { Color.black }
            if st.hasSession {
                MetalGameView(emu: model.emu, options: model.displayOptions) { menu.open() }
                if let img = model.snapshotFrame { SnapshotFrameView(image: img, options: model.displayOptions) }
                // Full-screen play hides everything drawn over the game (FullScreenChrome.swift):
                // any view above the layer forces a compositor pass.
                if !model.immersive && !menu.isOpen && !model.showLibrary {
                    if !st.paused { StatusBadges().padding(12) }
                    if model.showLatency {
                        VStack { Spacer(); HStack { LatencyOverlay(); Spacer() } }.padding(12).padding(.bottom, st.paused ? 126 : 0)
                    }
                    if st.practicing && !st.paused {
                        VStack { Spacer(); HStack { PracticeOverlay(); Spacer() } }
                            .padding(.leading, 12).padding(.bottom, 12)
                    }
                    if st.paused {
                        VStack { Spacer(); SeekBarOverlay() }
                            .transition(.opacity)
                    }
                }
            }
            if libraryShown {
                LibraryHome(library: model.library, nav: model.libraryNav)
                    .transition(.opacity)
            }
            if menu.isOpen {
                QuickMenuOverlay(menu: menu, monitor: monitor)
                    .transition(.opacity)
            }
            // The SwiftUI pill: paused, menus, library (and --snapshot captures). While playing the
            // Metal pass draws it (MenuPill.swift).
            if showSwiftUIPill {
                HStack {
                    Spacer()
                    MenuPillButton(glyph: model.pillGlyph) { menu.toggle() }
                }
                .padding(.top, MenuPillLayout.margin).padding(.trailing, MenuPillLayout.margin)
            }
            // In-window dialogs (Dialogs.swift): over everything but the toasts.
            if dialogs.isActive {
                DialogOverlay(center: dialogs, monitor: monitor)
                    .transition(.opacity)
            }
            if let n = model.notice {
                VStack {
                    Spacer()
                    Text(n)
                        .font(.callout)
                        .multilineTextAlignment(.center)
                        .lineLimit(3)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(.horizontal, 16).padding(.vertical, 9)
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 18))
                        .frame(maxWidth: 640)   // wraps longer toasts; short ones keep their own width
                        .padding(.bottom, st.paused && st.hasSession && !menu.isOpen ? 140 : 16)   // above the seek bar (and its hint row)
                }
                .frame(maxWidth: .infinity)
                .transition(.opacity)
                .allowsHitTesting(false)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .animation(QMStyle.anim, value: st.paused)
    }

    private var showSwiftUIPill: Bool {
        let st = model.status
        if model.immersive { return false }
        return !st.hasSession || st.paused || menu.isOpen || model.showLibrary || model.snapshotFrame != nil
    }
}

/// Transient states only (record mode is the default and shows nothing).
struct StatusBadges: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        let st = model.status
        HStack(spacing: 6) {
            if st.practicing { badge(String(localized: "Practicing (not recording)"), .orange) }
            else if !st.recording { badge(String(localized: "▶︎ Replay"), .green) }
            if st.rewinding { badge(String(localized: "◀◀ Rewinding"), .orange) }
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
