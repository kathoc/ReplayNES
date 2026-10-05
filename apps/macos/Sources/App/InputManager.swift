// Keyboard (NSEvent) + GameController.framework -> rn_input (engine input pipeline).
// The engine does mapping, turbo, SOCD and hotkey separation; this file only reports
// physical state changes with stable ids ("kb:<keyCode>", "gc<slot>:<element>").
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import GameController

final class InputManager {
    let handle: OpaquePointer // rn_input (internally synchronized)

    /// Called (main thread) with the physical id while capturing a binding. Esc cancels (nil).
    var captureHandler: ((String?) -> Void)? {
        didSet { captureLock.lock(); capturing = captureHandler != nil; captureLock.unlock() }
    }
    private let captureLock = NSLock()
    private var capturing = false
    private var isCapturing: Bool { captureLock.lock(); defer { captureLock.unlock() }; return capturing }
    /// Main thread: controller list changed.
    var onControllersChanged: (([String]) -> Void)?
    /// Main thread: a controller was disconnected (frontend pauses).
    var onDisconnect: ((String) -> Void)?
    /// Main thread: binding/turbo/SOCD configuration changed.
    var onConfigChanged: ((InputCatalog.Config) -> Void)?

    /// Whether keyboard events should reach the game (false while a text field has focus, etc).
    var keyboardEnabled: () -> Bool = { true }

    private var monitor: Any?
    private var slots: [GCController?] = [nil, nil, nil, nil]
    private let gcQueue = DispatchQueue(label: "replaynes.gamecontroller", qos: .userInteractive)
    private var lastState: [String: Bool] = [:] // gcQueue only
    private let eventLock = NSLock()
    private var eventTime: UInt64 = 0
    private var eventSeq: UInt64 = 0
    private var consumedSeq: UInt64 = 0

    static var configURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("ReplayNES", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("bindings.json")
    }

    init() {
        handle = rn_input_new()!
        loadConfig()
    }

    deinit { rn_input_free(handle) }

    // MARK: configuration (persisted as the engine's own JSON in Application Support)

    private func loadConfig() {
        if let text = try? String(contentsOf: Self.configURL, encoding: .utf8),
           rn_input_load_json(handle, text) == RN_OK {
            return
        }
        _ = rn_input_load_json(handle, InputCatalog.defaultConfigJSON())
    }

    var config: InputCatalog.Config {
        guard let p = rn_input_save_json(handle) else { return InputCatalog.parse(InputCatalog.defaultConfigJSON())! }
        defer { rn_string_free(p) }
        return InputCatalog.parse(String(cString: p)) ?? InputCatalog.parse(InputCatalog.defaultConfigJSON())!
    }

    private func persist() {
        if let p = rn_input_save_json(handle) {
            let text = String(cString: p)
            rn_string_free(p)
            do {
                try text.write(to: Self.configURL, atomically: true, encoding: .utf8)
            } catch {
                NSLog("ReplayNES: failed to save bindings: \(error)")
            }
        }
        onConfigChanged?(config)
    }

    func bind(_ physical: String, to action: String) {
        _ = rn_input_bind(handle, physical, action)
        persist()
    }

    func unbind(_ physical: String, from action: String) {
        _ = rn_input_unbind(handle, physical, action)
        persist()
    }

    func resetToDefaults() {
        _ = rn_input_load_json(handle, InputCatalog.defaultConfigJSON())
        persist()
    }

    func setTurbo(period: Int, duty: Int) {
        let p = max(1, period), d = min(max(1, duty), p)
        _ = rn_input_set_turbo(handle, UInt32(p), UInt32(d))
        persist()
    }

    func setSOCD(_ policy: String) {
        let v: rn_socd = policy == "last_wins" ? RN_SOCD_LAST_WINS : policy == "allow" ? RN_SOCD_ALLOW : RN_SOCD_NEUTRAL
        _ = rn_input_set_socd(handle, v)
        persist()
    }

    func setAnalogThreshold(_ t: Double) {
        _ = rn_input_set_analog_threshold(handle, Float(min(max(t, 0.05), 0.95)))
        persist()
    }

    // MARK: latency bookkeeping

    private func noteEvent() {
        let t = HostClock.now()
        eventLock.lock(); eventTime = t; eventSeq &+= 1; eventLock.unlock()
    }

    /// Emulation thread: time of the latest physical change not yet consumed by a frame (0 = none).
    func consumeEventTime() -> UInt64 {
        eventLock.lock()
        defer { eventLock.unlock() }
        if eventSeq == consumedSeq { return 0 }
        consumedSeq = eventSeq
        return eventTime
    }

    private func setPressed(_ id: String, _ down: Bool) {
        rn_input_set_pressed(handle, id, down ? 1 : 0)
        noteEvent()
    }

    // MARK: keyboard

    private static let modifierMasks: [UInt16: UInt] = [
        56: 0x02, 60: 0x04,          // left / right shift
        59: 0x01, 62: 0x2000,        // left / right control
        58: 0x20, 61: 0x40,          // left / right option
        55: 0x08, 54: 0x10,          // left / right command
    ]

    func startKeyboard() {
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] ev in
            guard let self else { return ev }
            return self.handleKey(ev)
        }
        NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            rn_input_release_prefix(self.handle, "kb:")
        }
    }

    private func handleKey(_ ev: NSEvent) -> NSEvent? {
        let id = "kb:\(ev.keyCode)"
        if let capture = captureHandler {
            switch ev.type {
            case .keyDown:
                if ev.keyCode == 53 { captureHandler = nil; capture(nil); return nil } // Esc
                captureHandler = nil
                capture(id)
                return nil
            case .flagsChanged:
                if let mask = Self.modifierMasks[ev.keyCode], ev.modifierFlags.rawValue & mask != 0, ev.keyCode != 55, ev.keyCode != 54 {
                    captureHandler = nil
                    capture(id)
                }
                return nil
            default:
                return nil
            }
        }
        switch ev.type {
        case .keyUp:
            setPressed(id, false)
            return keyboardEnabled() ? nil : ev
        case .keyDown:
            // Command shortcuts belong to the menu; text fields keep their keys.
            if ev.modifierFlags.contains(.command) || !keyboardEnabled() { return ev }
            if !ev.isARepeat { setPressed(id, true) }
            return nil
        case .flagsChanged:
            if let mask = Self.modifierMasks[ev.keyCode] {
                setPressed(id, ev.modifierFlags.rawValue & mask != 0)
            }
            return ev
        default:
            return ev
        }
    }

    // MARK: game controllers

    func startControllers() {
        let nc = NotificationCenter.default
        nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.attach(c) }
        }
        nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.detach(c) }
        }
        GCController.controllers().forEach { attach($0) }
        GCController.startWirelessControllerDiscovery {}
    }

    private func attach(_ c: GCController) {
        if slots.contains(where: { $0 === c }) { return }
        guard let slot = slots.firstIndex(where: { $0 == nil }) else { return }
        slots[slot] = c
        c.playerIndex = GCControllerPlayerIndex(rawValue: slot) ?? .indexUnset
        c.handlerQueue = gcQueue
        let prefix = "gc\(slot):"
        if let g = c.extendedGamepad {
            g.valueChangedHandler = { [weak self] pad, _ in self?.update(extended: pad, prefix: prefix) }
        } else if let m = c.microGamepad {
            m.reportsAbsoluteDpadValues = true
            m.valueChangedHandler = { [weak self] pad, _ in self?.update(micro: pad, prefix: prefix) }
        }
        publishControllers()
    }

    private func detach(_ c: GCController) {
        guard let slot = slots.firstIndex(where: { $0 === c }) else { return }
        slots[slot] = nil
        let prefix = "gc\(slot):"
        rn_input_release_prefix(handle, prefix)
        gcQueue.async { [weak self] in self?.lastState = self?.lastState.filter { !$0.key.hasPrefix(prefix) } ?? [:] }
        publishControllers()
        onDisconnect?(c.vendorName ?? "コントローラー")
    }

    private func publishControllers() {
        let names = slots.enumerated().compactMap { i, c in c.map { "パッド\(i + 1): \($0.vendorName ?? "コントローラー")" } }
        onControllersChanged?(names)
    }

    /// gcQueue
    private func set(_ prefix: String, _ name: String, _ down: Bool) {
        let id = prefix + name
        if lastState[id] == down { return }
        lastState[id] = down
        if down, isCapturing {
            DispatchQueue.main.async { [weak self] in
                guard let self, let cap = self.captureHandler else { return }
                self.captureHandler = nil
                cap(id)
            }
            return
        }
        setPressed(id, down)
    }

    private func stick(_ prefix: String, _ name: String, _ x: Float, _ y: Float) {
        rn_input_set_axis(handle, prefix + name, x, y)
        // Track virtual direction ids for change detection / capture (threshold 0.5).
        let t: Float = 0.5
        for (dir, on) in [("left", x <= -t), ("right", x >= t), ("up", y >= t), ("down", y <= -t)] {
            let id = prefix + name + "." + dir
            if lastState[id] == on { continue }
            lastState[id] = on
            if on, isCapturing {
                DispatchQueue.main.async { [weak self] in
                    guard let self, let cap = self.captureHandler else { return }
                    self.captureHandler = nil
                    cap(id)
                }
            } else {
                noteEvent()
            }
        }
    }

    private func update(extended g: GCExtendedGamepad, prefix p: String) {
        set(p, "buttonA", g.buttonA.isPressed)
        set(p, "buttonB", g.buttonB.isPressed)
        set(p, "buttonX", g.buttonX.isPressed)
        set(p, "buttonY", g.buttonY.isPressed)
        set(p, "dpad.up", g.dpad.up.isPressed)
        set(p, "dpad.down", g.dpad.down.isPressed)
        set(p, "dpad.left", g.dpad.left.isPressed)
        set(p, "dpad.right", g.dpad.right.isPressed)
        set(p, "leftShoulder", g.leftShoulder.isPressed)
        set(p, "rightShoulder", g.rightShoulder.isPressed)
        set(p, "leftTrigger", g.leftTrigger.isPressed)
        set(p, "rightTrigger", g.rightTrigger.isPressed)
        set(p, "menu", g.buttonMenu.isPressed)
        if let o = g.buttonOptions { set(p, "options", o.isPressed) }
        if let h = g.buttonHome { set(p, "home", h.isPressed) }
        if let l3 = g.leftThumbstickButton { set(p, "leftThumb", l3.isPressed) }
        if let r3 = g.rightThumbstickButton { set(p, "rightThumb", r3.isPressed) }
        stick(p, "lstick", g.leftThumbstick.xAxis.value, g.leftThumbstick.yAxis.value)
        stick(p, "rstick", g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value)
    }

    private func update(micro g: GCMicroGamepad, prefix p: String) {
        set(p, "buttonA", g.buttonA.isPressed)
        set(p, "buttonX", g.buttonX.isPressed)
        set(p, "menu", g.buttonMenu.isPressed)
        set(p, "dpad.up", g.dpad.up.isPressed)
        set(p, "dpad.down", g.dpad.down.isPressed)
        set(p, "dpad.left", g.dpad.left.isPressed)
        set(p, "dpad.right", g.dpad.right.isPressed)
    }
}
