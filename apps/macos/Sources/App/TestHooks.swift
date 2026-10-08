// Scripted UI test hooks (launch arguments; used by smoke tests, harmless otherwise).
//   --inject-keys "<sec>:<keyCode>:<d|u>,..."   post synthetic key events to the main window at
//                                                the given times after launch. They travel the
//                                                same path as real keys (app event queue ->
//                                                InputManager's local monitor -> rn_input), so the
//                                                key-window / first-responder gating is exercised.
//   --library-play "<ROM name>"                 start a new library project for that ROM (as if
//                                                chosen in the library) once the scan has found it.
//   --inject-pad "<sec>:<element>:<d|u>,..."     controller slot-0 element changes (e.g. rightTrigger,
//                                                leftShoulder, dpad.left) through the real controller
//                                                path (hotkeys, paused D-pad stepping, game input).
//   --test-actions "<sec>:<action>[:<n>],..."    UI actions: setA/setB/practice:<slot 0-7>, stopPractice,
//                                                toggleRecord, togglePause, panel, fill, integer,
//                                                quit (the ⌘Q path: persist + resume record),
//                                                fullscreen, crtOn, crtOff, export (sheet), latency,
//                                                open:<library|takes|guide|settings>, menu:<page> (Quick Menu
//                                                at a QMPage raw value, e.g. menu:display), menuClose,
//                                                nav:<up|down|left|right|confirm|back|x|y|pagePrev|pageNext|menu|escape>,
//                                                libraryClose, windowSize:<w>x<h> (content size, points),
//                                                seek:<frame>, bookmark, undoTake,
//                                                resetProject:<keep A/B 0|1> (no dialog), resetPrompt, windowWidth:<pt>,
//                                                screen:<n> (moves the main window to NSScreen.screens[n]),
//                                                dumpLayers (logs the app's windows and the main window's layer tree),
//                                                dumpMenus (logs the menu bar: "menu: <Menu> | <item> | <⌘ key>").
//   --snapshot-at "<sec>:<png>,..."              extra window snapshots (see Snapshot.swift).
//   --snapshot-windows "<sec>:<prefix>,..."      captures every visible window (and sheet) to
//                                                <prefix>-<n>-<title>.png (localization checks).
//   --menu-walk <dir> [--menu-walk-delay s]      snapshots of every Quick Menu page, the paused seek bar
//                                                and the library to <dir>/<n>-<name>.png, and
//                                                <dir>/menu-walk.json with whether each page fit
//                                                without scrolling (scripts/ui-check-macos.sh).
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

private var injectQueue: [(UInt16, Bool)] = []
private var injectPumpScheduled = false

extension AppModel {
    func scheduleInjectedKeys(_ spec: String) {
        for item in spec.split(separator: ",") {
            let parts = item.split(separator: ":")
            guard parts.count == 3, let t = Double(parts[0]), let code = UInt16(parts[1]) else {
                NSLog("ReplayNES: bad --inject-keys item \(item)")
                continue
            }
            let down = parts[2] == "d"
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { [weak self] in
                injectQueue.append((code, down))
                self?.pumpInjectedKeys()
            }
        }
    }

    /// Posts queued events in order, but only while the main window is key: launched from a
    /// shell the app may not be frontmost yet (activation is async) and the key-window gate in
    /// InputManager would drop the event.
    private func pumpInjectedKeys() {
        guard let w = mainWindow else { return }
        if !NSApp.isActive || !w.isKeyWindow {
            NSApp.activate(ignoringOtherApps: true)
            w.makeKeyAndOrderFront(nil)
            if !injectPumpScheduled {
                injectPumpScheduled = true
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                    injectPumpScheduled = false
                    self?.pumpInjectedKeys()
                }
            }
            return
        }
        for (code, down) in injectQueue {
            let chars = code == 36 ? "\r" : code == 49 ? " " : ""
            guard let ev = NSEvent.keyEvent(with: down ? .keyDown : .keyUp, location: .zero, modifierFlags: [],
                                            timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: w.windowNumber,
                                            context: nil, characters: chars, charactersIgnoringModifiers: chars,
                                            isARepeat: false, keyCode: code) else { continue }
            NSApp.postEvent(ev, atStart: false)
        }
        injectQueue.removeAll()
    }

    func scheduleInjectedPad(_ spec: String) {
        for item in spec.split(separator: ",") {
            let parts = item.split(separator: ":")
            guard parts.count == 3, let t = Double(parts[0]) else { NSLog("ReplayNES: bad --inject-pad item \(item)"); continue }
            let element = String(parts[1]), down = parts[2] == "d"
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { [weak self] in self?.input.injectController(element, down) }
        }
    }

    func scheduleTestActions(_ spec: String) {
        for item in spec.split(separator: ",") {
            let parts = item.split(separator: ":").map(String.init)
            guard parts.count >= 2, let t = Double(parts[0]) else { NSLog("ReplayNES: bad --test-actions item \(item)"); continue }
            let n = parts.count > 2 ? Int(parts[2]) ?? 0 : 0
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { [weak self] in
                guard let self else { return }
                switch parts[1] {
                case "setA": self.practiceSetA(n)
                case "setB": self.practiceSetB(n)
                case "practice": self.practiceStart(n)
                case "stopPractice": self.practiceStop()
                case "toggleRecord": self.toggleRecord()
                case "togglePause": self.togglePause()
                case "panel": self.quickMenu.open(at: .practice)
                case "fill": self.integerScale = false
                case "integer": self.integerScale = true
                case "quit": NSApp.terminate(nil)
                case "fullscreen": self.mainWindow?.toggleFullScreen(nil)
                case "crtOn": CRTSettingsModel.shared.launchOverride = true
                case "crtOff": CRTSettingsModel.shared.enabled = false
                case "export": self.showExport = true
                case "latency": self.showLatency.toggle()
                case "open" where parts.count > 2:
                    switch parts[2] {
                    case "library": self.showLibraryScreen()
                    case "takes": self.quickMenu.open(at: .takes)
                    case "guide": self.quickMenu.open(at: .controller)
                    default: self.quickMenu.open(at: .display)
                    }
                case "menu" where parts.count > 2:
                    if let page = QMPage(rawValue: parts[2]) { self.quickMenu.open(at: page) } else { NSLog("ReplayNES: unknown menu page \(parts[2])") }
                case "menuClose": self.quickMenu.close()
                case "libraryClose": self.hideLibraryScreen()
                case "nav" where parts.count > 2:
                    let inputs: [String: NavInput] = ["up": .up, "down": .down, "left": .left, "right": .right, "confirm": .confirm,
                                                      "back": .back, "x": .x, "y": .y, "pagePrev": .pagePrev, "pageNext": .pageNext,
                                                      "menu": .menu, "escape": .escape, "options": .options, "search": .search]
                    if let i = inputs[parts[2]] { self.handleNav(i) }
                case "windowSize" where parts.count > 2:
                    let wh = parts[2].split(separator: "x").compactMap { Double($0) }
                    if wh.count == 2, let w = self.mainWindow { w.setContentSize(NSSize(width: wh[0], height: wh[1])) }
                case "seek": self.seek(to: UInt64(max(0, n)))
                case "bookmark": self.addBookmark()
                case "undoTake": self.undoTake()
                case "resetProject":  // n = 1: keep the A/B sections; a saved project is backed up (resetBackupFolder)
                    let saved = self.current.map { !$0.isTemp && !$0.projectPath.isEmpty } ?? false
                    self.performProjectReset(keepPracticeSlots: n != 0, backup: saved)
                case "resetPrompt": self.resetProjectPrompt()
                case "windowWidth":
                    if let w = self.mainWindow { var f = w.frame; f.size.width = CGFloat(n); w.setFrame(f, display: true) }
                case "dumpLayers": Self.dumpWindowLayers()
                case "dumpMenus": Self.dumpMenus()
                case "screen":   // move the main window to NSScreen.screens[n] (0: the menu-bar screen)
                    let screens = NSScreen.screens
                    if let w = self.mainWindow, !screens.isEmpty {
                        let f = screens[max(0, min(n, screens.count - 1))].visibleFrame
                        w.setFrame(NSRect(x: f.minX + 40, y: f.maxY - 40 - w.frame.height, width: w.frame.width, height: w.frame.height), display: true)
                    }
                default: NSLog("ReplayNES: unknown test action \(parts[1])")
                }
            }
        }
    }

    /// Logs every item of the menu bar with its key equivalent (scripts/ui-check-macos.sh checks ⌘W).
    static func dumpMenus() {
        var out = "ReplayNES menus:\n"
        for top in NSApp.mainMenu?.items ?? [] {
            for i in top.submenu?.items ?? [] where !i.isSeparatorItem {
                let mods = i.keyEquivalentModifierMask
                let key = i.keyEquivalent.isEmpty ? "" : (mods.contains(.shift) ? "⇧" : "") + (mods.contains(.option) ? "⌥" : "")
                    + (mods.contains(.command) ? "⌘" : "") + i.keyEquivalent
                out += "menu: \(top.title) | \(i.title) | \(key)\n"
            }
        }
        NSLog("%@", out)
    }

    /// Logs every window of the app and the main window's layer tree (direct-to-display checks:
    /// anything visible above or beside the game layer makes the window server composite it).
    static func dumpWindowLayers() {
        var out = "ReplayNES layers:\n"
        for w in NSApp.windows {
            out += "window \(type(of: w)) #\(w.windowNumber) frame \(w.frame) level \(w.level.rawValue) visible \(w.isVisible) alpha \(w.alphaValue) opaque \(w.isOpaque) key \(w.isKeyWindow) screen \(w.screen?.frame ?? .zero)\n"
        }
        func walk(_ l: CALayer, _ depth: Int, _ root: CALayer) {
            let f = l.convert(l.bounds, to: root)
            let pad = String(repeating: "  ", count: depth)
            out += "\(pad)\(type(of: l)) \(l.name ?? "") frame \(f) hidden \(l.isHidden) opacity \(l.opacity) opaque \(l.isOpaque) contents \(l.contents != nil) bg \(l.backgroundColor.map { "\($0.alpha)" } ?? "-") mask \(l.mask != nil) clips \(l.masksToBounds) corner \(l.cornerRadius) filters \(l.filters?.count ?? 0)/\(l.backgroundFilters?.count ?? 0) comp \(String(describing: l.compositingFilter)) transform \(CATransform3DIsIdentity(l.transform)) delegate \(l.delegate.map { "\(type(of: $0))" } ?? "-")\n"
            for s in l.sublayers ?? [] { walk(s, depth + 1, root) }
        }
        for w in NSApp.windows where w.isVisible {
            if let root = w.contentView?.superview?.layer ?? w.contentView?.layer {
                out += "-- tree of #\(w.windowNumber)\n"
                walk(root, 0, root)
            }
        }
        NSLog("%@", out)
    }

    func scheduleSnapshots(_ spec: String) {
        for item in spec.split(separator: ",") {
            guard let colon = item.firstIndex(of: ":"), let t = Double(item[..<colon]) else { continue }
            let path = String(item[item.index(after: colon)...])
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { [weak self] in
                self?.writeSnapshot(to: URL(fileURLWithPath: path))
            }
        }
    }

    func scheduleWindowSnapshots(_ spec: String) {
        for item in spec.split(separator: ",") {
            guard let colon = item.firstIndex(of: ":"), let t = Double(item[..<colon]) else { continue }
            let prefix = String(item[item.index(after: colon)...])
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { Self.captureVisibleWindows(prefix: prefix) }
        }
    }

    /// Every visible window and sheet through cacheDisplay (no Metal content; for UI text checks).
    static func captureVisibleWindows(prefix: String) {
        for (i, w) in NSApp.windows.enumerated() where w.isVisible {
            guard let view = w.contentView?.superview ?? w.contentView,
                  let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { continue }
            view.cacheDisplay(in: view.bounds, to: rep)
            let title = w.title.isEmpty ? "untitled" : w.title.replacingOccurrences(of: "/", with: "_")
            try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: "\(prefix)-\(i)-\(title).png"))
        }
    }

    /// --menu-walk: every Quick Menu page (and the second page of paged ones), then the paused seek
    /// bar and the library, one snapshot each; menu-walk.json says whether each menu page fit.
    func scheduleMenuWalk(dir: URL, delay: Double) {
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var steps: [(String, () -> Void)] = []
        for p in QMPage.allCases {
            steps.append((p.rawValue, { [weak self] in self?.quickMenu.open(at: p) }))
            if p.paged {
                steps.append((p.rawValue + "-2", { [weak self] in self?.quickMenu.open(at: p); self?.quickMenu.handle(.pageNext) }))
            }
        }
        steps.append(("seekbar", { [weak self] in self?.quickMenu.close(resume: false) }))
        steps.append(("library", { [weak self] in self?.showLibraryScreen() }))
        var results: [[String: Any]] = []
        func run(_ i: Int) {
            guard i < steps.count else {
                if let data = try? JSONSerialization.data(withJSONObject: ["pages": results], options: [.prettyPrinted, .sortedKeys]) {
                    try? data.write(to: dir.appendingPathComponent("menu-walk.json"))
                }
                if ProcessInfo.processInfo.arguments.contains("--quit-after-snapshot") { shutdown(); exit(0) }
                return
            }
            let (name, act) = steps[i]
            act()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in
                guard let self else { return }
                let menuPage = self.quickMenu.isOpen ? self.quickMenu.page.rawValue : ""
                let probe = QuickMenuProbe.shared
                var r: [String: Any] = ["name": name, "menuOpen": self.quickMenu.isOpen, "page": menuPage]
                if self.quickMenu.isOpen {
                    r["fits"] = probe.page == self.quickMenu.page && probe.fits
                    r["panelHeight"] = Double(probe.panelHeight)
                    r["areaHeight"] = Double(probe.areaHeight)
                }
                results.append(r)
                self.writeSnapshot(to: dir.appendingPathComponent(String(format: "%02d-%@.png", i, name))) { run(i + 1) }
            }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { run(0) }
    }

    func scheduleLibraryPlay(_ name: String, attempts: Int = 50) {
        if let rom = library.roms.first(where: { $0.name == name || $0.relativePath == name }) {
            playFromLibrary(rom)
            return
        }
        guard attempts > 0 else { NSLog("ReplayNES: --library-play: ROM \(name) not found"); return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in self?.scheduleLibraryPlay(name, attempts: attempts - 1) }
    }

    /// Diagnostics for the snapshot JSON: can keyboard input reach the game right now?
    var keyboardDiagnostics: [String: Any] {
        let key = NSApp.keyWindow
        return [
            "appActive": NSApp.isActive,
            "keyWindowIsMain": key != nil && key === mainWindow,
            "firstResponder": key?.firstResponder.map { String(describing: type(of: $0)) } ?? "nil",
            "keyboardEnabled": input.keyboardEnabled(),
        ]
    }
}
