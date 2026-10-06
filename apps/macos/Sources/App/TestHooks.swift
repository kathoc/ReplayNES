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
//                                                toggleRecord, togglePause, panel, fill, integer.
//   --snapshot-at "<sec>:<png>,..."              extra window snapshots (see Snapshot.swift).
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
                case "panel": self.showPracticePanel.toggle()
                case "fill": self.integerScale = false
                case "integer": self.integerScale = true
                default: NSLog("ReplayNES: unknown test action \(parts[1])")
                }
            }
        }
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
