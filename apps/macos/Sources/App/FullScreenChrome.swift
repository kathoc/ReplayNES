// Full-screen play: the window chrome (transport bar, sidebar, status badges, practice panel,
// latency overlay) hides while the game runs and the pointer rests, so the game's CAMetalLayer
// covers the whole screen with nothing above it. macOS then shows it direct-to-display (no
// compositor pass: about one refresh less latency, and no compositor-induced judder;
// docs/FRAME_PACING.md). Moving the pointer or pausing brings everything back at once.
// -fullScreenAutoHide NO keeps the chrome visible (it then costs that refresh).
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

final class FullScreenChrome {
    /// Pointer idle time before the chrome hides.
    static let hideAfter = 2.0

    var onChange: (() -> Void)?
    weak var window: NSWindow? {
        didSet {
            fullScreen = window?.styleMask.contains(.fullScreen) ?? false
            window?.acceptsMouseMovedEvents = true
            onChange?()
        }
    }
    private(set) var fullScreen = false
    private(set) var pointerIdle = false
    private var hideWork: DispatchWorkItem?
    private var observers: [NSObjectProtocol] = []
    private var monitor: Any?

    init() {
        let nc = NotificationCenter.default
        for (name, on) in [(NSWindow.didEnterFullScreenNotification, true), (NSWindow.willExitFullScreenNotification, false)] {
            observers.append(nc.addObserver(forName: name, object: nil, queue: .main) { [weak self] n in
                guard let self, let w = n.object as? NSWindow, w === self.window else { return }
                self.fullScreen = on
                self.pointerMoved()
            })
        }
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseDown, .rightMouseDown, .scrollWheel]) { [weak self] ev in
            if let self, self.fullScreen, ev.window === self.window { self.pointerMoved() }
            return ev
        }
    }

    static var autoHide: Bool { UserDefaults.standard.flag("fullScreenAutoHide", default: true) }

    /// Shows the chrome and restarts the idle timer.
    func pointerMoved() {
        hideWork?.cancel()
        let wasIdle = pointerIdle
        pointerIdle = false
        defer { if wasIdle || !fullScreen { onChange?() } }
        guard fullScreen else { return }
        let w = DispatchWorkItem { [weak self] in
            guard let self, self.fullScreen else { return }
            self.pointerIdle = true
            NSCursor.setHiddenUntilMouseMoves(true)
            self.onChange?()
        }
        hideWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.hideAfter, execute: w)
    }
}

extension AppModel {
    /// Main thread: recomputes `immersive` (full screen, playing, pointer resting).
    func updateImmersive() {
        let on = FullScreenChrome.autoHide && chrome.fullScreen && chrome.pointerIdle && status.hasSession && !status.paused
        if on != immersive { immersive = on }
    }
}
