// iPhone app state: owns the emulation thread (shared EmulationController), input (shared
// InputManager for game controllers + the touch controls), the library and the session lifecycle
// (temporary slot / projects, autosave, resume), preferences and the gates of the paid features.
// Named AppModel like the macOS one so the shared app-layer files (SeekMarkers, TimelineActions,
// Filmstrip, QuickMenuController) compile unchanged; it implements the members they use.
// Differences from macOS (apps/macos/Sources/App/AppModel.swift): no document / save panels
// (projects are named automatically in Documents/Projects), dialogs are SwiftUI alerts, the app is
// persisted when it leaves the foreground, and the paid features are gated (FeatureGate).
// SPDX-License-Identifier: GPL-2.0-or-later
import Combine
import SwiftUI
import UIKit
import UniformTypeIdentifiers

extension UTType {
    static let nesROM = UTType(importedAs: "io.github.replaynes.nes-rom")
    static let nesrec = UTType(exportedAs: "io.github.replaynes.nesrec")
}

/// A dialog (SwiftUI alert) with up to three buttons.
struct AppAlert: Identifiable {
    struct Action {
        var title: String
        var role: ButtonRole?
        var run: () -> Void = {}
    }
    let id = UUID()
    var title: String
    var message: String
    var actions: [Action] = []
}

/// A text prompt (rename).
struct AppPrompt: Identifiable {
    let id = UUID()
    var title: String
    var text: String
    var done: (String) -> Void
}

final class ExportJob: ObservableObject {
    let url: URL
    @Published var done: UInt64 = 0
    @Published var total: UInt64 = 0
    @Published var finished = false
    @Published var result: ExportResult?
    @Published var error: String?
    private let lock = NSLock()
    private var cancelled = false
    init(url: URL) { self.url = url }
    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
}

/// The per-frame status for views showing counters (see AppModel.status).
final class PlaybackClock: ObservableObject {
    @Published var status = EmuStatus()
}

extension EmuStatus {
    var coarse: EmuStatus {
        var c = self
        c.frame = 0
        c.takeLength = takeLength > 0 ? 1 : 0
        c.practiceFrame = 0
        return c
    }
}

final class AppModel: ObservableObject {
    static let shared = AppModel()

    let input = InputManager()
    let emu: EmulationController
    let library = LibraryModel()
    let entitlements = Entitlements.shared
    let audioSession = AudioSessionSetup()

    var status = EmuStatus() {
        willSet { if newValue.coarse != status.coarse { objectWillChange.send() } }
        didSet {
            if status.paused != oldValue.paused || status.hasSession != oldValue.hasSession { updateUIMode() }
            else if status.coarse != oldValue.coarse { markers.sync() }
        }
    }
    let markers = SeekMarkers()
    let quickMenu = QuickMenuController()
    /// Who counts 3, 2, 1 before a resume from pause (ResumeCountdown.swift): the shared core's.
    private(set) lazy var countdownDriver: ResumeCountdownDriver = CoreResumeCountdown(emu: emu)
    let clock = PlaybackClock()

    @Published var showLibrary = false
    @Published var bookmarks: [BookmarkInfo] = []
    @Published var takes: [TakeInfo] = []
    @Published var controllers: [String] = []
    @Published var inputConfig: InputCatalog.Config
    @Published var stats = LatencyMeter.Snapshot()
    @Published var notice: String?
    @Published var showExport = false { didSet { markers.sync() } }
    @Published var showUnlocks = false
    @Published var exportJob: ExportJob?
    @Published var capturingAction: String?
    @Published var practiceSlots: [PracticeSlotInfo] = (0..<EngineSession.practiceSlotCount).map { PracticeSlotInfo(index: $0) }
    @Published var alert: AppAlert? {
        didSet {
            if alert?.id != oldValue?.id { dialogFocus = max(0, (alert?.actions.count ?? 1) - 1) }
            if (alert == nil) != (oldValue == nil) { updateUIMode() }   // the pad runs TopDialog, not the game
        }
    }
    /// The button focused in the upper-half dialog (TopDialog; the last one, as iOS's preferred action).
    @Published var dialogFocus = 0
    /// RootView draws the app in the upper half (a FlipPad, portrait): dialogs are TopDialog, run by the pad.
    var upperHalfLayout = false { didSet { if upperHalfLayout != oldValue { updateUIMode() } } }
    /// A connected controller covers the lower half of the phone held upright (controllerCoversLowerHalf).
    var upperHalfController: Bool {
        controllers.contains(where: controllerCoversLowerHalf) || UserDefaults.standard.bool(forKey: "assumeCoverPad")   // test hook
    }
    /// The shared QuickMenuController asks whether a dialog owns the input (the Mac's in-window
    /// dialogs); on iPhone the dialogs are the SwiftUI alerts above.
    struct DialogState { let isActive: Bool }
    var dialogs: DialogState { DialogState(isActive: alert != nil) }
    @Published var prompt: AppPrompt?
    /// Where the game view and the touch controls are (PlaySurfaceView), for the overlays.
    @Published var playLayout: TouchLayout?
    @Published private(set) var pendingResume: ResumeRecord?
    @Published var selectedSlot = 0 { didSet { FilmstripModel.shared.selectedSlot = selectedSlot } }
    /// A session change (save, close, switch, open, reset) is in flight on the emulation thread /
    /// a background queue; RootView shows a spinner (after a short delay) and takes no touches.
    @Published private(set) var showBusy = false
    private var busyCount = 0
    private var afterBusy: [() -> Void] = []

    // Preferences (same keys as macOS where the setting exists there).
    @AppStorage("showLatency") var showLatency = false { didSet { objectWillChange.send() } }
    @AppStorage("pauseAfterRewind") var pauseAfterRewind = false { didSet { pushPrefs(); objectWillChange.send() } }
    @AppStorage("autosaveInterval") var autosaveInterval = 2.0 { didSet { pushPrefs(); objectWillChange.send() } }
    @AppStorage("integerScale") var integerScale = true { didSet { objectWillChange.send() } }
    @AppStorage("displayPAR87") var displayPAR87 = false { didSet { objectWillChange.send() } }
    @AppStorage("hideOverscan") var hideOverscan = true { didSet { objectWillChange.send() } }
    /// The empty sides show the picture enlarged, blurred and dimmed (Settings › Display › Picture Shape).
    @AppStorage("backdrop") var backdrop = true { didSet { objectWillChange.send() } }
    @AppStorage("volume") var volume = 0.8 { didSet { emu.audio.volume = Float(volume); objectWillChange.send() } }
    @AppStorage("southConfirm") var southConfirm = false { didSet { input.setSouthConfirm(southConfirm); objectWillChange.send() } }
    @AppStorage("dpadStepWhenPaused") var dpadStepWhenPaused = true { didSet { input.setPausedStepEnabled(dpadStepWhenPaused); objectWillChange.send() } }
    @AppStorage("flashReduction") var flashReduction = FlashLevel.standard.rawValue { didSet { pushPrefs(); objectWillChange.send() } }
    @AppStorage("showFlashIndicator") var showFlashIndicator = true { didSet { objectWillChange.send() } }
    // iOS only.
    @AppStorage("touchHaptics") var touchHaptics = true { didSet { objectWillChange.send() } }
    /// NESTERM's controller is drawn opaque (its colours already sit on --bb-bg); kept adjustable.
    @AppStorage("touchOpacity") var touchOpacity = 1.0 { didSet { objectWillChange.send() } }
    /// AA / BB (turbo A / B) up-left of A / B; on by default (a new key: the old "touchTurbo" switch
    /// showed the turbo pills and was off by default).
    @AppStorage("touchAABB") var touchAABB = true { didSet { objectWillChange.send() } }
    /// Settings › Display › Picture Shape › Orientation (iPhone): OrientationLock raw value.
    @AppStorage(OrientationLock.key) var orientationLock = OrientationLock.auto.rawValue {
        didSet { OrientationLock.apply(OrientationLock(rawValue: orientationLock) ?? .auto); objectWillChange.send() }
    }
    /// controller.mjs (NESTERM settings, 操作): 十字キーの大きさ --bb-dpad (130...210 px, CSS 144px),
    /// 追従 ↔ / ↕ setHomeGain (0...2, defaults recentre-x 0.3, recentre-y 1).
    @AppStorage("touchDpadSize") var touchDpadSize = Double(TouchLayout.dpadDefault) { didSet { objectWillChange.send() } }
    @AppStorage("touchFollowX") var touchFollowX = 0.3 { didSet { objectWillChange.send() } }
    @AppStorage("touchFollowY") var touchFollowY = 1.0 { didSet { objectWillChange.send() } }
    /// Hide the touch controls while a game controller is connected.
    @AppStorage("touchHideWithPad") var touchHideWithPad = true { didSet { objectWillChange.send() } }
    /// Settings › Controls › Controls Details › Resume Countdown (same key as macOS).
    @AppStorage("resumeCountdown") var resumeCountdown = true { didSet { pushPrefs(); objectWillChange.send() } }
    /// The countdown before play resumes and before each practice run: 3 or 1 second(s) (1: the ring
    /// alone, no numeral; the default), 0 = none - the desktop's Settings › System › Countdown
    /// (rnf_countdown_set_seconds). `resumeCountdown` is kept in the defaults but no longer used.
    @AppStorage("countdownSeconds") var countdownSeconds = 1 { didSet { pushPrefs(); objectWillChange.send() } }

    var flashLevel: FlashLevel { FlashLevel(rawValue: flashReduction) ?? .standard }

    var displayOptions: DisplayOptions {
        DisplayOptions(integerScale: integerScale, pixelAspect87: displayPAR87, hideOverscan: hideOverscan, geometry: status.geometry,
                       backdrop: backdrop ? 2 : 0)  // the switch keeps on / off; on = the default level (low)
    }


    /// The NES picture's shown width / height with the Display options (overscan, 8:7): the portrait
    /// picture row's (TouchLayout.Options.rowAspect), whatever core is playing.
    /// CRT Display on: the CRT's shown part (DisplayOptions.crtShownAspect), which MetalView fits instead.
    var nesRowAspect: CGFloat {
        var o = displayOptions
        o.geometry = .nes
        let crt = CRTSettingsModel.shared
        return crt.enabled ? o.crtShownAspect(activeWidth: crt.settings.activeWidth) : o.displayedAspect
    }

    /// The picture's width / height on the screen: the CRT's shown part while the CRT is on (NES picture).
    var shownPictureAspect: CGFloat {
        let crt = CRTSettingsModel.shared
        if crt.enabled && status.geometry.isNESPicture { return displayOptions.crtShownAspect(activeWidth: crt.settings.activeWidth) }
        return displayOptions.displayedAspect
    }

    var touchOptions: TouchLayout.Options {
        TouchLayout.Options(showAABB: touchAABB, dpadSize: min(max(CGFloat(touchDpadSize), TouchLayout.dpadRange.lowerBound), TouchLayout.dpadRange.upperBound),
                            ways: touchWays, fill: !integerScale, pictureAspect: shownPictureAspect,
                            rowAspect: nesRowAspect,
                            buttons: status.fireButtons)
    }

    /// controller.mjs syncWays / setGame: 8-way or 4-way, remembered per ROM ("ways:<crc32>" there,
    /// the ROM's SHA-256 here) and globally without a game; a game never set starts with 8-way.
    var touchWays: Int { UserDefaults.standard.integer(forKey: touchWaysKey) == 4 ? 4 : 8 }

    private var touchWaysKey: String {
        if let sha = current?.romSHA256, !sha.isEmpty { return "touchWays:" + sha }
        return "touchWays"
    }

    /// The ways switch (#ways-switch) / the Touch Controls setting.
    func toggleTouchWays() {
        UserDefaults.standard.set(touchWays == 4 ? 8 : 4, forKey: touchWaysKey)
        objectWillChange.send()
    }

    var touchControlsVisible: Bool { !(touchHideWithPad && !controllers.isEmpty) }

    // Session persistence (Application Support/ReplayNES/Session: temporary project, resume.json).
    private(set) var sessionPaths = SessionPaths.standard
    private var sessionLock: SessionLock?
    private(set) var persistSessions = false
    struct SessionIdentity { let projectPath: String; let isTemp: Bool; let romSHA256: String }
    private(set) var current: SessionIdentity?
    private var lastResume: ResumeRecord?
    private var playSHA = ""
    private var playStart = Date()
    private var playMark = ProcessInfo.processInfo.systemUptime
    private var playFlushTicks = 0
    private var resumeTimer: Timer?
    private var statsTimer: Timer?
    private let resumeQueue = DispatchQueue(label: "replaynes.resume", qos: .utility)
    private var noticeWork: DispatchWorkItem?
    private var subs: Set<AnyCancellable> = []
    /// Touch buttons routed to paused stepping (their release goes there too).
    private var touchRouted: Set<TouchButton> = []
    private var started = false
    private var afterStart: [() -> Void] = []
    /// ROM imports (document picker, Open in / Share from other apps), off the main thread.
    private(set) lazy var importQueue: ROMImportQueue = {
        let paths = library.paths
        let q = ROMImportQueue(romDir: { paths.roms }, inbox: { paths.root.appendingPathComponent("Inbox", isDirectory: true) })
        q.onStart = { [weak self] n in self?.importStarted(n) }
        q.onFinish = { [weak self] r, b in self?.importFinished(r, b) }
        return q
    }()
    private var wasInBackground = false

    private init() {
        emu = EmulationController(input: input)
        // Another core's layout (ReplayArcade): the touch controls drive its buttons, coin and start.
        emu.layoutExtraBindings = { TouchBindings.forLayout(actions: $0) }
        inputConfig = input.config
        emu.onStatus = { [weak self] st in
            guard let self else { return }
            if self.status.practicing != st.practicing { self.input.setPracticing(st.practicing) }
            if self.status != st { self.status = st; self.clock.status = st }
        }
        emu.onStructure = { [weak self] st in
            guard let self else { return }
            if self.bookmarks != st.bookmarks { self.bookmarks = st.bookmarks }
            if self.takes != st.takes { self.takes = st.takes }
            if !st.practiceSlots.isEmpty, self.practiceSlots != st.practiceSlots { self.practiceSlots = st.practiceSlots }
            self.markers.sync()
        }
        emu.onError = { [weak self] title, msg in self?.showError(title, msg) }
        emu.onNotice = { [weak self] text in self?.flash(text) }
        emu.onCoach = { [weak self] tip, shown in self?.showCoach(tip, shown: shown) }
        input.onConfigChanged = { [weak self] c in self?.inputConfig = c }
        input.onControllersChanged = { [weak self] c in self?.controllers = c; self?.updateUIMode() }
        input.onNav = { [weak self] n in self?.handleNav(n) }
        input.onPausedResume = { [weak self] in
            guard let self, self.status.hasSession, self.status.paused, !self.quickMenu.isOpen, !self.showLibrary else { return }
            self.togglePause()
        }
        input.onSeekInput = { [weak self] i, down in self?.markers.seekInput(i, down: down) }
        input.onMarkerHold = { [weak self] dir, down in self?.markers.markerHold(dir, down: down) }
        input.onPausedShoulder = { [weak self] dir in self?.markers.pausedShoulder(dir) }
        markers.model = self
        input.onDisconnect = { [weak self] name in
            guard let self else { return }
            self.emu.perform { emu in emu.paused = true }
            self.flash(String(localized: "Paused because \(name) was disconnected"))
        }
        input.onPausedStep = { [weak emu] dir, down in emu?.perform { e in e.pausedStep(dir, down: down) } }
        // Y while practicing (playing, paused, countdown): stop practice (shared InputManager).
        input.onPracticeStop = { [weak self] in
            guard let self, self.status.practicing, !self.quickMenu.isOpen, !self.showLibrary else { return }
            self.practiceStop()
        }
        // Pad 1's shoulder gestures in play (L / R + START pause, L held + R / R held + L step).
        input.onPauseGesture = { [weak self] dir, repeated in self?.markers.pauseGesture(dir, repeated: repeated) }
        quickMenu.model = self
        // The screens read quickMenu.isOpen through this model (RootView / LibraryView / PlayView /
        // PlaySurface): without this, opening or closing the menu redrew nothing unless the game's
        // pause state changed with it - "…" › Settings in the library, the MENU pill on a paused game
        // and ✕ / the backdrop / Resume on a menu opened while paused did nothing on screen.
        quickMenu.$isOpen.removeDuplicates().sink { [weak self] _ in self?.objectWillChange.send() }.store(in: &subs)
        ensureTouchBindings()
        entitlements.$state.sink { [weak self] st in
            DispatchQueue.main.async { self?.entitlementsChanged(st) }
        }.store(in: &subs)
    }

    /// After "Default Buttons": the touch ids get their default bindings back.
    func restoreTouchBindings() { ensureTouchBindings() }

    /// Bindings of the touch ids (TouchBindings) in a configuration that has none yet.
    private func ensureTouchBindings() {
        let add = TouchBindings.missing(existing: input.config.bindings.map { ($0.input, $0.action) })
        guard !add.isEmpty else { return }
        for b in add { _ = rn_input_bind(input.handle, b.input, b.action) }
        // Persist through the regular path (writes bindings.json, refreshes the derived state).
        if let first = add.first { input.bind(first.input, to: first.action) }
    }

    func start() {
        guard !started else { return }
        started = true
        OrientationLock.apply(OrientationLock(rawValue: orientationLock) ?? .auto)
        audioSession.activate()
        input.setPausedStepEnabled(dpadStepWhenPaused)
        input.setSouthConfirm(southConfirm)
        input.setPausedStepMode(true)
        FilmstripModel.shared.$selectedSlot.removeDuplicates()
            .sink { [weak self] _ in DispatchQueue.main.async { self?.markers.sync() } }
            .store(in: &subs)
        emu.audio.volume = Float(volume)
        audioSession.onRestart = { [weak self] in self?.emu.audio.start() }
        pushPrefs()
        configureCoach()
        entitlements.refresh()
        entitlementsChanged(entitlements.state)
        emu.start()
        input.startControllers()
        setupSessionPersistence()
        library.start()
        observeShareInbox()
        drainShareInbox()
        loadPendingResume()
        updateUIMode()
        statsTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self, self.showLatency || PacingProbe.shared.active else { return }
            self.stats = self.emu.latency.snapshot(audio: self.emu.audio)
            PacingProbe.shared.observe(self.stats)
        }
        let pending = afterStart
        afterStart = []
        pending.forEach { $0() }
    }

    private func pushPrefs() {
        let par = pauseAfterRewind, auto = autosaveInterval, flash = flashLevel, cs = countdownSeconds
        // One countdown setting for both (3 / 1 / none): the length, and whether there is one at all.
        rnf_countdown_set_seconds(Int32(max(1, cs)))
        emu.perform { e in
            e.pauseAfterRewind = par; e.autosaveInterval = auto; e.setFlashLevel(flash)
            e.setResumeCountdown(cs > 0); e.setPracticeCountdown(cs > 0)
        }
    }

    /// The renderer sees CRT only while the feature is unlocked (the stored choice is kept).
    private func entitlementsChanged(_ st: EntitlementState) {
        let crt = CRTSettingsModel.shared
        crt.launchOverride = false
        if !FeatureGate.allows(.crtDisplay, st) && crt.enabled {
            UserDefaults.standard.set(true, forKey: "crtWantedWhenUnlocked")
            crt.enabled = false
        } else if FeatureGate.allows(.crtDisplay, st) && UserDefaults.standard.bool(forKey: "crtWantedWhenUnlocked") {
            UserDefaults.standard.removeObject(forKey: "crtWantedWhenUnlocked")
            crt.enabled = true
        }
        objectWillChange.send()
    }

    // MARK: messages

    func flash(_ text: String, duration: Double = 3.5) {
        notice = text
        noticeWork?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.notice = nil }
        noticeWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + duration, execute: w)
    }

    /// First-run coach lines (docs/design/VTR_UX.md): each shown once ever; off for scripted runs
    /// (TestHooks -autoplayTestROM / -autoplayROM).
    private func configureCoach() {
        let d = UserDefaults.standard
        let off = d.bool(forKey: "autoplayTestROM") || d.string(forKey: "autoplayROM") != nil
        let shown: UInt32? = off ? nil : UInt32(truncatingIfNeeded: UserDefaults.standard.integer(forKey: "coachShown"))
        emu.perform { e in e.setCoach(shown: shown) }
    }

    private func showCoach(_ tip: rnf_coach_tip, shown: UInt32) {
        UserDefaults.standard.set(Int(shown), forKey: "coachShown")
        flash(CoachTips.text(tip, rewindControl: controlName(for: "hk.rewind").map(FaceGlyph.plain)), duration: 5)
    }

    /// The name of the control bound to `action`: pad 1's button as printed on the connected
    /// controller (a face button: its FaceGlyph token, drawn by KeyCapIOS), else the touch button as
    /// drawn ("◀◀"); nil when unbound.
    func controlName(for action: String) -> String? {
        let config = inputConfig
        if let c = input.controllerMonitor.controllers.min(by: { $0.slot < $1.slot }),
           let id = InputCatalog.boundInput(config, action: action, pad: true), let colon = id.firstIndex(of: ":") {
            let element = String(id[id.index(after: colon)...])
            return FaceGlyph.token(forElement: element) ?? c.label(element)
        }
        guard let id = config.bindings.first(where: { $0.action == action && $0.input.hasPrefix("touch:") })?.input else { return nil }
        // The touch buttons as drawn: the transport row's ◀◀ / ▶▶ (no L2 / R2 label on screen).
        let name = String(id.dropFirst("touch:".count))
        if let b = TouchButton(rawValue: name) { return TouchLayout.label(.button(b), ways: 8) }
        return name.uppercased()
    }

    func showError(_ title: String, _ message: String) {
        alert = AppAlert(title: title, message: message, actions: [AppAlert.Action(title: String(localized: "OK"), role: .cancel)])
    }

    /// A locked feature was asked for: say what unlocks it.
    func showLocked(_ f: PaidFeature) {
        alert = AppAlert(title: Unlocks.title(f), message: Unlocks.lockedMessage(f), actions: [
            AppAlert.Action(title: L("Unlocks…")) { [weak self] in self?.showUnlocks = true },
            AppAlert.Action(title: String(localized: "Cancel"), role: .cancel),
        ])
    }

    // MARK: transport (main thread -> emulation thread)

    /// Pause / resume (seek bar Resume, controller cancel tap): a resume counts down first (core).
    func togglePause() { emu.perform { e in e.togglePause() } }
    /// true = pause; false = the user resumes (the Quick Menu's close / Resume): through the countdown.
    func setPaused(_ p: Bool) {
        if p { emu.perform { e in e.paused = true } } else { countdownDriver.resume() }
    }
    /// Plays at once, no countdown (a new run started by the app).
    func resumeNow() { emu.perform { e in e.paused = false } }
    /// ✕ / the pause button during the resume countdown: back to paused.
    func abortResumeCountdown() { countdownDriver.abort() }
    var resumeCountdownShown: Bool { countdownDriver.isCounting(status) }
    func stepFrame(_ dir: Int) {
        emu.perform { e in
            guard let s = e.session else { return }
            e.stepFrame(dir, s)
        }
    }
    /// L held + R / R held + L (EmulationController.followGesture: in play pause / slow 1/2, paused a step).
    func followGesture(_ dir: Int, repeated: Bool) {
        emu.perform { e in
            guard let s = e.session else { return }
            e.followGesture(dir, repeated: repeated, s)
        }
    }
    func toggleSlow() { emu.perform { e in e.slow = e.slow.toggled } }
    func toggleRecord() { emu.perform { e in e.toggleRecord() } }
    func setRewindHeld(_ h: Bool) { emu.perform { e in e.uiRewindHeld = h } }
    func setFastForwardHeld(_ h: Bool) { emu.perform { e in e.uiFastForwardHeld = h } }
    func scrub(to f: UInt64) { emu.perform { e in e.paused = true; e.scrubTarget = f } }
    func seek(to f: UInt64) { emu.perform { e in e.seekCommand(f) } }
    func softReset() { emu.perform { e in e.requestEvent(UInt8(RN_EV_SOFT_RESET)) } }
    func powerCycle() { emu.perform { e in e.requestEvent(UInt8(RN_EV_POWER_CYCLE)) } }
    func rerecordHere() { emu.perform { e in e.rerecordHere() } }
    /// "↶ Undo": back to before the last retry (10 s window; EmulationController.undoRetry).
    func undoRetry() { emu.perform { e in e.undoRetry() } }
    func addBookmark() { emu.perform { e in e.addBookmark(name: nil) } }
    func practiceSetA(_ slot: Int) { emu.perform { e in e.practiceSetA(slot) } }
    func practiceSetB(_ slot: Int) { emu.perform { e in e.practiceSetB(slot) } }
    func practiceStart(_ slot: Int) { emu.perform { e in e.startPractice(slot) } }
    func practiceStop() { emu.perform { e in e.stopPractice() } }
    func practiceRename(_ slot: Int, _ name: String) { emu.perform { e in e.practiceRename(slot, name) } }
    func practiceClear(_ slot: Int) { emu.perform { e in e.practiceClear(slot) } }

    private func blockedInPractice() -> Bool {
        if status.practicing { flash(EmulationController.practiceBlockedText); return true }
        return false
    }

    func gotoBookmark(_ id: UInt64) {
        if blockedInPractice() { return }
        emu.perform { e in
            guard let s = e.session else { return }
            do {
                try s.gotoBookmark(id)
                e.paused = true
                e.markStructureDirty()
                e.publishVideo()
            } catch { e.reportError(String(localized: "Couldn’t jump to the bookmark"), error) }
        }
    }
    func removeBookmark(_ id: UInt64) {
        emu.perform { e in
            do { try e.session?.removeBookmark(id); e.markStructureDirty() } catch { e.reportError(String(localized: "Couldn’t delete the bookmark"), error) }
        }
    }
    func renameBookmark(_ id: UInt64, _ name: String) {
        emu.perform { e in
            do { try e.session?.renameBookmark(id, name: name); e.markStructureDirty() } catch { e.reportError(String(localized: "Couldn’t rename"), error) }
        }
    }
    func activateTake(_ id: UInt64) {
        if blockedInPractice() { return }
        emu.perform { e in
            guard let s = e.session else { return }
            do {
                try s.activateTake(id)
                e.paused = true
                e.markStructureDirty()
                e.publishVideo()
                e.notice(String(localized: "Switched to take #\(id)"))
            } catch { e.reportError(String(localized: "Couldn’t switch takes"), error) }
        }
    }

    // MARK: touch controls (main thread, from TouchControlsView)

    func touchButtons(pressed: [TouchButton], released: [TouchButton]) {
        if Self.logTouch { NSLog("ReplayNES touch: %@", (pressed.map { $0.rawValue + "+" } + released.map { $0.rawValue + "-" }).joined(separator: " ")) }
        let st = status
        let pausedInPlay = st.hasSession && st.paused && !quickMenu.isOpen && !showLibrary
        for b in pressed {
            if b == .menu { quickMenu.open(); continue }
            // Pause (above MENU; the picture is no longer tapped to pause): while the game plays; during
            // the resume countdown it goes back to paused (as the countdown's ✕).
            if b == .pause {
                guard st.hasSession, !quickMenu.isOpen, !showLibrary else { continue }
                if resumeCountdownShown { abortResumeCountdown() } else if !st.paused { setPaused(true) }
                continue
            }
            // Slow motion: normal ⇔ 1/2 at the press (as a controller's R held + L in play).
            if b == .slow {
                if st.hasSession && !quickMenu.isOpen && !showLibrary { toggleSlow() }
                continue
            }
            // Frame step buttons: as the desktop's Step Back / Frame Advance keys (EmulationController
            // stepFrame): playing (also during the resume countdown, which the pause cancels), the
            // press pauses and steps; paused, it steps. Held: repeats as a controller's paused L / R.
            // Never game input.
            if b == .stepBack || b == .stepForward {
                if st.hasSession && !quickMenu.isOpen && !showLibrary { startStepRepeat(b) }
                continue
            }
            // Paused: the D-pad ← / → step one frame (setting), never game input.
            if pausedInPlay {
                if dpadStepWhenPaused && (b == .left || b == .right) {
                    let dir = b == .left ? -1 : 1
                    emu.perform { e in e.pausedStep(dir, down: true) }
                    touchRouted.insert(b)
                    continue
                }
            }
            if let id = b.inputID {
                rn_input_set_pressed(input.handle, id, 1)
                // Playback: a game button plays from here (EmulationController samples the game input).
                input.noteTakeoverPress()
            }
        }
        for b in released {
            if b == .slow { continue }
            if b == .stepBack || b == .stepForward {
                if stepRepeatButton == b { stopStepRepeat() }
                continue
            }
            if touchRouted.remove(b) != nil {
                if b == .left || b == .right {
                    let dir = b == .left ? -1 : 1
                    emu.perform { e in e.pausedStep(dir, down: false) }
                }
                continue
            }
            if let id = b.inputID { rn_input_set_pressed(input.handle, id, 0) }
        }
    }

    /// The touch frame step held down (main thread): one step at the press, then after
    /// `stepRepeatDelay` one every `stepRepeatInterval` (the shared chord detector's paused L / R
    /// repeat: docs/design/UI_REDESIGN.md) until it is released.
    private var stepRepeatButton: TouchButton?
    private var stepRepeatTimer: Timer?
    static let stepRepeatDelay = 0.4, stepRepeatInterval = 0.05

    private func startStepRepeat(_ b: TouchButton) {
        stopStepRepeat()
        let dir = b == .stepBack ? -1 : 1
        stepFrame(dir)
        stepRepeatButton = b
        let t = Timer(fire: Date().addingTimeInterval(Self.stepRepeatDelay), interval: Self.stepRepeatInterval, repeats: true) { [weak self] _ in
            guard let self else { return }
            let st = self.status
            guard st.hasSession, !self.quickMenu.isOpen, !self.showLibrary else { self.stopStepRepeat(); return }
            self.stepFrame(dir)
        }
        RunLoop.main.add(t, forMode: .common)
        stepRepeatTimer = t
    }

    private func stopStepRepeat() {
        stepRepeatTimer?.invalidate()
        stepRepeatTimer = nil
        stepRepeatButton = nil
    }

    /// TestHooks -logTouch: every touch press / release edge in the system log.
    private static let logTouch = UserDefaults.standard.bool(forKey: "logTouch")

    /// Everything the touch controls held is released (menu opened, app in the background).
    func releaseTouch() {
        stopStepRepeat()
        rn_input_release_prefix(input.handle, "touch:")
        for b in touchRouted where b == .left || b == .right {
            let dir = b == .left ? -1 : 1
            emu.perform { e in e.pausedStep(dir, down: false) }
        }
        touchRouted.removeAll()
    }

    // MARK: navigation (controllers)

    func handleNav(_ n: NavInput) {
        if let a = alert, upperHalfLayout { handleDialogNav(n, a); return }
        if quickMenu.isOpen { quickMenu.handle(n); return }
        let libraryOn = !status.hasSession || showLibrary
        switch n {
        case .menu:
            quickMenu.open()
        case .escape, .back:
            if libraryOn && status.hasSession { hideLibraryScreen() }
        default:
            if libraryOn { LibraryFocus.shared.handle(n, model: self) }
        }
    }

    /// TopDialog with the pad: left / right choose, confirm runs, back / B runs the cancel action.
    private func handleDialogNav(_ n: NavInput, _ a: AppAlert) {
        let count = max(1, a.actions.count)
        switch n {
        case .left, .up: dialogFocus = (dialogFocus + count - 1) % count
        case .right, .down: dialogFocus = (dialogFocus + 1) % count
        case .confirm: runDialogAction(a.actions.indices.contains(dialogFocus) ? dialogFocus : nil)
        case .back, .escape: runDialogAction(a.actions.firstIndex { $0.role == .cancel })
        default: break
        }
    }

    /// Closes the dialog and runs action `i` (nil: just close).
    func runDialogAction(_ i: Int?) {
        guard let a = alert else { return }
        alert = nil
        if let i, a.actions.indices.contains(i) { a.actions[i].run() }
    }

    func updateUIMode() {
        let libraryOn = !status.hasSession || showLibrary
        input.setNavMode(quickMenu.isOpen || libraryOn || (alert != nil && upperHalfLayout))
        if quickMenu.isOpen || libraryOn { releaseTouch() }
        markers.sync()
    }

    func showLibraryScreen() {
        if quickMenu.isOpen { quickMenu.close(resume: false) }
        if status.hasSession { setPaused(true); captureThumbnail() }
        showLibrary = true
        library.refresh()
        updateUIMode()
    }

    func hideLibraryScreen() {
        guard showLibrary else { return }
        showLibrary = false
        updateUIMode()
    }

    // MARK: library

    /// "Play" on a game card: free = the temporary slot; recording unlocked = a new project in
    /// Projects/ (FeatureGate.sessionTarget).
    func playFromLibrary(_ rom: LibraryROM, autoplay: Bool = false) {
        guard whenIdle({ [weak self] in self?.playFromLibrary(rom, autoplay: autoplay) }) else { return }
        leaveCurrentSession { [weak self] in
            guard let self else { return }
            switch FeatureGate.sessionTarget(forLibraryPlay: self.entitlements.state) {
            case .newProject:
                guard self.library.ensureFolders() else {
                    self.showError(String(localized: "The library folder isn’t available"), self.library.folderError ?? "")
                    return
                }
                let dir = LibraryScanner.newProjectURL(projectsDir: self.library.paths.projects, romName: rom.name, date: Date())
                self.createSession(rom: rom.url, projectDir: dir, autoplay: autoplay)
            case .temporary:
                self.createSession(rom: rom.url, projectDir: nil, autoplay: autoplay)
            }
        }
    }

    /// Test hooks: forget the temporary session and the resume record (no prompts).
    func discardTemporarySession() {
        guard whenIdle({ [weak self] in self?.discardTemporarySession() }) else { return }
        let finish = { [weak self] in
            guard let self else { return }
            self.removeTempProject()
            self.writeResume(nil)
        }
        if current?.isTemp == true { releaseSession(then: finish) } else { finish() }
    }

    /// "Continue" on a project of a game.
    func continueProject(_ url: URL) {
        guard whenIdle({ [weak self] in self?.continueProject(url) }) else { return }
        leaveCurrentSession { [weak self] in self?.openProject(url) }
    }

    /// The Continue card: the last session where it was, else the newest project.
    func continueLast() {
        guard whenIdle({ [weak self] in self?.continueLast() }) else { return }
        guard !status.hasSession else { hideLibraryScreen(); return }
        if let r = pendingResume {
            openProject(URL(fileURLWithPath: r.projectPath), resume: r)
        } else if let p = library.projectsBySHA.values.flatMap({ $0 }).max(by: { $0.modified < $1.modified }) {
            continueProject(p.url)
        }
    }

    /// Before another game replaces the session: a project is saved (full save, no prompt); the
    /// temporary session is replaced (free) or offered for keeping (recording unlocked).
    private func leaveCurrentSession(then next: @escaping () -> Void) {
        if let c = current, !c.isTemp {
            captureThumbnail()
            releaseSession(flush: true, then: next)
            return
        }
        settleTempSlot(then: next)
    }

    /// Makes room for a new temporary session (see leaveCurrentSession).
    private func settleTempSlot(then next: @escaping () -> Void) {
        guard persistSessions else { next(); return }
        if current?.isTemp == true {
            // Persist it first (on the emulation thread), then decide with its files up to date.
            captureThumbnail()
            beginBusy()
            emu.perform({ e in _ = e.flushForResume(fullSave: true) }, completion: { [weak self] in
                self?.decideTempSlot(isCurrent: true, then: next)
                self?.endBusy()
            })
            return
        }
        decideTempSlot(isCurrent: false, then: next)
    }

    private func decideTempSlot(isCurrent: Bool, then next: @escaping () -> Void) {
        let exists = sessionPaths.tempProjectExists
        let hasContent: Bool = {
            if isCurrent { return takes.contains { $0.length > 0 } || !bookmarks.isEmpty || practiceSlots.contains { $0.hasA } }
            return ((try? ResumeStore.read(sessionPaths.resumeFile)) ?? nil)?.hasContent ?? exists
        }()
        let discard = { [weak self] in
            guard let self else { return }
            let remove = { [weak self] in
                self?.removeTempProject()
                next()
            }
            if isCurrent { self.releaseSession(then: remove) } else { remove() }
        }
        guard exists || isCurrent else { next(); return }
        switch FeatureGate.previousSession(hasContent: hasContent, entitlements.state) {
        case .nothingToAsk, .replace:
            if hasContent { flash(L("The previous temporary session was replaced (unlock Recording to keep sessions)")) }
            discard()
        case .askKeep:
            let manifest = (try? Engine.manifestJSON(projectDir: sessionPaths.tempProject)) ?? [:]
            let romName = (manifest["rom"] as? [String: Any])?["name"] as? String ?? "?"
            alert = AppAlert(title: String(localized: "An unsaved previous session remains"),
                             message: L("Keep the previous session (ROM: %@) as a project in the Projects folder? If you don’t keep it, it is discarded.", romName),
                             actions: [
                                AppAlert.Action(title: L("Keep as Project")) { [weak self] in
                                    guard let self else { return }
                                    let keep = { [weak self] in
                                        if self?.keepTempAsProject(romName: romName) == true { next() }
                                    }
                                    if isCurrent { self.releaseSession(then: keep) } else { keep() }
                                },
                                AppAlert.Action(title: String(localized: "Don’t Save"), role: .destructive) { discard() },
                                AppAlert.Action(title: String(localized: "Cancel"), role: .cancel),
                             ])
        }
    }

    /// Moves the (closed) temporary project to Projects/<ROM> <date>.nesrec. False on failure.
    private func keepTempAsProject(romName: String) -> Bool {
        let name = URL(fileURLWithPath: romName).deletingPathExtension().lastPathComponent
        let dest = LibraryScanner.newProjectURL(projectsDir: library.paths.projects, romName: name, date: Date())
        do { try SessionResume.moveTempProject(sessionPaths, to: dest) } catch {
            showError(String(localized: "Couldn’t save"), "\(dest.lastPathComponent)\n\(error.localizedDescription)")
            return false
        }
        if lastResume?.isTemp == true { writeResume(nil) }
        library.refresh()
        flash(L("Kept as a project"))
        return true
    }

    /// projectDir nil = the temporary project (or memory when persistence is unavailable).
    func createSession(rom: URL, projectDir requestedDir: URL?, autoplay: Bool = false) {
        var projectDir = requestedDir
        if projectDir == nil && persistSessions { projectDir = sessionPaths.tempProject }
        let dirExisted = projectDir.map { FileManager.default.fileExists(atPath: $0.path) } ?? true
        let isTemp = requestedDir == nil
        beginBusy()
        DispatchQueue.global(qos: .userInitiated).async {
            defer { DispatchQueue.main.async { self.endBusy() } }
            do {
                let s = try EngineSession.create(rom: rom, projectDir: projectDir)
                DispatchQueue.main.async {
                    self.install(s, recovered: false)
                    if autoplay { self.resumeNow() }
                    if projectDir != nil && !isTemp { self.library.refresh() }
                }
            } catch let e as RNError {
                if let projectDir, !dirExisted { try? FileManager.default.removeItem(at: projectDir) }
                DispatchQueue.main.async {
                    let hint = e.status == RN_ERR_ROM_INVALID ? "\n" + String(localized: "This ROM can’t be loaded (unsupported mapper or invalid file).") : ""
                    self.showError(String(localized: "Couldn’t create the project"), e.message + hint)
                }
            } catch {
                DispatchQueue.main.async { self.showError(String(localized: "Couldn’t create the project"), "\(error)") }
            }
        }
    }

    func openProject(_ url: URL, romOverride: URL? = nil, dropCorrupt: Bool = false, dropCorruptPractice: Bool = false,
                     resume: ResumeRecord? = nil) {
        beginBusy()
        DispatchQueue.global(qos: .userInitiated).async {
            defer { DispatchQueue.main.async { self.endBusy() } }
            do {
                let s = try EngineSession.open(projectDir: url, romOverride: romOverride, dropCorruptStates: dropCorrupt,
                                               dropCorruptPractice: dropCorruptPractice)
                DispatchQueue.main.async { self.install(s, recovered: s.recovered, resume: resume) }
            } catch let e as RNError {
                DispatchQueue.main.async {
                    self.handleOpenError(e, url: url, romOverride: romOverride, dropCorrupt: dropCorrupt,
                                         dropCorruptPractice: dropCorruptPractice, resume: resume)
                }
            } catch {
                DispatchQueue.main.async { self.showError(String(localized: "Couldn’t open the project"), "\(error)") }
            }
        }
    }

    /// iOS moves the app's container when it is reinstalled, so a project's ROM path goes stale:
    /// the ROM with the project's SHA-256 is looked up in the library first.
    private func handleOpenError(_ e: RNError, url: URL, romOverride: URL?, dropCorrupt: Bool, dropCorruptPractice: Bool,
                                 resume: ResumeRecord?) {
        let manifest = (try? Engine.manifestJSON(projectDir: url)) ?? [:]
        let rom = manifest["rom"] as? [String: Any] ?? [:]
        let romName = rom["name"] as? String ?? "?"
        let romSHA = rom["sha256"] as? String ?? ""
        switch e.status {
        case RN_ERR_ROM_NOT_FOUND where romOverride == nil:
            if let match = library.rom(sha256: romSHA) {
                openProject(url, romOverride: match.url, dropCorrupt: dropCorrupt, dropCorruptPractice: dropCorruptPractice, resume: resume)
                return
            }
            showError(String(localized: "ROM not found"),
                      L("Put the ROM “%1$@” into the ROM folder (Files › On My iPhone › %2$@ › ROM) and try again.\nSHA-256: %3$@", romName, RNProduct.name, romSHA))
        case RN_ERR_CORE_MISMATCH:
            showError(String(localized: "This project was recorded with a different emulation core"),
                      L("Open it with the version of %@ it was recorded with. See docs/COMPATIBILITY.md for details.", RNProduct.name))
        case RN_ERR_CORRUPT where !dropCorrupt || !dropCorruptPractice:
            let practice = e.message.lowercased().contains("practice")
            alert = AppAlert(title: practice ? String(localized: "The practice section (A/B) data is damaged") : String(localized: "The project files are damaged"),
                             message: e.message, actions: [
                                AppAlert.Action(title: practice ? String(localized: "Discard Damaged Sections and Open") : String(localized: "Discard Damaged Checkpoints and Open"),
                                                role: .destructive) { [weak self] in
                                    self?.openProject(url, romOverride: romOverride, dropCorrupt: dropCorrupt || !practice,
                                                      dropCorruptPractice: dropCorruptPractice || practice, resume: resume)
                                },
                                AppAlert.Action(title: String(localized: "Cancel"), role: .cancel),
                             ])
        case RN_ERR_UNSUPPORTED_FORMAT:
            showError(String(localized: "This project was made with a newer version of ReplayNES"), String(localized: "Please update the app.") + "\n\(e.message)")
        default:
            showError(String(localized: "Couldn’t open the project"), "\(e.statusName): \(e.message)")
        }
    }

    private func install(_ s: EngineSession, recovered: Bool, resume: ResumeRecord? = nil) {
        if current != nil { captureThumbnail() }
        showLibrary = false
        let dir = s.projectDir
        let isTemp = persistSessions && sessionPaths.isTempProject(dir)
        var record = ResumeRecord(projectPath: dir, isTemp: isTemp, romPath: s.romPath, romSHA256: s.romSHA256,
                                  frame: s.frame, atTakeEnd: s.frame >= s.takeLength,
                                  mode: s.mode == RN_MODE_REPLAY ? .replay : .record,
                                  hasContent: SessionResume.hasRecordedContent(s))
        if let resume {
            record.frame = SessionResume.targetFrame(resume, takeLength: s.takeLength) ?? record.frame
            record.atTakeEnd = record.frame == s.takeLength
            record.mode = resume.mode ?? record.mode
            record.practiceSlot = resume.practiceSlot
        }
        current = SessionIdentity(projectPath: dir, isTemp: isTemp, romSHA256: record.romSHA256)
        pendingResume = nil
        beginPlay(record.romSHA256)
        emu.perform { e in
            e.tempSession = isTemp
            e.install(s)
            if let resume { e.applyResume(resume) }
        }
        writeResume(dir.isEmpty ? nil : record)
        if resume != nil {
            flash(String(localized: "Resumed where you left off"))
        } else if recovered {
            showError(String(localized: "Unsaved work was restored"),
                      L("%@ didn’t close normally last time, so the recording up to the last autosave was restored.", RNProduct.name))
        }
        updateUIMode()
    }

    /// Game › Save: a project saves; the temporary session becomes a project (recording unlocked).
    func save() {
        guard whenIdle({ [weak self] in self?.save() }) else { return }
        guard status.hasSession, let c = current else { return }
        guard entitlements.allows(.save) else { showLocked(.recording); return }
        if c.isTemp {
            let romName = URL(fileURLWithPath: status.romPath).lastPathComponent
            let practicing = status.practicing
            beginBusy()
            emu.perform({ e -> String? in
                guard let s = e.session else { return String(localized: "No session") }
                do { try s.save() } catch { return (error as? LocalizedError)?.errorDescription ?? "\(error)" }
                e.install(nil)
                return nil
            }, completion: { [weak self] failure in
                self?.tempSaved(failure: failure, romName: romName, practicing: practicing)
                self?.endBusy()
            })
            return
        }
        emu.perform({ e -> Bool in
            guard let s = e.session else { return false }
            do { try s.save(); e.markStructureDirty(); return true } catch {
                e.reportError(String(localized: "Couldn’t save (your work is still kept in memory)"), error)
                return false
            }
        }, completion: { [weak self] ok in
            if ok { self?.flash(String(localized: "Saved")) }
        })
    }

    /// The temporary session was saved and closed on the emulation thread: it becomes a project.
    private func tempSaved(failure: String?, romName: String, practicing: Bool) {
        if let failure { showError(String(localized: "Couldn’t save (your work is still in the temporary location)"), failure); return }
        current = nil
        let name = URL(fileURLWithPath: romName).deletingPathExtension().lastPathComponent
        let dest = LibraryScanner.newProjectURL(projectsDir: library.paths.projects, romName: name, date: Date())
        var target = dest
        do { try SessionResume.moveTempProject(sessionPaths, to: dest) } catch {
            target = sessionPaths.tempProject
            showError(String(localized: "Couldn’t save (your work is still in the temporary location)"), error.localizedDescription)
        }
        openProject(target, resume: ResumeRecord(projectPath: target.path, isTemp: target == sessionPaths.tempProject,
                                                 practiceSlot: practicing ? -1 : nil))
        library.refresh()
        flash(String(localized: "Saved"))
    }

    /// Game › Close: back to the library. Everything is persisted; the session stays offered by the
    /// Continue card (a temporary one until another game replaces it).
    func closeProject() {
        guard whenIdle({ [weak self] in self?.closeProject() }) else { return }
        guard current != nil else { return }
        captureThumbnail()
        updateResumeRecord()
        let record = lastResume
        releaseSession(flush: true) { [weak self] in
            guard let self else { return }
            self.pendingResume = record
            self.showLibrary = false
            self.updateUIMode()
        }
    }

    /// Closes the session on the emulation thread (paused; with `flush` a full save first), then
    /// on the main thread forgets it and runs `done`. Never waits on the main thread.
    private func releaseSession(flush: Bool = false, then done: @escaping () -> Void = {}) {
        flushPlay()
        playSHA = ""
        beginBusy()
        emu.perform({ e in
            e.paused = true
            if flush { _ = e.flushForResume(fullSave: true) }
            e.install(nil)
        }, completion: { [weak self] in
            self?.current = nil
            done()
            self?.endBusy()
        })
    }

    // MARK: busy (session changes in flight)

    private func beginBusy() {
        busyCount += 1
        // A spinner only when it takes noticeably long (no flicker for a quick close).
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
            guard let self, self.busyCount > 0 else { return }
            self.showBusy = true
        }
    }

    private func endBusy() {
        busyCount = max(0, busyCount - 1)
        guard busyCount == 0 else { return }
        showBusy = false
        let run = afterBusy
        afterBusy = []
        // Next main-loop turn: after the completion that ended the change has finished.
        if !run.isEmpty { DispatchQueue.main.async { run.forEach { $0() } } }
    }

    /// True when no session change is in flight; otherwise `retry` runs once it is done (a second
    /// tap / a test hook right behind the first change waits instead of racing it).
    private func whenIdle(_ retry: @escaping () -> Void) -> Bool {
        guard busyCount > 0 else { return true }
        afterBusy.append(retry)
        return false
    }

    // MARK: Restart Recording / Start Over (as ProjectReset.swift on macOS)

    func resetProjectPrompt() {
        guard status.hasSession, let c = current else { return }
        let saved = !status.projectPath.isEmpty && !c.isTemp
        var info = String(localized: "The recording starts over from power-on: every take, bookmark and the take history are deleted. The ROM and the project location stay the same.")
        info += "\n\n" + (saved ? L("A backup copy of the project is kept in the Backups folder first.")
                                : String(localized: "This session isn’t saved as a project, so no backup is made."))
        alert = AppAlert(title: String(localized: "Reset this project?"), message: info, actions: [
            AppAlert.Action(title: String(localized: "Reset"), role: .destructive) { [weak self] in
                self?.performProjectReset(keepPracticeSlots: true, backup: saved)
            },
            AppAlert.Action(title: String(localized: "Cancel"), role: .cancel),
        ])
    }

    func performProjectReset(keepPracticeSlots: Bool, backup: Bool) {
        guard whenIdle({ [weak self] in self?.performProjectReset(keepPracticeSlots: keepPracticeSlots, backup: backup) }) else { return }
        let backups = library.paths.root.appendingPathComponent("Backups", isDirectory: true)
        beginBusy()
        emu.perform({ e -> String? in
            guard let s = e.session else { return nil }
            if s.mode == RN_MODE_PRACTICE { try? s.setMode(RN_MODE_RECORD) }
            if e.fastForwardActive { e.endFastForward(s) }
            if backup, !s.projectDir.isEmpty {
                do {
                    if s.hasUnsavedChanges { try s.autosave() }
                    try ProjectBackup.makeBackup(of: URL(fileURLWithPath: s.projectDir)) { copy in
                        try FileManager.default.createDirectory(at: backups, withIntermediateDirectories: true)
                        let dest = backups.appendingPathComponent(copy.lastPathComponent, isDirectory: true)
                        try FileManager.default.moveItem(at: copy, to: dest)
                        return dest
                    }
                } catch {
                    return String(localized: "Couldn’t make a backup copy, so the project was not reset.") + "\n\n"
                        + ((error as? LocalizedError)?.errorDescription ?? "\(error)")
                }
            }
            do { try s.reset(keepPracticeSlots: keepPracticeSlots) } catch {
                e.paused = true
                e.markStructureDirty()
                e.publishVideo()
                return (error as? LocalizedError)?.errorDescription ?? "\(error)"
            }
            e.install(s)
            return nil
        }, completion: { [weak self] err in
            self?.projectResetDone(err, keepPracticeSlots: keepPracticeSlots)
            self?.endBusy()
        })
    }

    private func projectResetDone(_ err: String?, keepPracticeSlots: Bool) {
        if let msg = err {
            showError(String(localized: "Couldn’t reset the project"), msg)
            return
        }
        if let c = current, !c.projectPath.isEmpty {
            writeResume(ResumeRecord(projectPath: c.projectPath, isTemp: c.isTemp, romPath: status.romPath, romSHA256: c.romSHA256,
                                     frame: 0, atTakeEnd: true, mode: .record, hasContent: keepPracticeSlots && practiceSlots.contains { $0.hasA }))
        }
        flash(String(localized: "Project reset"))
    }

    // MARK: play history

    private func beginPlay(_ sha: String) {
        if !playSHA.isEmpty && playSHA == sha { flushPlay(); return }
        flushPlay()
        playSHA = sha
        playStart = Date()
        playMark = ProcessInfo.processInfo.systemUptime
        library.recordPlay(sha, start: playStart, seconds: 0)
    }

    private func flushPlay() {
        guard !playSHA.isEmpty else { return }
        let t = ProcessInfo.processInfo.systemUptime
        library.recordPlay(playSHA, start: playStart, seconds: t - playMark)
        playMark = t
    }

    // MARK: session persistence / resume

    private func setupSessionPersistence() {
        ThumbnailStore.dir = sessionPaths.root.appendingPathComponent("Thumbnails", isDirectory: true)
        do { try sessionPaths.ensure() } catch {
            NSLog("ReplayNES: session folder unavailable (\(error)); sessions are not persisted")
            return
        }
        sessionLock = SessionLock(url: sessionPaths.lockFile)
        persistSessions = true
        library.setCatalogFile(sessionPaths.root.deletingLastPathComponent().appendingPathComponent(RNF_LIBRARY_PREFS_FILE))
        resumeTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.updateResumeRecord()
            self.playFlushTicks += 1
            if self.playFlushTicks >= 60 { self.playFlushTicks = 0; self.flushPlay() }
        }
    }

    /// The last session is offered by the library's Continue card (nothing reopens by itself).
    private func loadPendingResume() {
        pendingResume = nil
        guard persistSessions else { return }
        rebaseResumeRecord()
        switch SessionResume.decide(paths: sessionPaths, explicitOpen: false, legacyProjectPath: "") {
        case .none:
            return
        case .projectMissing:
            writeResume(nil)
        case .resume(let r):
            pendingResume = r
        }
    }

    /// The app container's path changes when iOS reinstalls the app: a resume record that points
    /// into the old container is moved to the same place in the current one.
    private func rebaseResumeRecord() {
        guard let rec = (try? ResumeStore.read(sessionPaths.resumeFile)) ?? nil,
              !FileManager.default.fileExists(atPath: rec.projectPath) else { return }
        let home = NSHomeDirectory()
        for anchor in ["/Documents/", "/Library/"] {
            guard let range = rec.projectPath.range(of: anchor) else { continue }
            let moved = home + anchor + rec.projectPath[range.upperBound...]
            if FileManager.default.fileExists(atPath: moved) {
                var fixed = rec
                fixed.projectPath = moved
                try? ResumeStore.write(fixed, to: sessionPaths.resumeFile)
                return
            }
        }
    }

    /// The app leaves the foreground: pause, persist (temporary: full save; project: journal) and
    /// write the resume record right away (iOS may end the process without notice from here on).
    /// Nothing waits on the main thread: a background task keeps the process running until the
    /// emulation thread has saved and the resume queue (record, thumbnail) is written.
    func enterBackground() {
        wasInBackground = true
        releaseTouch()
        guard current != nil else { return }
        setPaused(true)
        captureThumbnail()
        updateResumeRecord()
        flushPlay()
        let isTemp = current?.isTemp == true
        let app = UIApplication.shared
        var task = UIBackgroundTaskIdentifier.invalid
        let finish = {
            guard task != .invalid else { return }
            app.endBackgroundTask(task)
            task = .invalid
        }
        task = app.beginBackgroundTask(withName: "ReplayNES save", expirationHandler: finish)
        emu.perform({ e in
            e.paused = true
            _ = e.flushForResume(fullSave: isTemp)
        }, completion: { [weak self] in
            guard let self else { finish(); return }
            self.updateResumeRecord()
            self.resumeQueue.async { DispatchQueue.main.async(execute: finish) }
        })
    }

    /// Back from the background (not the launch's first activation: the audio session was set up
    /// by start(), and reconfiguring it while the engine starts stalls the audio server).
    func enterForeground() {
        drainShareInbox()
        guard wasInBackground else { return }
        wasInBackground = false
        audioSession.reactivate()
        library.refresh()
    }

    private func updateResumeRecord() {
        guard persistSessions, let c = current, status.hasSession, !c.projectPath.isEmpty,
              status.projectPath == c.projectPath else { return }
        let content = takes.contains { $0.length > 0 } || !bookmarks.isEmpty || practiceSlots.contains { $0.hasA }
        let r = ResumeRecord(projectPath: c.projectPath, isTemp: c.isTemp, romPath: status.romPath, romSHA256: c.romSHA256,
                             frame: status.frame, atTakeEnd: status.frame >= status.takeLength,
                             mode: status.practicing ? nil : (status.recording ? .record : .replay),
                             practiceSlot: status.practicing ? status.practiceSlot : nil, hasContent: content)
        if let last = lastResume, last.sameState(as: r) { return }
        writeResume(r)
    }

    private func writeResume(_ r: ResumeRecord?) {
        if r == nil { pendingResume = nil }
        guard persistSessions else { return }
        lastResume = r
        let url = sessionPaths.resumeFile
        resumeQueue.async {
            if let r {
                do { try ResumeStore.write(r, to: url) } catch { NSLog("ReplayNES: cannot write \(url.path): \(error)") }
            } else {
                ResumeStore.clear(url)
            }
        }
    }

    private func removeTempProject() {
        do {
            if sessionPaths.tempProjectExists { try FileManager.default.removeItem(at: sessionPaths.tempProject) }
        } catch {
            showError(String(localized: "Couldn’t delete the temporary data"), error.localizedDescription)
        }
        if lastResume?.isTemp == true || pendingResume?.isTemp == true { writeResume(nil) }
    }

    func captureThumbnail() {
        guard persistSessions, let c = current, status.hasSession else { return }
        var picture: VideoPicture?
        _ = emu.frames.readIfNewer(than: .max) { p, m in
            picture = VideoPicture(pixels: Array(UnsafeBufferPointer(start: p, count: m.geometry.width * m.geometry.height)), geometry: m.geometry)
        }
        guard let picture else { return }
        ThumbnailStore.save(picture, project: c.projectPath, romSHA: c.romSHA256, on: resumeQueue) { [weak self] in
            self?.objectWillChange.send()
        }
    }

    /// Thumbnail of the resume record's session (Continue card).
    func continueThumbnail() -> CGImage? {
        guard let r = pendingResume else { return nil }
        return ThumbnailStore.image(project: r.projectPath) ?? ThumbnailStore.image(romSHA: r.romSHA256)
    }

    // MARK: ROM import (document picker, Files "Open in", AirDrop)

    /// Copies ROMs into ROM/ from the in-app picker (security-scoped URLs), off the main thread.
    func importROMs(_ urls: [URL]) {
        importQueue.enqueue(urls, fromOtherApp: false)
    }

    /// Files opened from another app (Files › Share › ReplayNES, a tap on a .nes / .nesrec,
    /// AirDrop): every URL context the scene delegate gets, at once (SceneDelegate).
    /// ROMs are imported off the main thread (ROMImportQueue): one library refresh and one toast at
    /// the end; no game is started (see importFinished). A single .nesrec opens that project.
    func open(urls: [URL]) {
        let projects = urls.filter { RNProduct.isProject($0) }
        let others = urls.filter { !RNProduct.isProject($0) }
        if projects.count == 1 && others.isEmpty {
            let url = projects[0]
            whenStarted { [weak self] in self?.continueProject(url) }
            return
        }
        // Projects are only opened one at a time (a .nesrec among ROMs is skipped and counted).
        importQueue.enqueue(others + projects, fromOtherApp: true)
    }

    private func importStarted(_ count: Int) {
        if count >= 10 { flash(L("Importing %lld files…", Int64(count)), duration: 60) }
    }

    private func importFinished(_ r: ROMImportResult, _ batch: ROMImportQueue.Batch) {
        if let root = ShareInbox.standardRoot() { ShareInbox.finish(batch.urls, root: root) }
        NSLog("ReplayNES import: \(batch.urls.count) file(s): \(r.added.count) added, \(r.existing.count) existing, \(r.invalid.count) invalid, \(r.failed.count) failed")
        for (name, why) in r.failed { NSLog("ReplayNES import: \(name): \(why)") }
        library.refresh()
        flash(r.summary, duration: 5)
        // No game is started by an import (also not for one file): iOS hands over only the first
        // file of a multi-item Share › ReplayNES, so one URL doesn't mean the user picked one game,
        // and starting a game would replace the screen (and the session) they came back to.
    }

    // MARK: Share Extension inbox ("Add to ReplayNES": every file of a multi-file share)

    /// The extension posts a Darwin notification after each share: import at once when running.
    private func observeShareInbox() {
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), nil, { _, _, _, _, _ in
            DispatchQueue.main.async { AppModel.shared.drainShareInbox() }
        }, ShareInbox.darwinNotification as CFString, nil, .deliverImmediately)
    }

    private let shareQueue = DispatchQueue(label: "replaynes.share-inbox", qos: .userInitiated)

    /// Claims the batches the Share Extension left (off the main thread) and imports them: ROMs
    /// through the import queue (one toast); projects (.nesrec) move into Projects/, and a share of
    /// exactly one project opens it.
    func drainShareInbox() {
        guard started, let root = ShareInbox.standardRoot() else { return }
        let projectsDir = library.paths.projects
        shareQueue.async { [weak self] in
            let items = ShareInbox.claim(root: root)
            guard !items.isEmpty else { return }
            NSLog("ReplayNES: share inbox: \(items.count) item(s)")
            let fm = FileManager.default
            var roms: [URL] = [], projects: [URL] = []
            for u in items {
                guard RNProduct.isProject(u) else { roms.append(u); continue }
                try? fm.createDirectory(at: projectsDir, withIntermediateDirectories: true)
                let base = u.deletingPathExtension().lastPathComponent
                var dest = projectsDir.appendingPathComponent(u.lastPathComponent, isDirectory: true)
                var n = 2
                while fm.fileExists(atPath: dest.path) { dest = projectsDir.appendingPathComponent("\(base) \(n).\(RNProduct.projectExtension)", isDirectory: true); n += 1 }
                if (try? fm.moveItem(at: u, to: dest)) != nil { projects.append(dest) }
            }
            if roms.isEmpty { ShareInbox.finish(items, root: root) }
            DispatchQueue.main.async {
                guard let self else { return }
                if !roms.isEmpty { self.importQueue.enqueue(roms, fromOtherApp: true) }
                if !projects.isEmpty { self.library.refresh() }
                if projects.count == 1 && roms.isEmpty {
                    self.continueProject(projects[0])
                } else if !projects.isEmpty {
                    self.flash(L("Added %lld project(s) to the Projects folder", Int64(projects.count)), duration: 5)
                }
            }
        }
    }

    /// Runs `f` now when start() has run, else at its end (a cold launch by "Open in").
    private func whenStarted(_ f: @escaping () -> Void) {
        if started { f() } else { afterStart.append(f) }
    }

    // MARK: export (MP4)

    func requestExport() {
        guard entitlements.allows(.exportMP4) else { showLocked(.mp4Export); return }
        guard status.hasSession, status.takeLength > 0 else { return }
        showExport = true
    }

    func startExport(_ settings: ExportSettings, to url: URL) {
        let job = ExportJob(url: url)
        exportJob = job
        emu.perform { e in
            guard let s = e.session else { return }
            let renderer: OpaquePointer
            do {
                renderer = try s.makeRenderer(start: settings.startFrame, end: settings.endFrame)
            } catch {
                DispatchQueue.main.async { job.error = (error as? LocalizedError)?.errorDescription ?? "\(error)"; job.finished = true }
                return
            }
            DispatchQueue.global(qos: .utility).async {
                let exporter = MP4Exporter(renderer: renderer, settings: settings, url: url)
                var lastUI = 0.0
                do {
                    let r = try exporter.run(progress: { d, t in
                        let now = CACurrentMediaTime()
                        if now - lastUI > 0.1 || d == t {
                            lastUI = now
                            DispatchQueue.main.async { job.done = d; job.total = t }
                        }
                    }, isCancelled: { job.isCancelled })
                    DispatchQueue.main.async { job.result = r; job.finished = true }
                } catch {
                    let msg = (error as? LocalizedError)?.errorDescription ?? "\(error)"
                    DispatchQueue.main.async { job.error = msg; job.finished = true }
                }
            }
        }
    }
}
