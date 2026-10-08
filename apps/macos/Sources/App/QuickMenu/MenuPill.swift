// The always-visible menu pill "☰ Menu  L+R" (docs/design/UI_REDESIGN.md, "Always-visible menu
// affordance"). While the game plays it is drawn by GameRenderer inside the game's own Metal pass
// (MenuPillOverlay below hands it the picture), so nothing sits above the game layer and full
// screen stays direct-to-display; a click on it is hit-tested by GameLayerView. Paused, in menus
// and on the library it is an ordinary SwiftUI button at the same place.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI

/// The pill's look (also rendered to the Metal texture with ImageRenderer).
struct MenuPillLabel: View {
    let glyph: String
    var hovered = false
    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "line.3.horizontal").font(.system(size: 12, weight: .bold))
            Text("Menu").font(.system(size: 13, weight: .semibold))
            Text(verbatim: glyph).font(.system(size: 11, weight: .bold, design: .rounded))
                .padding(.horizontal, 5).frame(height: 17)
                .background(RoundedRectangle(cornerRadius: 4).fill(Color.white.opacity(0.18)))
        }
        .foregroundStyle(Color.white.opacity(0.95))
        .padding(.leading, 10).padding(.trailing, 5)
        .frame(height: 27)
        .background(Capsule().fill(Color(red: 0.08, green: 0.08, blue: 0.09).opacity(hovered ? 0.9 : 0.72)))
        .overlay(Capsule().stroke(Color.white.opacity(hovered ? 0.35 : 0.16), lineWidth: 1))
        .fixedSize()
        .environment(\.colorScheme, .dark)
    }
}

/// SwiftUI pill (paused / menus / library). Pulses twice on the very first launch.
struct MenuPillButton: View {
    let glyph: String
    let action: () -> Void
    @State private var hovered = false
    @State private var pulse = false
    @AppStorage("menuPillIntroduced") private var introduced = false

    var body: some View {
        MenuPillLabel(glyph: glyph, hovered: hovered)
            .scaleEffect(pulse ? 1.14 : 1)
            .contentShape(Capsule())
            .onHover { hovered = $0 }
            .onTapGesture(perform: action)
            .help(Text("Menu (L+R on a controller, Esc on the keyboard)"))
            .onAppear {
                guard !introduced, !AppModel.shared.scripted else { return }
                introduced = true
                withAnimation(.easeInOut(duration: 0.35).repeatCount(4, autoreverses: true).delay(0.6)) { pulse = true }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.6 + 4 * 0.35) { pulse = false }
            }
    }
}

/// Shared between the main thread (picture, visibility, pointer activity) and the emulation
/// thread's renderer (reads `state`).
final class MenuPillOverlay {
    static let shared = MenuPillOverlay()

    struct State {
        var image: CGImage?
        var generation = 0
        /// Pixel size of `image` (it is rendered at the display's backing scale).
        var pixelSize = CGSize.zero
        var scale: CGFloat = 2
        /// Drawn by the Metal pass (playing; the SwiftUI pill is hidden then).
        var visible = false
        /// Pointer activity / resume (host seconds, CACurrentMediaTime): the fade restarts.
        var activity = 0.0
    }
    private let lock = NSLock()
    private var s = State()
    private var glyph = ""
    private var monitor: Any?

    var state: State { lock.lock(); defer { lock.unlock() }; return s }

    /// Main thread: (re)renders the picture when the glyph or the backing scale changes.
    func update(glyph g: String, scale: CGFloat) {
        guard g != glyph || abs(scale - state.scale) > 0.01 || state.image == nil else { return }
        glyph = g
        let rendered: CGImage? = MainActor.assumeIsolated {
            let r = ImageRenderer(content: MenuPillLabel(glyph: g))
            r.scale = scale
            return r.cgImage
        }
        guard let img = rendered else { return }
        lock.lock()
        s.image = img
        s.generation += 1
        s.pixelSize = CGSize(width: img.width, height: img.height)
        s.scale = scale
        lock.unlock()
    }

    /// -menuPillMetal NO: never drawn while playing (A/B latency measurements, perf-smoke EXTRA_ARGS).
    static let metalEnabled = UserDefaults.standard.flag("menuPillMetal", default: true)

    func setVisible(_ visible: Bool) {
        let v = visible && Self.metalEnabled
        lock.lock()
        if v && !s.visible { s.activity = CACurrentMediaTime() }
        s.visible = v
        lock.unlock()
    }

    func noteActivity() {
        lock.lock(); s.activity = CACurrentMediaTime(); lock.unlock()
    }

    /// Pointer movement over the main window brings the pill back to full opacity.
    func startMonitoring(window: @escaping () -> NSWindow?) {
        guard monitor == nil else { return }
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseDown]) { [weak self] ev in
            if ev.window != nil, ev.window === window() { self?.noteActivity() }
            return ev
        }
    }
}
