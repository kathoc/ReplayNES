// Keyboard (NSEvent) + GameController.framework -> rn_input (engine input pipeline).
// The engine does mapping, turbo, SOCD and hotkey separation; this file only reports
// physical state changes with stable ids ("kb:<keyCode>", "gc<slot>:<element>").
// Face buttons are reported by position ("gc0:face.east"), see ControllerLayout.swift.
// The Quick Menu (docs/design/UI_REDESIGN.md, "Input model") is the action "hk.menu": pad 1's L+R
// through the shared chord detector and Esc. L or R alone act on their RELEASE (never in the way of
// the chord): their own bindings in play (pause / slow), previous / next page in menus, a frame
// step back / forward while paused (held >= 400 ms: repeated), a held marker move while a seek bar
// marker is edited. Menus confirm with the UI's confirm button (east by default, "southConfirm"
// swaps it) and go back with the cancel one. While paused in play (the seek bar) cancel tapped
// resumes, confirm tapped drops a marker, up tapped goes to the markers, Y tapped picks the next
// A/B slot (taps: they still reach the game, so a held button survives frame steps); with a marker
// focused the pad is the seek bar's (onSeekInput) and nothing reaches the game.
// Same rules as the Linux / Windows frontend (apps/desktop/src/input_router.cpp).
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

/// Menu / library navigation from a controller or the keyboard (docs/design/UI_REDESIGN.md,
/// "Input model"). `menu` = the L+R chord (open / close the Quick Menu); `escape` = the Esc key.
enum NavInput: Equatable {
    case up, down, left, right, confirm, back, x, y, pagePrev, pageNext, menu, escape
    case options  // View / Select / − (the library: next sort order)
    case search   // "/" (the library: search)
}

final class InputManager {
    let handle: OpaquePointer // rn_input (internally synchronized)
    let controllerMonitor = ControllerMonitor()

    /// Main thread: navigation input (menus open / library shown, the L+R chord and Esc always).
    var onNav: ((NavInput) -> Void)?
    /// Main thread: the UI's cancel button tapped while the game is paused = resume
    /// (ConfirmTap.swift: holding it for a frame advance never resumes).
    var onPausedResume: (() -> Void)?
    /// The paused seek bar's controller input: taps on the bar (ok, up, y: press only), everything
    /// while a marker is focused / edited (press and release). Main thread.
    enum SeekInput { case ok, cancel, up, down, left, right, x, y }
    var onSeekInput: ((SeekInput, Bool) -> Void)?
    /// Main thread: L / R alone while paused: a frame step (-1 / +1) at the release, repeated while held.
    var onPausedShoulder: ((Int) -> Void)?
    /// Main thread: L / R held while a marker is edited: the marker moves (-1 / +1) until released.
    var onMarkerHold: ((Int, Bool) -> Void)?

    // Navigation mode: the Quick Menu or the library is on screen. Controller and keyboard input
    // then drive it and never reach the game.
    private let navLock = NSLock()
    private var navOn = false                         // navLock
    private var isNavMode: Bool { navLock.lock(); defer { navLock.unlock() }; return navOn }
    private var navRepeat: [String: DispatchWorkItem] = [:]   // gcQueue only

    // The Quick Menu is the action "hk.menu": combos bound to it (pad 1's L+R) go through the shared
    // core's chord detector (ChordDetector.swift, gcQueue only); single inputs bound to it (Esc)
    // open it directly and never reach the game.
    private let chord = ChordDetector()
    /// Combo members bound to game input: held for the game from ALONE_DOWN (gcQueue only).
    private var gameMembers: Set<String> = []
    /// Combo members whose solo press was handed on (their release follows).
    private var forwardedMembers: Set<String> = []  // gcQueue only
    /// Combo members moving an edited marker now -> -1 / +1 (gcQueue only).
    private var holdMembers: [String: Int] = [:]
    private var chordTickWork: DispatchWorkItem?    // gcQueue only
    private var menuIDs: Set<String> = []           // stepLock
    /// Paused seek bar: cancel / confirm / up / Y taps (gcQueue only).
    private var confirmTap = ConfirmTap()
    /// Settings › Controls › Confirm Button: south confirms (default: east). gcQueue only.
    private var southConfirm = false
    /// The paused seek bar's focus: 0 = the bar, 1 = a marker, 2 = a marker being edited. gcQueue only.
    private var seekFocus = 0
    /// Presses taken by "press a key" (keys only): their releases are swallowed too. gcQueue only.
    private var captureSwallowed: Set<String> = []

    /// Called (main thread) with the physical id while capturing a binding (keys only). Esc and the
    /// controller's cancel button cancel (nil); other controller input is ignored meanwhile.
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
        defer { refreshBindingState() }
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
    /// layout 4: untouched trigger hotkeys swap to L2 rewind / R2 fast-forward.
    /// layout 5: the Quick Menu ("hk.menu"): pad 1's L+R and Esc (unless Esc is taken).
    /// layout 6: bindings to HOME / guide (never assignable) are dropped.
    private func migrateControllerLayout() {
        let d = UserDefaults.standard
        let from = d.integer(forKey: "controllerLayoutVersion")
        guard from < InputCatalog.controllerLayoutVersion else { return }
        d.set(InputCatalog.controllerLayoutVersion, forKey: "controllerLayoutVersion")
        var changed = false
        if from < 2 { changed = apply(InputCatalog.controllerLayoutMigration(config)) || changed }
        if from < 3 { changed = apply(InputCatalog.faceLayoutMigration(config)) || changed }
        if from < 4 { changed = apply(InputCatalog.triggerSwapMigration(config)) || changed }
        if from < 5 { changed = apply(InputCatalog.menuMigration(config)) || changed }
        if from < 6 { changed = apply(InputCatalog.homeMigration(config)) || changed }
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

    /// After the bindings changed: paused-step directions, the Quick Menu's inputs and combos.
    private func refreshBindingState() {
        let c = config
        let dirs = InputCatalog.pausedStepDirections(c)
        let menu = Set(c.bindings.filter { $0.action == "hk.menu" && !InputCatalog.isCombo($0.input) }.map(\.input))
        stepLock.lock(); stepDirs = dirs; menuIDs = menu; stepLock.unlock()
        gcQueue.async { [weak self] in
            guard let self else { return }
            // Reconfiguring drops the detector's state: members handed on are released here.
            self.releaseForwardedMembers()
            self.endMarkerHolds()
            self.chord.configure(c.bindings)
            // A combo member bound to a game button is held for the game (from the end of the window).
            self.gameMembers = Set(c.bindings.filter { self.chord.isMember($0.input) && !$0.action.hasPrefix("hk.") }.map(\.input))
            self.updateRepeat()
        }
    }

    /// gcQueue
    private func releaseForwardedMembers() {
        for id in forwardedMembers { setPressed(id, false) }
        forwardedMembers.removeAll()
    }

    /// gcQueue: members moving a marker stop (the marker stops).
    private func endMarkerHolds() {
        for (_, dir) in holdMembers { DispatchQueue.main.async { [weak self] in self?.onMarkerHold?(dir, false) } }
        holdMembers.removeAll()
    }

    /// gcQueue: paused frame steps repeat while L / R is held (not while a marker is edited: it moves).
    private func updateRepeat() {
        stepLock.lock(); let paused = stepModeOn; stepLock.unlock()
        let on = paused && !isNavMode && seekFocus < 2
        if chord.repeatMode != on {
            chord.repeatMode = on
            scheduleChordTick()
        }
    }

    /// Paused in play: the seek bar (not a menu / the library).
    private var isPausedInPlay: Bool {
        stepLock.lock(); let paused = stepModeOn; stepLock.unlock()
        return paused && !isNavMode
    }

    private var confirmElement: String { String(cString: rnf_ui_confirm_element(southConfirm ? 1 : 0)) }
    private var cancelElement: String { String(cString: rnf_ui_cancel_element(southConfirm ? 1 : 0)) }

    /// Settings › Controls › Confirm Button (any thread).
    func setSouthConfirm(_ on: Bool) {
        gcQueue.async { [weak self] in self?.southConfirm = on }
    }

    /// Main thread: the paused seek bar's focus (0 the bar, 1 a marker, 2 editing). Entering a marker
    /// releases what the game had held: the pad is the seek bar's then.
    func setSeekFocus(_ focus: Int) {
        gcQueue.async { [weak self] in
            guard let self, focus != self.seekFocus else { return }
            let entering = self.seekFocus == 0 && focus > 0
            self.seekFocus = focus
            if entering {
                rn_input_release_prefix(self.handle, "gc")
                rn_input_release_prefix(self.handle, "kb:")
                self.confirmTap.clear()
                self.forwardedMembers.removeAll()
                for id in self.routedSteps {
                    self.stepLock.lock(); let d = self.stepDirs[id]; self.stepLock.unlock()
                    if let d { self.onPausedStep?(d, false) }
                }
                self.routedSteps.removeAll()
            }
            if focus < 2 { self.endMarkerHolds() }
            self.updateRepeat()
        }
    }

    private func isMenuInput(_ id: String) -> Bool {
        stepLock.lock(); defer { stepLock.unlock() }
        return menuIDs.contains(id)
    }

    // MARK: navigation mode

    /// Main thread: the Quick Menu or the library appeared / went away. Entering releases what
    /// the game had held, so nothing stays pressed behind the menu.
    func setNavMode(_ on: Bool) {
        navLock.lock()
        let changed = navOn != on
        navOn = on
        navLock.unlock()
        guard changed else { return }
        if on {
            rn_input_release_prefix(handle, "gc")
            rn_input_release_prefix(handle, "kb:")
        }
        gcQueue.async { [weak self] in
            guard let self else { return }
            for w in self.navRepeat.values { w.cancel() }
            self.navRepeat.removeAll()
            self.updateRepeat()
            guard on else { return }
            self.confirmTap.clear()
            self.forwardedMembers.removeAll()
            self.endMarkerHolds()
            for id in self.routedSteps {
                self.stepLock.lock(); let d = self.stepDirs[id]; self.stepLock.unlock()
                if let d { self.onPausedStep?(d, false) }
            }
            self.routedSteps.removeAll()
        }
    }

    private func post(_ n: NavInput) {
        DispatchQueue.main.async { [weak self] in self?.onNav?(n) }
    }

    /// gcQueue: a controller element while navigating. Directions repeat while held.
    private func routeNav(_ id: String, _ name: String, _ down: Bool) {
        let dir: NavInput?
        switch name {
        case "dpad.up", "lstick.up": dir = .up
        case "dpad.down", "lstick.down": dir = .down
        case "dpad.left", "lstick.left": dir = .left
        case "dpad.right", "lstick.right": dir = .right
        default: dir = nil
        }
        if let dir {
            navRepeat[id]?.cancel()
            navRepeat[id] = nil
            guard down else { return }
            post(dir)
            scheduleNavRepeat(id, dir, delay: 0.35)
            return
        }
        // Pads 2-4 (pad 1's L / R come through the chord detector): pages, on the release.
        if !down {
            if name == "leftShoulder" { post(.pagePrev) } else if name == "rightShoulder" { post(.pageNext) }
            return
        }
        switch name {
        case confirmElement: post(.confirm)
        case cancelElement: post(.back)
        case "face.west": post(.x)
        case "face.north": post(.y)
        case "menu": post(.menu)
        case "options": post(.options)
        default: break
        }
    }

    private func scheduleNavRepeat(_ id: String, _ n: NavInput, delay: Double) {
        let w = DispatchWorkItem { [weak self] in
            guard let self, self.isNavMode, self.lastState[id] == true else { return }
            self.post(n)
            self.scheduleNavRepeat(id, n, delay: 0.09)
        }
        navRepeat[id] = w
        gcQueue.asyncAfter(deadline: .now() + delay, execute: w)
    }

    // MARK: L+R chord (gcQueue)

    private func now() -> Double { ProcessInfo.processInfo.systemUptime }

    /// One pending tick at the detector's next deadline (the window's end, a repeat).
    private func scheduleChordTick() {
        chordTickWork?.cancel()
        chordTickWork = nil
        guard let deadline = chord.nextDeadline else { return }
        let delay = max(0, deadline - now())
        let w = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.chordTickWork = nil
            self.emitChord(self.chord.tick(now: self.now()))
            self.scheduleChordTick()
        }
        chordTickWork = w
        gcQueue.asyncAfter(deadline: .now() + delay + 0.001, execute: w)
    }

    private func onMain(_ f: @escaping (InputManager) -> Void) {
        DispatchQueue.main.async { [weak self] in if let self { f(self) } }
    }

    /// gcQueue: the chord detector's outcome: the Quick Menu, or L / R alone (on their release).
    private func emitChord(_ events: [ChordEvent]) {
        for e in events {
            switch e {
            case .comboDown:
                post(.menu)
            case .comboUp:
                break
            case .alone(let id, true, let member, _):   // held alone: a game button is held, a marker moves
                let dir = member == 0 ? -1 : 1
                stepLock.lock(); let paused = stepModeOn; stepLock.unlock()
                if paused && !isNavMode && seekFocus == 2 {
                    holdMembers[id] = dir
                    onMain { $0.onMarkerHold?(dir, true) }
                } else if !isNavMode && !paused && gameMembers.contains(id) {
                    forwardedMembers.insert(id)
                    confirmTap.cancel()
                    setPressed(id, true)
                }
            case .repeated(_, let member):
                let dir = member == 0 ? -1 : 1
                if isPausedInPlay && seekFocus < 2 { onMain { $0.onPausedShoulder?(dir) } }
            case .alone(let id, false, let member, let repeats):   // the trigger of a single press
                let dir = member == 0 ? -1 : 1
                if holdMembers.removeValue(forKey: id) != nil {
                    onMain { $0.onMarkerHold?(dir, false) }
                } else if forwardedMembers.remove(id) != nil {
                    setPressed(id, false)
                } else if isNavMode {
                    post(dir < 0 ? .pagePrev : .pageNext)
                } else if isPausedInPlay {
                    if repeats == 0 { onMain { $0.onPausedShoulder?(dir) } }
                } else {
                    // A tap of its own action (pause, slow, ...): the hotkey fires on the press edge.
                    confirmTap.cancel()
                    setPressed(id, true)
                    setPressed(id, false)
                }
            }
        }
    }

    private enum PausedTap { case resume, seek(SeekInput) }

    /// gcQueue: paused in play (the seek bar), taps (ConfirmTap.swift) of the UI's cancel (resume),
    /// confirm (a marker), up (to the markers) and Y (the next A/B slot). They still reach the game,
    /// so a button can be held for a frame advance; holding one and stepping never counts as a tap.
    private func trackPausedTap(_ id: String, _ name: String, _ down: Bool) -> PausedTap? {
        if down {
            confirmTap.cancel()
            guard isPausedInPlay, [cancelElement, confirmElement, "dpad.up", "face.north"].contains(name) else { return nil }
            confirmTap.press(id)
            return nil
        }
        guard confirmTap.release(id), isPausedInPlay else { return nil }
        switch name {
        case cancelElement: return .resume
        case "dpad.up": return .seek(.up)
        case "face.north": return .seek(.y)
        default: return .seek(.ok)
        }
    }

    /// gcQueue: a marker focused / edited: the pad is the seek bar's (D-pad / left stick, confirm,
    /// cancel, X, Y); nothing reaches the game. L2 / R2 (rewind / fast-forward) keep working.
    private func routeSeek(_ name: String, _ down: Bool) -> Bool {
        guard seekFocus > 0, isPausedInPlay, name != "leftTrigger", name != "rightTrigger" else { return false }
        let input: SeekInput?
        switch name {
        case confirmElement: input = .ok
        case cancelElement: input = .cancel
        case "dpad.up", "lstick.up": input = .up
        case "dpad.down", "lstick.down": input = .down
        case "dpad.left", "lstick.left": input = .left
        case "dpad.right", "lstick.right": input = .right
        case "face.west": input = .x
        case "face.north": input = .y
        default: input = nil
        }
        if let input { onMain { $0.onSeekInput?(input, down) } }
        return true
    }

    /// Any thread: a key or a menu command (frame advance, ...) while a confirm button may be held.
    func cancelPausedConfirm() {
        gcQueue.async { [weak self] in self?.confirmTap.cancel() }
    }

    /// Setting "While paused, the D-pad ←/→ steps frames".
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
        guard changed else { return }
        gcQueue.async { [weak self] in
            guard let self else { return }
            self.updateRepeat()
            if !on { self.confirmTap.clear(); self.endMarkerHolds() }
            guard on else { return }
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
            confirmTap.cancel()
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
        refreshBindingState()
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

    /// "Reset to Defaults" for one controller slot only (keyboard and other pads untouched).
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

    /// `at`: the event's own timestamp (seconds on the host clock: NSEvent.timestamp, GameController
    /// lastEventTimestamp), so input->display latency includes any delay before it was handled.
    private func noteEvent(at: Double? = nil) {
        let now = HostClock.now()
        var t = now
        if let at, at > 0 { let ev = HostClock.ticks(seconds: at); if ev <= now { t = ev } }
        eventLock.lock(); eventTime = t; eventSeq &+= 1; eventLock.unlock()
    }

    /// gcQueue: timestamp of the controller event being handled.
    private var gcEventTime: Double?

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

    private func setPressed(_ id: String, _ down: Bool, at: Double? = nil) {
        rn_input_set_pressed(handle, id, down ? 1 : 0)
        if down { eventLock.lock(); pressSeq &+= 1; eventLock.unlock() }
        noteEvent(at: at)
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
            setPressed(id, false, at: ev.timestamp)
            return keyboardEnabled() ? nil : ev
        case .keyDown:
            // Any key (also ⌘→ frame advance) while a pad's confirm is held: not a resume tap.
            if !ev.isARepeat { cancelPausedConfirm() }
            // Command shortcuts belong to the menu; text fields keep their keys.
            if ev.modifierFlags.contains(.command) || !keyboardEnabled() { return ev }
            if isNavMode {
                if let n = Self.navKey(ev) {
                    // Arrows follow the key repeat; other keys act once per press.
                    if !ev.isARepeat || [.up, .down, .left, .right].contains(n) { onNav?(n) }
                } else if isMenuInput(id) && !ev.isARepeat {
                    onNav?(.menu)
                }
                return nil   // nothing reaches the game behind a menu
            }
            // Keys bound to the Quick Menu ("hk.menu": Esc by default) open it; never game input.
            if isMenuInput(id) {
                if !ev.isARepeat { onNav?(.menu) }
                return nil
            }
            if !ev.isARepeat { setPressed(id, true, at: ev.timestamp) }
            return nil
        case .flagsChanged:
            if let mask = Self.modifierMasks[ev.keyCode] {
                let down = ev.modifierFlags.rawValue & mask != 0
                if !down || !isNavMode { setPressed(id, down, at: ev.timestamp) }
            }
            return ev
        default:
            return ev
        }
    }

    /// Keys of menus / the library: arrows, Return / Space (confirm), Delete (back), Esc, Tab (pages).
    private static func navKey(_ ev: NSEvent) -> NavInput? {
        switch ev.keyCode {
        case 126: return .up
        case 125: return .down
        case 123: return .left
        case 124: return .right
        case 36, 76, 49: return .confirm
        case 51: return .back
        case 53: return .escape
        case 48: return ev.modifierFlags.contains(.shift) ? .pagePrev : .pageNext
        case 7: return .x
        case 16: return .y
        case 1: return .options   // S
        case 44: return .search   // /
        default: return nil
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

    private static func faceSymbols(_ c: GCController) -> [String: String?] {
        guard let g = c.extendedGamepad else { return [:] }
        return ["buttonA": g.buttonA.sfSymbolsName, "buttonB": g.buttonB.sfSymbolsName,
                "buttonX": g.buttonX.sfSymbolsName, "buttonY": g.buttonY.sfSymbolsName]
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
            ? GCFaceMapping.positions(productCategory: category, vendorName: c.vendorName, symbols: Self.faceSymbols(c))
            : GCFaceMapping.micro
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
        infos[slot] = ControllerInfo(slot: slot, name: c.vendorName ?? String(localized: "Controller"), productCategory: category, family: family, labels: labels)
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
            // The detector's state is shared by every pad: forget it and release what it handed on.
            self.chord.reset()
            self.releaseForwardedMembers()
            self.endMarkerHolds()
            self.confirmTap.clear()
            self.lastState = self.lastState.filter { !$0.key.hasPrefix(prefix) }
            for id in self.routedSteps where id.hasPrefix(prefix) {
                self.routedSteps.remove(id)
                self.stepLock.lock(); let d = self.stepDirs[id]; self.stepLock.unlock()
                if let d { self.onPausedStep?(d, false) }
            }
        }
        publishControllers()
        onDisconnect?(c.vendorName ?? String(localized: "Controller"))
    }

    private func publishControllers() {
        let names = slots.enumerated().compactMap { i, c in c.map { String(localized: "Pad \(i + 1): \($0.vendorName ?? String(localized: "Controller"))") } }
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
        // "Press a key": keys only; the controller's cancel button cancels, the rest waits.
        if down, isCapturing {
            captureSwallowed.insert(id)
            if name == cancelElement {
                onMain { m in
                    guard let cap = m.captureHandler else { return }
                    m.captureHandler = nil
                    cap(nil)
                }
            }
            return
        }
        if !down, captureSwallowed.remove(id) != nil { return }
        // The Quick Menu: a combo member (L+R) or an input of its own.
        let fed = chord.feed(id, down: down, now: now())
        if fed.taken {
            if down { confirmTap.cancel() }   // L / R step while paused: a held confirm is no longer a tap
            emitChord(fed.events)
            scheduleChordTick()
            return
        }
        emitChord(fed.events)   // a member held alone may have fired just now
        if isMenuInput(id) {
            if down { post(.menu) }
            return
        }
        if isNavMode { routeNav(id, name, down); return }
        if routeSeek(name, down) { return }
        if routePausedStep(id, down) { return }
        let tap = trackPausedTap(id, name, down)
        setPressed(id, down, at: gcEventTime)
        switch tap {
        case .resume: onMain { $0.onPausedResume?() }
        case .seek(let input): onMain { $0.onSeekInput?(input, true) }
        case nil: break
        }
    }

    /// Test hook (--fake-controller <family>): a controller of that family listed on slot 0 (hint
    /// glyphs, the diagram, the pill) without a device; its input comes from --inject-pad.
    func addFakeController(family: ControllerFamily) {
        infos[0] = ControllerInfo(slot: 0, name: "Test Pad", productCategory: "Test", family: family, labels: [:])
        publishControllers()
    }

    /// Test hook (--inject-pad): a controller element change on slot 0, through the same path
    /// as a real controller (hotkeys, paused stepping, game input).
    func injectController(_ element: String, _ down: Bool) {
        gcQueue.async { [weak self] in self?.set("gc0:", element, down) }
    }

    private func stick(_ prefix: String, _ name: String, _ x: Float, _ y: Float) {
        let nav = isNavMode
        let seek = !nav && seekFocus > 0 && isPausedInPlay   // the markers have the pad
        rn_input_set_axis(handle, prefix + name, nav || seek ? 0 : x, nav || seek ? 0 : y)
        // Track virtual direction ids for change detection / capture (threshold 0.5).
        let t: Float = 0.5
        for (dir, on) in [("left", x <= -t), ("right", x >= t), ("up", y >= t), ("down", y <= -t)] {
            let id = prefix + name + "." + dir
            if lastState[id] == on { continue }
            lastState[id] = on
            notePressed(id, on)
            if isCapturing {
                continue   // keys only
            } else if nav {
                routeNav(id, name + "." + dir, on)
            } else if routeSeek(name + "." + dir, on) {
                continue
            } else {
                if on { confirmTap.cancel() }
                noteEvent(at: gcEventTime)
            }
        }
    }

    private func update(extended g: GCExtendedGamepad, prefix p: String, face: FaceNames) {
        gcEventTime = g.lastEventTimestamp
        defer { gcEventTime = nil }
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
        // No buttonHome: HOME / guide belongs to the system (rnf_input_element_ignored), never an input.
        if let l3 = g.leftThumbstickButton { set(p, "leftThumb", l3.isPressed) }
        if let r3 = g.rightThumbstickButton { set(p, "rightThumb", r3.isPressed) }
        stick(p, "lstick", g.leftThumbstick.xAxis.value, g.leftThumbstick.yAxis.value)
        stick(p, "rstick", g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value)
    }

    private func update(micro g: GCMicroGamepad, prefix p: String, face: FaceNames) {
        gcEventTime = g.lastEventTimestamp
        defer { gcEventTime = nil }
        set(p, face.a, g.buttonA.isPressed)
        set(p, face.x, g.buttonX.isPressed)
        set(p, "menu", g.buttonMenu.isPressed)
        set(p, "dpad.up", g.dpad.up.isPressed)
        set(p, "dpad.down", g.dpad.down.isPressed)
        set(p, "dpad.left", g.dpad.left.isPressed)
        set(p, "dpad.right", g.dpad.right.isPressed)
    }
}
