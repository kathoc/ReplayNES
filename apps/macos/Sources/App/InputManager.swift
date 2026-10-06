// Keyboard (NSEvent) + GameController.framework -> rn_input (engine input pipeline).
// The engine does mapping, turbo, SOCD and hotkey separation; this file only reports
// physical state changes with stable ids ("kb:<keyCode>", "gc<slot>:<element>").
// Face buttons are reported by position ("gc0:face.east"), see ControllerLayout.swift.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import GameController

/// Main-thread state for the controller diagram in Settings: connected controllers and, while a
/// diagram is visible (`setLive`), the controller ids currently held.
final class ControllerMonitor: ObservableObject {
    @Published fileprivate(set) var controllers: [ControllerInfo] = []
    @Published fileprivate(set) var pressed: Set<String> = []
    private let lock = NSLock()
    private var live = false

    var isLive: Bool { lock.lock(); defer { lock.unlock() }; return live }

    func setLive(_ on: Bool) {
        lock.lock(); live = on; lock.unlock()
        if !on { pressed = [] }
    }
}

final class InputManager {
    let handle: OpaquePointer // rn_input (internally synchronized)
    let controllerMonitor = ControllerMonitor()

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
    /// gcQueue: paused D-pad frame stepping (direction -1/+1, pressed). Wired to the emulation thread.
    var onPausedStep: ((Int, Bool) -> Void)?

    // Paused stepping: while the emulation is paused, controller D-pad ←/→ step frames and are
    // NOT sent to the game (so a frame advance never records them).
    private let stepLock = NSLock()
    private var stepModeOn = false              // stepLock
    private var stepEnabled = true              // stepLock (setting)
    private var stepDirs: [String: Int] = [:]   // stepLock
    private var routedSteps: Set<String> = []   // gcQueue only

    private var monitor: Any?
    private var slots: [GCController?] = [nil, nil, nil, nil]
    private var infos: [ControllerInfo?] = [nil, nil, nil, nil] // main thread
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
        defer { refreshStepDirections() }
        if let text = try? String(contentsOf: Self.configURL, encoding: .utf8),
           rn_input_load_json(handle, text) == RN_OK {
            migrateControllerLayout()
            return
        }
        _ = rn_input_load_json(handle, InputCatalog.defaultConfigJSON())
        UserDefaults.standard.set(InputCatalog.controllerLayoutVersion, forKey: "controllerLayoutVersion")
    }

    /// One-time upgrades of saved bindings, unless the user customised the part concerned:
    /// layout 2 (0.2.0): 0.1.x controller hotkeys (L1 rewind, R1 FF, L2/R2 step) -> new hotkeys.
    /// layout 3: GameController face-button names -> positional ids (fixes A/B, X/Y on Nintendo
    /// controllers). Customised face buttons are translated when their controller attaches.
    private func migrateControllerLayout() {
        let d = UserDefaults.standard
        let from = d.integer(forKey: "controllerLayoutVersion")
        guard from < InputCatalog.controllerLayoutVersion else { return }
        d.set(InputCatalog.controllerLayoutVersion, forKey: "controllerLayoutVersion")
        var changed = false
        if from < 2 { changed = apply(InputCatalog.controllerLayoutMigration(config)) || changed }
        if from < 3 { changed = apply(InputCatalog.faceLayoutMigration(config)) || changed }
        guard changed else { return }
        if let p = rn_input_save_json(handle) {
            try? String(cString: p).write(to: Self.configURL, atomically: true, encoding: .utf8)
            rn_string_free(p)
        }
    }

    /// Applies an (unbind, bind) plan to the engine table without saving. Returns whether it did anything.
    @discardableResult
    private func apply(_ plan: (unbind: [(String, String)], bind: [(String, String)])) -> Bool {
        for (i, a) in plan.unbind { _ = rn_input_unbind(handle, i, a) }
        for (i, a) in plan.bind { _ = rn_input_bind(handle, i, a) }
        return !plan.unbind.isEmpty || !plan.bind.isEmpty
    }

    private func refreshStepDirections() {
        let dirs = InputCatalog.pausedStepDirections(config)
        stepLock.lock(); stepDirs = dirs; stepLock.unlock()
    }

    /// Setting 「一時停止中は十字キー←→でコマ送り」.
    func setPausedStepEnabled(_ on: Bool) {
        stepLock.lock(); stepEnabled = on; stepLock.unlock()
    }

    /// Emulation thread: paused state changed. Entering pause releases D-pad ←/→ held for the
    /// game so a following frame advance does not record them.
    func setPausedStepMode(_ on: Bool) {
        stepLock.lock()
        let changed = stepModeOn != on
        stepModeOn = on
        let ids = Array(stepDirs.keys)
        stepLock.unlock()
        guard changed, on else { return }
        gcQueue.async { [weak self] in
            guard let self else { return }
            for id in ids where self.lastState[id] == true && !self.routedSteps.contains(id) {
                rn_input_set_pressed(self.handle, id, 0)
                self.routedSteps.insert(id) // its release is then swallowed too
            }
        }
    }

    /// gcQueue: returns true if the change was consumed by paused stepping.
    private func routePausedStep(_ id: String, _ down: Bool) -> Bool {
        stepLock.lock()
        let dir = stepDirs[id]
        let active = stepModeOn && stepEnabled
        stepLock.unlock()
        guard let dir else { return false }
        if down {
            guard active else { return false }
            routedSteps.insert(id)
            onPausedStep?(dir, true)
            return true
        }
        if routedSteps.remove(id) != nil {
            onPausedStep?(dir, false)
            return true
        }
        return false
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
        refreshStepDirections()
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

    /// Diagram: `physical` does exactly `action` from now on (nil = nothing).
    func setAssignment(_ physical: String, action: String?) {
        _ = rn_input_unbind(handle, physical, nil)
        if let action { _ = rn_input_bind(handle, physical, action) }
        persist()
    }

    /// 「初期設定に戻す」 for one controller slot only (keyboard and other pads untouched).
    func resetController(slot: Int) {
        apply(InputCatalog.controllerResetPlan(config, slot: slot))
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

    private var pressSeq: UInt64 = 0 // guarded by eventLock

    /// Count of physical key/button presses so far (any thread). Lets the emulation thread
    /// notice "a button was pressed while paused" without sampling (sampling consumes latches).
    var pressSequence: UInt64 { eventLock.lock(); defer { eventLock.unlock() }; return pressSeq }

    private func setPressed(_ id: String, _ down: Bool) {
        rn_input_set_pressed(handle, id, down ? 1 : 0)
        if down { eventLock.lock(); pressSeq &+= 1; eventLock.unlock() }
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
        // Controller input keeps working while another app (e.g. OBS) is frontmost. The keyboard
        // stays foreground-only (local event monitor).
        GCController.shouldMonitorBackgroundEvents = true
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

    /// Positional element names for GameController's buttonA/B/X/Y on one controller.
    private struct FaceNames {
        let a: String, b: String, x: String, y: String
        init(_ p: [String: FacePosition]) {
            a = (p["buttonA"] ?? .south).element
            b = (p["buttonB"] ?? .east).element
            x = (p["buttonX"] ?? .west).element
            y = (p["buttonY"] ?? .north).element
        }
    }

    private func attach(_ c: GCController) {
        if slots.contains(where: { $0 === c }) { return }
        guard let slot = slots.firstIndex(where: { $0 == nil }) else { return }
        slots[slot] = c
        c.playerIndex = GCControllerPlayerIndex(rawValue: slot) ?? .indexUnset
        c.handlerQueue = gcQueue
        let prefix = "gc\(slot):"
        let category = c.productCategory
        let family = ControllerFamily.from(productCategory: category, vendorName: c.vendorName)
        let positions = c.extendedGamepad != nil
            ? GCFaceMapping.positions(productCategory: category, vendorName: c.vendorName) : GCFaceMapping.micro
        let face = FaceNames(positions)
        var labels: [String: String] = [:]
        if let g = c.extendedGamepad {
            g.valueChangedHandler = { [weak self] pad, _ in self?.update(extended: pad, prefix: prefix, face: face) }
            for (name, button) in [("buttonA", g.buttonA), ("buttonB", g.buttonB), ("buttonX", g.buttonX), ("buttonY", g.buttonY)] {
                if let pos = positions[name], let l = GCFaceMapping.label(fromSymbol: button.sfSymbolsName) { labels[pos.element] = l }
            }
        } else if let m = c.microGamepad {
            m.reportsAbsoluteDpadValues = true
            m.valueChangedHandler = { [weak self] pad, _ in self?.update(micro: pad, prefix: prefix, face: face) }
        }
        NSLog("ReplayNES: controller \(slot + 1) \"\(c.vendorName ?? "?")\" category=\"\(category)\" family=\(family.rawValue) buttonA=\(face.a)")
        infos[slot] = ControllerInfo(slot: slot, name: c.vendorName ?? "コントローラー", productCategory: category, family: family, labels: labels)
        // Customised bindings saved with GameController names (layout 2) keep their meaning on
        // this controller.
        if apply(InputCatalog.legacyFaceTranslation(config, slot: slot, positions: positions)) { persist() }
        publishControllers()
    }

    private func detach(_ c: GCController) {
        guard let slot = slots.firstIndex(where: { $0 === c }) else { return }
        slots[slot] = nil
        infos[slot] = nil
        let prefix = "gc\(slot):"
        rn_input_release_prefix(handle, prefix)
        gcQueue.async { [weak self] in
            guard let self else { return }
            self.lastState = self.lastState.filter { !$0.key.hasPrefix(prefix) }
            for id in self.routedSteps where id.hasPrefix(prefix) {
                self.routedSteps.remove(id)
                self.stepLock.lock(); let d = self.stepDirs[id]; self.stepLock.unlock()
                if let d { self.onPausedStep?(d, false) }
            }
        }
        publishControllers()
        onDisconnect?(c.vendorName ?? "コントローラー")
    }

    private func publishControllers() {
        let names = slots.enumerated().compactMap { i, c in c.map { "パッド\(i + 1): \($0.vendorName ?? "コントローラー")" } }
        controllerMonitor.controllers = infos.compactMap { $0 }
        onControllersChanged?(names)
    }

    /// gcQueue: live highlight for the settings diagram (only while it is visible).
    private func notePressed(_ id: String, _ down: Bool) {
        guard controllerMonitor.isLive else { return }
        DispatchQueue.main.async { [weak self] in
            guard let m = self?.controllerMonitor, m.isLive else { return }
            if down { m.pressed.insert(id) } else { m.pressed.remove(id) }
        }
    }

    /// gcQueue
    private func set(_ prefix: String, _ name: String, _ down: Bool) {
        let id = prefix + name
        if lastState[id] == down { return }
        lastState[id] = down
        notePressed(id, down)
        if down, isCapturing {
            DispatchQueue.main.async { [weak self] in
                guard let self, let cap = self.captureHandler else { return }
                self.captureHandler = nil
                cap(id)
            }
            return
        }
        if routePausedStep(id, down) { return }
        setPressed(id, down)
    }

    /// Test hook (--inject-pad): a controller element change on slot 0, through the same path
    /// as a real controller (hotkeys, paused stepping, game input).
    func injectController(_ element: String, _ down: Bool) {
        gcQueue.async { [weak self] in self?.set("gc0:", element, down) }
    }

    private func stick(_ prefix: String, _ name: String, _ x: Float, _ y: Float) {
        rn_input_set_axis(handle, prefix + name, x, y)
        // Track virtual direction ids for change detection / capture (threshold 0.5).
        let t: Float = 0.5
        for (dir, on) in [("left", x <= -t), ("right", x >= t), ("up", y >= t), ("down", y <= -t)] {
            let id = prefix + name + "." + dir
            if lastState[id] == on { continue }
            lastState[id] = on
            notePressed(id, on)
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

    private func update(extended g: GCExtendedGamepad, prefix p: String, face: FaceNames) {
        set(p, face.a, g.buttonA.isPressed)
        set(p, face.b, g.buttonB.isPressed)
        set(p, face.x, g.buttonX.isPressed)
        set(p, face.y, g.buttonY.isPressed)
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

    private func update(micro g: GCMicroGamepad, prefix p: String, face: FaceNames) {
        set(p, face.a, g.buttonA.isPressed)
        set(p, face.x, g.buttonX.isPressed)
        set(p, "menu", g.buttonMenu.isPressed)
        set(p, "dpad.up", g.dpad.up.isPressed)
        set(p, "dpad.down", g.dpad.down.isPressed)
        set(p, "dpad.left", g.dpad.left.isPressed)
        set(p, "dpad.right", g.dpad.right.isPressed)
    }
}
