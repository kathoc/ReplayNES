// Scripted UI test hooks (launch arguments; used by smoke tests, harmless otherwise).
//   --inject-keys "<sec>:<keyCode>:<d|u>,..."   post synthetic key events to the main window at
//                                                the given times after launch. They travel the
//                                                same path as real keys (app event queue ->
//                                                InputManager's local monitor -> rn_input), so the
//                                                key-window / first-responder gating is exercised.
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
