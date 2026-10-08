// App state, project lifecycle (new/open/save/recovery) and export jobs. Main thread only.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers

extension UTType {
    static let nesrec = UTType(exportedAs: "io.github.replaynes.nesrec", conformingTo: .package)
    static let nesROM = UTType(importedAs: "io.github.replaynes.nes-rom", conformingTo: .data)
}

final class ExportJob: ObservableObject {
    @Published var done: UInt64 = 0
    @Published var total: UInt64 = 0
    @Published var finished = false
    @Published var result: ExportResult?
    @Published var error: String?
    let url: URL
    private let lock = NSLock()
    private var cancelled = false
    init(url: URL) { self.url = url }
    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
}

final class AppModel: ObservableObject {
    static let shared = AppModel()

    let input = InputManager()
    let emu: EmulationController
    let library = LibraryModel()

    /// Latest emulation status (~20 updates a second while running). Views observing AppModel are
    /// only invalidated when its coarse part changes (EmuStatus.coarse: not the frame counters);
    /// views showing counters (timecodes, timeline) observe `clock` instead, so the whole window
    /// (toolbar, menus, layout) is not rebuilt for every update.
    var status = EmuStatus() {
        willSet { if newValue.coarse != status.coarse { objectWillChange.send() } }
        didSet {
            if status.paused != oldValue.paused || status.hasSession != oldValue.hasSession {
                updateImmersive()
                updateUIMode()
            }
        }
    }
    /// Quick Menu (QuickMenu/QuickMenuController.swift) and the library screen's focus.
    let quickMenu = QuickMenuController()
    let libraryNav = LibraryNav()
    /// The library (start screen) shown over an open game (Game › Choose Game).
    @Published var showLibrary = false
    /// Menu pill label after "Menu": the connected controller's chord, or Esc.
    @Published var pillGlyph = "Esc"
    /// Launched by a script (--snapshot, --inject-*, ...): no first-run effects are remembered.
    var scripted = false
    /// Full-screen play with the window chrome hidden (FullScreenChrome.swift).
    @Published var immersive = false
    let chrome = FullScreenChrome()
    let clock = PlaybackClock()
    @Published var bookmarks: [BookmarkInfo] = []
    @Published var takes: [TakeInfo] = []
    @Published var controllers: [String] = []
    @Published var inputConfig: InputCatalog.Config
    @Published var stats = LatencyMeter.Snapshot()
    @Published var notice: String?
    @Published var showExport = false
    @Published var exportJob: ExportJob?
    @Published var capturingAction: String?
    @Published var practiceSlots: [PracticeSlotInfo] = (0..<EngineSession.practiceSlotCount).map { PracticeSlotInfo(index: $0) }
    /// Practice OSD (A/B slots) over the viewport. Always shown while practicing.
    @Published var showPracticePanel = false
    /// Set while --snapshot captures the window: the current frame drawn by SwiftUI under the overlays.
    @Published var snapshotFrame: CGImage?

    // Preferences
    @AppStorage("showLatency") var showLatency = false { didSet { objectWillChange.send() } }
    @AppStorage("pauseAfterRewind") var pauseAfterRewind = true { didSet { pushPrefs(); objectWillChange.send() } }
    @AppStorage("autosaveInterval") var autosaveInterval = 2.0 { didSet { pushPrefs(); objectWillChange.send() } }
    /// Pixel-perfect (largest integer scale that fits, pixel-perfect) vs FILL (fill the window, aspect kept).
    @AppStorage("integerScale") var integerScale = true { didSet { objectWillChange.send() } }
    @AppStorage("displayPAR87") var displayPAR87 = false { didSet { objectWillChange.send() } }
    @AppStorage("hideOverscan") var hideOverscan = true { didSet { objectWillChange.send() } }
    @AppStorage("volume") var volume = 0.8 { didSet { emu.audio.volume = Float(volume); objectWillChange.send() } }
    /// While paused, controller D-pad ←/→ step one frame back/forward (not sent to the game).
    @AppStorage("dpadStepWhenPaused") var dpadStepWhenPaused = true { didSet { input.setPausedStepEnabled(dpadStepWhenPaused); objectWillChange.send() } }
    /// Photosensitive flash reduction level (FlashLevel raw value). Default Standard (on): safety first.
    @AppStorage("flashReduction") var flashReduction = FlashLevel.standard.rawValue { didSet { pushPrefs(); objectWillChange.send() } }
    @AppStorage("showFlashIndicator") var showFlashIndicator = true { didSet { objectWillChange.send() } }

    var flashLevel: FlashLevel { FlashLevel(rawValue: flashReduction) ?? .standard }

    /// Crash marker of versions before resume.json (read once at launch, see loadPendingResume).
    @AppStorage("openProjectPath") private var openProjectPath = ""

    // Session persistence (SessionResume.swift): every session is on disk and resumed at launch.
    private(set) var sessionPaths = SessionPaths.standard
    private var sessionLock: SessionLock?
    /// false: second instance, scripted run without --session-root, or the folder is unusable.
    /// Then quick play stays in memory and nothing is resumed (the behaviour before 0.3).
    private(set) var persistSessions = false
    struct SessionIdentity { let projectPath: String; let isTemp: Bool; let romSHA256: String }
    /// The installed session (main-thread view of what the emulation thread owns).
    private(set) var current: SessionIdentity?
    private var lastResume: ResumeRecord?
    /// The launch's resume record (also after a crash / force quit): offered by the library's
    /// "Continue" card instead of being reopened by itself.
    @Published private(set) var pendingResume: ResumeRecord?
    // Play history (library.json): the session's ROM, its start, the time not yet recorded.
    private var playSHA = ""
    private var playStart = Date()
    private var playMark = ProcessInfo.processInfo.systemUptime
    private var playFlushTicks = 0
    private var resumeTimer: Timer?
    private let resumeQueue = DispatchQueue(label: "replaynes.resume", qos: .utility)

    /// The temporary project of a session without a project (window title, menus).
    func isTempSession(_ projectPath: String) -> Bool { persistSessions && sessionPaths.isTempProject(projectPath) }

    var displayOptions: DisplayOptions {
        DisplayOptions(integerScale: integerScale, pixelAspect87: displayPAR87, hideOverscan: hideOverscan)
    }

    weak var mainWindow: NSWindow? { didSet { if mainWindow !== oldValue { chrome.window = mainWindow } } }
    private var statsTimer: Timer?
    private var activity: NSObjectProtocol?
    private var noticeWork: DispatchWorkItem?
    /// What the menu bar observes (see MenuState.swift).
    let menu = MenuState()
    private var menuSubs: Set<AnyCancellable> = []
    private var menuRefreshQueued = false

    private init() {
        emu = EmulationController(input: input)
        inputConfig = input.config
        emu.onStatus = { [weak self] st in
            guard let self else { return }
            if self.status != st { self.status = st; self.clock.status = st }
        }
        emu.onStructure = { [weak self] st in
            guard let self else { return }
            if self.bookmarks != st.bookmarks { self.bookmarks = st.bookmarks }
            if self.takes != st.takes { self.takes = st.takes }
            if !st.practiceSlots.isEmpty, self.practiceSlots != st.practiceSlots { self.practiceSlots = st.practiceSlots }
        }
        emu.onError = { [weak self] title, msg in self?.showError(title, msg) }
        emu.onNotice = { [weak self] text in self?.flash(text) }
        input.onConfigChanged = { [weak self] c in self?.inputConfig = c }
        input.onControllersChanged = { [weak self] c in self?.controllers = c; self?.updateUIMode() }
        input.onNav = { [weak self] n in self?.handleNav(n) }
        input.onPausedConfirm = { [weak self] in
            guard let self, self.status.hasSession, self.status.paused, !self.quickMenu.isOpen, !self.showLibrary else { return }
            self.togglePause()
        }
        input.onDisconnect = { [weak self] name in
            guard let self else { return }
            self.emu.perform { emu in emu.paused = true }
            self.flash(String(localized: "Paused because \(name) was disconnected"))
        }
        input.onPausedStep = { [weak emu] dir, down in emu?.perform { e in e.pausedStep(dir, down: down) } }
        chrome.onChange = { [weak self] in self?.updateImmersive() }
        quickMenu.model = self
        input.keyboardEnabled = { [weak self] in
            guard let w = NSApp.keyWindow, w === self?.mainWindow else { return false }
            return !(w.firstResponder is NSText)
        }
        // objectWillChange fires before the new value is stored: read it on the next turn.
        objectWillChange.merge(with: StreamOutputModel.shared.objectWillChange)
            .sink { [weak self] _ in self?.queueMenuRefresh() }
            .store(in: &menuSubs)
        refreshMenu()
    }

    private func queueMenuRefresh() {
        guard !menuRefreshQueued else { return }
        menuRefreshQueued = true
        DispatchQueue.main.async { [weak self] in
            self?.menuRefreshQueued = false
            self?.refreshMenu()
        }
    }

    private func refreshMenu() {
        let st = status
        menu.set(MenuValues(hasSession: st.hasSession, paused: st.paused, recording: st.recording,
                            practicing: st.practicing, slowOn: st.slow != .normal, takeEmpty: st.takeLength == 0,
                            undoAvailable: st.undoDepth > 0, integerScale: integerScale,
                            showLatency: showLatency, streamOn: StreamOutputModel.shared.isOn))
    }

    func start() {
        // Keep emulation timing and controller input precise while another app (OBS) is in front:
        // no App Nap throttling. The display may still sleep normally.
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
                                                        reason: "NES emulation")
        input.setPausedStepEnabled(dpadStepWhenPaused)
        input.setPausedStepMode(true)
        emu.audio.volume = Float(volume)
        pushPrefs()
        emu.start()
        input.startKeyboard()
        input.startControllers()
        MenuPillOverlay.shared.startMonitoring { [weak self] in self?.mainWindow }
        updateUIMode()
        statsTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self, self.showLatency else { return }
            self.stats = self.emu.latency.snapshot(audio: self.emu.audio)
        }
    }

    private func pushPrefs() {
        let par = pauseAfterRewind, auto = autosaveInterval, flash = flashLevel
        emu.perform { e in e.pauseAfterRewind = par; e.autosaveInterval = auto; e.setFlashLevel(flash) }
    }

    // MARK: messages

    func flash(_ text: String) {
        notice = text
        noticeWork?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.notice = nil }
        noticeWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 4, execute: w)
    }

    func showError(_ title: String, _ message: String) {
        let a = NSAlert()
        a.alertStyle = .warning
        a.messageText = title
        a.informativeText = message
        a.runModal()
    }

    // MARK: transport (all forwarded to the emulation thread)

    func togglePause() { emu.perform { e in e.togglePause() } }
    func setPaused(_ p: Bool) { emu.perform { e in e.paused = p } }
    func frameAdvance(_ n: Int = 1) { emu.perform { e in e.paused = true; e.advanceRemaining += max(0, n) } }
    func stepBack(_ n: UInt64 = 1) {
        emu.perform { e in
            guard let s = e.session else { return }
            e.stepBack(s, n)
        }
    }
    func setSlow(_ r: SlowRate) { emu.perform { e in e.slow = r } }
    func toggleSlow() { emu.perform { e in e.slow = e.slow.toggled } }
    func toggleRecord() { emu.perform { e in e.toggleRecord() } }
    /// Seconds forward/back on the take (seek = replay only, never records).
    func jump(seconds: Double) {
        let n = UInt64(abs(seconds) * 60)
        emu.perform { e in
            guard let s = e.session else { return }
            if seconds < 0 { e.stepBack(s, n) } else { e.seekCommand(min(s.takeLength, s.frame + n)) }
        }
    }

    // MARK: practice (A/B repeat)

    func practiceSetA(_ slot: Int) { emu.perform { e in e.practiceSetA(slot) } }
    func practiceSetB(_ slot: Int) { emu.perform { e in e.practiceSetB(slot) } }
    func practiceStart(_ slot: Int) { showPracticePanel = true; emu.perform { e in e.startPractice(slot) } }
    func practiceStop() { emu.perform { e in e.stopPractice() } }
    func practiceRename(_ slot: Int, _ name: String) { emu.perform { e in e.practiceRename(slot, name) } }
    func practiceClear(_ slot: Int) { emu.perform { e in e.practiceClear(slot) } }
    /// Practice… (⇧⌘P): the Quick Menu's practice page; while practicing it leaves practice.
    func togglePracticePanel() {
        if status.practicing { practiceStop(); return }
        quickMenu.open(at: .practice)
    }

    /// Take navigation is not available while practicing (the engine refuses it).
    private func blockedInPractice() -> Bool {
        if status.practicing { flash(EmulationController.practiceBlockedText); return true }
        return false
    }
    func setRewindHeld(_ h: Bool) { emu.perform { e in e.uiRewindHeld = h } }
    func setFastForwardHeld(_ h: Bool) { emu.perform { e in e.uiFastForwardHeld = h } }
    func scrub(to f: UInt64) {
        if status.practicing { return }
        emu.perform { e in e.paused = true; e.scrubTarget = f }
    }
    func seek(to f: UInt64) { emu.perform { e in e.seekCommand(f) } }
    func softReset() { emu.perform { e in e.requestEvent(UInt8(RN_EV_SOFT_RESET)) } }
    func powerCycle() { emu.perform { e in e.requestEvent(UInt8(RN_EV_POWER_CYCLE)) } }
    func setRecording(_ r: Bool) { emu.perform { e in e.setRecording(r) } }
    func rerecordHere() { emu.perform { e in e.rerecordHere() } }
    func undoTake() { emu.perform { e in e.undoTake() } }
    func addBookmark() { emu.perform { e in e.addBookmark(name: nil) } }

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

    // MARK: project lifecycle

    /// Before another ROM / project replaces the current session (or it is closed): asks to save
    /// unsaved work. Returns false if the user cancelled. A temporary session with recorded content
    /// asks Save… (= Save As) / Don’t Save (its temporary project is deleted when replaced).
    func confirmDiscardIfNeeded() -> Bool {
        let info = emu.sync(timeout: 10) { e -> (unsaved: Bool, dir: String, takeLength: UInt64, content: Bool)? in
            guard let s = e.session else { return nil }
            return (s.hasUnsavedChanges, s.projectDir, s.takeLength, SessionResume.hasRecordedContent(s))
        } ?? nil
        guard let info else { return true }
        if current?.isTemp == true {
            guard info.content else { return true }
            let a = NSAlert()
            a.messageText = String(localized: "Do you want to save?")
            a.informativeText = String(localized: "This session hasn’t been saved as a project yet (it is stored temporarily). If you don’t save, the temporary data will be discarded.")
            a.addButton(withTitle: String(localized: "Save…"))
            a.addButton(withTitle: String(localized: "Don’t Save"))
            a.addButton(withTitle: String(localized: "Cancel"))
            switch a.runModal() {
            case .alertFirstButtonReturn: return saveTempAs()
            case .alertSecondButtonReturn: return true
            default: return false
            }
        }
        let inMemory = info.dir.isEmpty
        guard info.unsaved || (inMemory && info.takeLength > 0) else { return true }
        let a = NSAlert()
        a.messageText = String(localized: "The current project has unsaved changes")
        a.informativeText = String(localized: "Do you want to save?")
        a.addButton(withTitle: String(localized: "Save"))
        a.addButton(withTitle: String(localized: "Don’t Save"))
        a.addButton(withTitle: String(localized: "Cancel"))
        switch a.runModal() {
        case .alertFirstButtonReturn:
            return inMemory ? saveAs() : saveSync()
        case .alertSecondButtonReturn:
            return true
        default:
            return false
        }
    }

    func newProject() {
        guard confirmDiscardIfNeeded() else { return }
        let open = NSOpenPanel()
        open.title = String(localized: "Choose a ROM")
        open.message = String(localized: "Choose the NES ROM (.nes) for the new project. The ROM is not copied into the project.")
        open.allowedContentTypes = [.nesROM, .data]
        open.allowsMultipleSelection = false
        guard open.runModal() == .OK, let rom = open.url else { return }
        let save = NSSavePanel()
        save.title = String(localized: "Where to Save the Project")
        save.nameFieldStringValue = rom.deletingPathExtension().lastPathComponent + ".nesrec"
        save.allowedContentTypes = [.nesrec]
        save.canCreateDirectories = true
        guard save.runModal() == .OK, let dir = save.url else { return }
        if FileManager.default.fileExists(atPath: dir.path) {
            do { try FileManager.default.trashItem(at: dir, resultingItemURL: nil) } catch {
                showError(String(localized: "Couldn’t replace the existing project"), error.localizedDescription); return
            }
        }
        createSession(rom: rom, projectDir: dir)
    }

    /// Plays a ROM without a project (in-memory; "Save As" writes it later).
    func quickPlay() {
        guard confirmDiscardIfNeeded() else { return }
        let open = NSOpenPanel()
        open.title = String(localized: "Try a ROM (No Project, Stored Temporarily)")
        open.allowedContentTypes = [.nesROM, .data]
        guard open.runModal() == .OK, let rom = open.url else { return }
        createSession(rom: rom, projectDir: nil)
    }

    /// projectDir == nil: a session without a project; with session persistence it lives in the
    /// temporary project (SessionResume.swift), otherwise in memory.
    func createSession(rom: URL, projectDir requestedDir: URL?, autoplay: Bool = false) {
        var projectDir = requestedDir
        if projectDir == nil && persistSessions {
            guard prepareTempSlot() else { return }
            projectDir = sessionPaths.tempProject
        }
        let dirExisted = projectDir.map { FileManager.default.fileExists(atPath: $0.path) } ?? true
        let isTemp = requestedDir == nil
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let s = try EngineSession.create(rom: rom, projectDir: projectDir)
                DispatchQueue.main.async {
                    self.install(s, recovered: false)
                    if autoplay { self.setPaused(false) }
                    if projectDir != nil && !isTemp { self.library.refresh() }
                }
            } catch let e as RNError {
                // A project folder this call created (e.g. a library project whose first save
                // failed) is removed again; existing folders are never touched.
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

    // MARK: library

    /// Starts a new project for a library ROM right away, auto-saved as
    /// Projects/<ROM name> <yyyy-MM-dd HHmm>.nesrec (no save panel). Returns false if cancelled.
    @discardableResult
    func playFromLibrary(_ rom: LibraryROM) -> Bool {
        guard confirmDiscardIfNeeded(), settlePendingTemp() else { return false }
        guard library.ensureFolders() else {
            showError(String(localized: "The library folder isn’t available"), library.folderError ?? "")
            return false
        }
        let dir = LibraryScanner.newProjectURL(projectsDir: library.paths.projects, romName: rom.name, date: Date())
        createSession(rom: rom.url, projectDir: dir)
        return true
    }

    /// "Continue": opens a library project (the usual save prompt first).
    @discardableResult
    func continueProject(_ url: URL) -> Bool {
        guard confirmDiscardIfNeeded(), settlePendingTemp() else { return false }
        openProject(url)
        return true
    }

    /// The library's "Continue" (no session open): the recorded last session where it was (a
    /// temporary session or a project, journal recovered after a crash), else the latest project.
    func continueLast() {
        guard !status.hasSession else { hideLibraryScreen(); return }
        if let r = pendingResume {
            openProject(URL(fileURLWithPath: r.projectPath), resume: r)
        } else if let p = library.projectsBySHA.values.flatMap({ $0 }).max(by: { $0.modified < $1.modified }) {
            continueProject(p.url)
        }
    }

    /// Idle on the library with a temporary session left (resumable by "Continue"): starting
    /// something else asks first whether to save it. False: cancelled.
    private func settlePendingTemp() -> Bool {
        guard current == nil, persistSessions, sessionPaths.tempProjectExists else { return true }
        return prepareTempSlot()
    }

    // MARK: play history

    private func beginPlay(_ sha: String) {
        if !playSHA.isEmpty && playSHA == sha { flushPlay(); return }  // the same session reopened (Save As)
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

    func openProjectPanel() {
        guard confirmDiscardIfNeeded() else { return }
        let open = NSOpenPanel()
        open.title = String(localized: "Open Project")
        open.allowedContentTypes = [.nesrec]
        open.canChooseDirectories = true
        open.treatsFilePackagesAsDirectories = false
        guard open.runModal() == .OK, let url = open.url else { return }
        openProject(url)
    }

    /// `resume`: reopening the last session at launch (position restored, paused, no recovery alert).
    func openProject(_ url: URL, romOverride: URL? = nil, dropCorrupt: Bool = false, dropCorruptPractice: Bool = false,
                     resume: ResumeRecord? = nil) {
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let s = try EngineSession.open(projectDir: url, romOverride: romOverride, dropCorruptStates: dropCorrupt,
                                               dropCorruptPractice: dropCorruptPractice)
                let dropped = s.droppedPracticeSlots
                DispatchQueue.main.async {
                    self.install(s, recovered: s.recovered, resume: resume)
                    if dropped != 0 { self.reportDroppedPracticeSlots(dropped) }
                }
            } catch let e as RNError {
                DispatchQueue.main.async {
                    let retrying = self.handleOpenError(e, url: url, romOverride: romOverride, dropCorrupt: dropCorrupt,
                                                        dropCorruptPractice: dropCorruptPractice, resume: resume)
                    if !retrying, let resume { self.resumeFailed(resume) }
                }
            } catch {
                DispatchQueue.main.async {
                    self.showError(String(localized: "Couldn’t open the project"), "\(error)")
                    if let resume { self.resumeFailed(resume) }
                }
            }
        }
    }

    private func reportDroppedPracticeSlots(_ mask: UInt32) {
        let names = (0..<EngineSession.practiceSlotCount).filter { mask & (1 << UInt32($0)) != 0 }.map { String(localized: "Section \($0 + 1)") }
        showError(String(localized: "Discarded damaged practice sections"),
                  String(localized: "These practice sections (A/B) couldn’t be loaded and were removed: \(names.joined(separator: String(localized: ", ")))\n\nYour takes (recordings) are not affected. Set A and B again if needed."))
    }

    /// RN_ERR_CORRUPT caused by an A/B practice slot ("practice slot N" in the engine message).
    static func isPracticeCorruption(_ message: String) -> Bool { message.lowercased().contains("practice") }

    /// Explains an open failure and offers the fix where there is one. Returns true if the open is retried.
    @discardableResult
    private func handleOpenError(_ e: RNError, url: URL, romOverride: URL?, dropCorrupt: Bool, dropCorruptPractice: Bool = false,
                                 resume: ResumeRecord? = nil) -> Bool {
        let manifest = (try? Engine.manifestJSON(projectDir: url)) ?? [:]
        let rom = manifest["rom"] as? [String: Any] ?? [:]
        let romName = rom["name"] as? String ?? "?"
        let romPath = rom["lastPath"] as? String ?? "?"
        let romSHA = rom["sha256"] as? String ?? "?"
        let a = NSAlert()
        a.alertStyle = .warning
        switch e.status {
        case RN_ERR_ROM_NOT_FOUND, RN_ERR_ROM_MISMATCH:
            if e.status == RN_ERR_ROM_NOT_FOUND {
                a.messageText = String(localized: "ROM not found")
                a.informativeText = String(localized: "This project’s ROM “\(romName)” is no longer at its original location.\nOriginal location: \(romPath)\n\nPlease locate the same ROM (a file with a matching SHA-256).\nSHA-256: \(romSHA)")
            } else {
                a.messageText = String(localized: "ROM doesn’t match")
                a.informativeText = String(localized: "The selected ROM differs from the one this project was recorded with.\nRequired ROM: \(romName)\nSHA-256: \(romSHA)\n\nDetails: \(e.message)")
            }
            a.addButton(withTitle: String(localized: "Locate ROM…"))
            a.addButton(withTitle: String(localized: "Cancel"))
            guard a.runModal() == .alertFirstButtonReturn else { return false }
            let open = NSOpenPanel()
            open.title = String(localized: "Locate “\(romName)”")
            open.allowedContentTypes = [.nesROM, .data]
            guard open.runModal() == .OK, let newRom = open.url else { return false }
            openProject(url, romOverride: newRom, dropCorrupt: dropCorrupt, dropCorruptPractice: dropCorruptPractice, resume: resume)
            return true
        case RN_ERR_CORE_MISMATCH:
            let projCore = manifest["coreCompatID"] as? String ?? "?"
            a.messageText = String(localized: "This project was recorded with a different emulation core")
            a.informativeText = String(localized: "To keep replays exact, this version can’t open it (no automatic conversion).\nProject core: \(projCore)\nThis app’s core: \(Engine.coreCompatID)\n\nOpen it with the version of ReplayNES it was recorded with. See docs/COMPATIBILITY.md for details.")
            a.runModal()
            return false
        case RN_ERR_CORRUPT where Self.isPracticeCorruption(e.message) && !dropCorruptPractice:
            a.messageText = String(localized: "The practice section (A/B) data is damaged")
            a.informativeText = e.message + "\n\n" + String(localized: "You can open it by discarding only the damaged practice sections (their A/B points are lost). Your takes (recordings) are not changed.")
            a.addButton(withTitle: String(localized: "Discard Damaged Sections and Open"))
            a.addButton(withTitle: String(localized: "Cancel"))
            guard a.runModal() == .alertFirstButtonReturn else { return false }
            openProject(url, romOverride: romOverride, dropCorrupt: dropCorrupt, dropCorruptPractice: true, resume: resume)
            return true
        case RN_ERR_CORRUPT:
            a.messageText = String(localized: "The project files are damaged")
            a.informativeText = e.message
            if !dropCorrupt {
                a.informativeText += "\n\n" + String(localized: "If only checkpoints (states kept for speed) are damaged, you can open the project by discarding them. The input history (the source of truth) is not changed.")
                a.addButton(withTitle: String(localized: "Discard Damaged Checkpoints and Open"))
                a.addButton(withTitle: String(localized: "Cancel"))
                guard a.runModal() == .alertFirstButtonReturn else { return false }
                openProject(url, romOverride: romOverride, dropCorrupt: true, dropCorruptPractice: dropCorruptPractice, resume: resume)
                return true
            }
            a.runModal()
            return false
        case RN_ERR_UNSUPPORTED_FORMAT:
            a.messageText = String(localized: "This project was made with a newer version of ReplayNES")
            a.informativeText = String(localized: "Please update the app.") + "\n\(e.message)"
            a.runModal()
            return false
        default:
            a.messageText = String(localized: "Couldn’t open the project")
            a.informativeText = "\(e.statusName): \(e.message)"
            a.runModal()
            return false
        }
    }

    /// `resume`: restore that take mode / position (paused) and the practice panel; `resumeNotice`
    /// says so (false when Save As reopens the moved temporary project).
    private func install(_ s: EngineSession, recovered: Bool, resume: ResumeRecord? = nil, resumeNotice: Bool = true) {
        if current != nil { captureThumbnail() }   // the picture of the game being replaced
        showLibrary = false
        let dir = s.projectDir
        let isTemp = persistSessions && sessionPaths.isTempProject(dir)
        // A temporary session being replaced was confirmed (saved elsewhere, or Don’t Save / empty).
        if let p = current, p.isTemp, !isTemp { releaseSession(); removeTempProject() }
        // Read before the emulation thread owns the session.
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
        pendingResume = nil  // superseded by this session (its record is written below)
        beginPlay(record.romSHA256)
        showPracticePanel = resume?.practiceSlot != nil
        emu.perform { e in
            e.tempSession = isTemp
            e.install(s)
            if let resume { e.applyResume(resume) }
        }
        writeResume(dir.isEmpty ? nil : record)
        if !dir.isEmpty && !isTemp { NSDocumentController.shared.noteNewRecentDocumentURL(URL(fileURLWithPath: dir)) }
        if resume != nil {
            if resumeNotice { flash(String(localized: "Resumed where you left off")) }
        } else if recovered {
            let a = NSAlert()
            a.messageText = String(localized: "Unsaved work was restored")
            a.informativeText = String(localized: "ReplayNES didn’t quit normally last time, so the recording up to the last autosave was restored from the journal. Please review it and save.")
            a.runModal()
        }
    }

    @discardableResult
    func saveSync() -> Bool {
        if current?.isTemp == true { return saveTempAs() }
        let inMemory = emu.sync { e in e.session?.projectDir.isEmpty ?? true } ?? true
        if inMemory { return saveAs() }
        let ok = emu.sync(timeout: 60) { e -> Bool in
            guard let s = e.session else { return false }
            do { try s.save(); e.markStructureDirty(); return true } catch {
                e.reportError(String(localized: "Couldn’t save (your work is still kept in memory)"), error)
                return false
            }
        } ?? false
        if ok { flash(String(localized: "Saved")) }
        return ok
    }

    @discardableResult
    func saveAs() -> Bool {
        if current?.isTemp == true { return saveTempAs() }
        guard status.hasSession else { return false }
        let save = NSSavePanel()
        save.title = String(localized: "Save Project")
        save.nameFieldStringValue = (URL(fileURLWithPath: status.romPath).deletingPathExtension().lastPathComponent) + ".nesrec"
        save.allowedContentTypes = [.nesrec]
        guard save.runModal() == .OK, let dir = save.url else { return false }
        if FileManager.default.fileExists(atPath: dir.path) {
            if (try? FileManager.default.trashItem(at: dir, resultingItemURL: nil)) == nil {
                showError(String(localized: "Couldn’t save"), String(localized: "Couldn’t replace the existing item: \(dir.path)")); return false
            }
        }
        let ok = emu.sync(timeout: 60) { e -> Bool in
            guard let s = e.session else { return false }
            do { try s.saveAs(dir); e.markStructureDirty(); return true } catch {
                e.reportError(String(localized: "Couldn’t save"), error)
                return false
            }
        } ?? false
        if ok {
            if let c = current { current = SessionIdentity(projectPath: dir.path, isTemp: false, romSHA256: c.romSHA256) }
            lastResume = nil // rewritten with the new path on the next timer tick
            flash(String(localized: "Saved"))
        }
        return ok
    }

    /// Clean shutdown: stop the emulation thread (call prepareForQuit first).
    func shutdown() {
        resumeTimer?.invalidate()
        StreamOutputModel.shared.stop()
        emu.shutdown()
        resumeQueue.sync {} // pending resume.json writes
    }

    func closeProject() {
        guard confirmDiscardIfNeeded() else { return }
        captureThumbnail()
        showLibrary = false
        let wasTemp = current?.isTemp == true
        releaseSession()
        if wasTemp { removeTempProject() }
        writeResume(nil)
    }

    /// After "Reset Project" (ProjectReset.swift): the resume record restarts like a new project's
    /// (frame 0, record mode) right away instead of at the next timer tick.
    func sessionWasReset(keepPracticeSlots: Bool) {
        guard let c = current, !c.projectPath.isEmpty else { return }
        let hasSlots = keepPracticeSlots && practiceSlots.contains { $0.hasA }
        writeResume(ResumeRecord(projectPath: c.projectPath, isTemp: c.isTemp, romPath: status.romPath, romSHA256: c.romSHA256,
                                 frame: 0, atTakeEnd: true, mode: .record, hasContent: hasSlots))
    }

    /// Stops the emulation thread from using the installed session and closes it (files released).
    private func releaseSession() {
        flushPlay()
        playSHA = ""
        _ = emu.sync(timeout: 30) { e in e.install(nil) }
        current = nil
    }

    // MARK: session persistence / resume (SessionResume.swift)

    /// Called once at launch, before anything is opened. `root` overrides the folder (tests);
    /// `enabled` false keeps quick play in memory and never resumes.
    func setupSessionPersistence(root: URL?, enabled: Bool) {
        if let root { sessionPaths = SessionPaths(root: root) }
        guard enabled else { return }
        ThumbnailStore.dir = sessionPaths.root.appendingPathComponent("Thumbnails", isDirectory: true)
        do { try sessionPaths.ensure() } catch {
            NSLog("ReplayNES: session folder unavailable (\(error)); sessions are not persisted")
            return
        }
        guard let lock = SessionLock(url: sessionPaths.lockFile) else {
            NSLog("ReplayNES: another instance owns \(sessionPaths.root.path); this one does not resume")
            return
        }
        sessionLock = lock
        persistSessions = true
        // Favourites / history / order next to the session folder (a scratch --session-root keeps its own).
        library.setCatalogFile((root != nil ? sessionPaths.root : sessionPaths.root.deletingLastPathComponent()).appendingPathComponent(RNF_LIBRARY_PREFS_FILE))
        resumeTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.updateResumeRecord()
            self.playFlushTicks += 1
            if self.playFlushTicks >= 60 { self.playFlushTicks = 0; self.flushPlay() }  // play time survives a crash
        }
        NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.persistNow()
        }
    }

    /// Launch: the library shows; the last session (resume record, also after a crash) is offered by
    /// its "Continue" card (unless the launch opens something). Nothing is reopened by itself.
    func loadPendingResume(explicitOpen: Bool) {
        let legacy = openProjectPath
        openProjectPath = ""
        pendingResume = nil
        guard persistSessions else { return }
        switch SessionResume.decide(paths: sessionPaths, explicitOpen: explicitOpen, legacyProjectPath: legacy) {
        case .none:
            return
        case .projectMissing(let r):
            writeResume(nil)
            showError(String(localized: "The last project can’t be found"),
                      String(localized: "“\(URL(fileURLWithPath: r.projectPath).lastPathComponent)” was moved or deleted, so ReplayNES couldn’t resume where you left off.\nOriginal location: \(r.projectPath)\n\nChoose it again from the library, or use “Open Project…”."))
        case .resume(let r):
            pendingResume = r
            libraryNav.focus = LibraryNav.heroFocus
        }
    }

    /// The last session could not be reopened (the reason was already shown). Nothing is deleted:
    /// resume.json and the temporary project stay, so the next launch tries again until the user
    /// starts something else (and decides about the temporary data then).
    private func resumeFailed(_ r: ResumeRecord) {
        let a = NSAlert()
        a.alertStyle = .warning
        a.messageText = String(localized: "Couldn’t resume where you left off")
        a.informativeText = r.isTemp
            ? String(localized: "The unsaved previous session has been kept (ReplayNES will try to resume it again at the next launch).\n\nMove the ROM back to its original location, or choose it from the library. When you start another game, you can choose to save or discard the previous session.\nTemporary location: \(sessionPaths.tempProject.path)")
            : String(localized: "The project “\(URL(fileURLWithPath: r.projectPath).lastPathComponent)” has not been changed. You can open it again with “Continue” in the library or “Open Project…”.")
        a.runModal()
    }

    /// ⌘Q / window close / Sparkle relaunch: no save prompt. Everything is persisted (temporary
    /// project: full save; project: autosave journal) and resume.json is final. Returns false if
    /// persisting failed and the user chose not to quit.
    func prepareForQuit() -> Bool {
        flushPlay()
        guard persistSessions else { return confirmDiscardIfNeeded() }
        guard let c = current else { return true }
        captureThumbnail()
        let isTemp = c.isTemp
        let err: String? = emu.sync(timeout: 60) { e in e.flushForResume(fullSave: isTemp) } ?? String(localized: "Saving didn’t respond")
        updateResumeRecord()
        resumeQueue.sync {}
        guard let err else { return true }
        let a = NSAlert()
        a.alertStyle = .critical
        a.messageText = String(localized: "Your work couldn’t be saved")
        a.informativeText = String(localized: "If you quit now, anything recorded after the last autosave may be lost.") + "\n\n\(err)"
        a.addButton(withTitle: String(localized: "Don’t Quit"))
        a.addButton(withTitle: String(localized: "Quit Anyway"))
        return a.runModal() == .alertSecondButtonReturn
    }

    /// App sent to the background: persist without waiting for the next autosave tick.
    private func persistNow() {
        guard persistSessions, current != nil else { return }
        captureThumbnail()
        emu.perform { e in _ = e.flushForResume(fullSave: false) }
        updateResumeRecord()
    }

    /// Main-thread timer (1 s) and quit: writes resume.json when what would be resumed changed.
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

    /// nil clears the record. Writes are serialized off the main thread.
    private func writeResume(_ r: ResumeRecord?) {
        if r == nil { pendingResume = nil }  // nothing to continue any more
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
            showError(String(localized: "Couldn’t delete the temporary data"), "\(sessionPaths.tempProject.path)\n\(error.localizedDescription)")
        }
        if lastResume?.isTemp == true { writeResume(nil) }
    }

    /// Makes room for a new temporary project. The installed temporary session was already
    /// confirmed (confirmDiscardIfNeeded); one left from an earlier run that was never reopened
    /// (resume failed or skipped) is offered for saving first. Returns false if cancelled.
    private func prepareTempSlot() -> Bool {
        if current?.isTemp == true {
            releaseSession()
            removeTempProject()
            return true
        }
        guard sessionPaths.tempProjectExists else { return true }
        let record = try? ResumeStore.read(sessionPaths.resumeFile)
        let pointsHere = record?.isTemp == true
        if let record, pointsHere, !record.hasContent {
            removeTempProject()
            writeResume(nil)
            return true
        }
        let manifest = (try? Engine.manifestJSON(projectDir: sessionPaths.tempProject)) ?? [:]
        let romName = (manifest["rom"] as? [String: Any])?["name"] as? String ?? "?"
        let a = NSAlert()
        a.messageText = String(localized: "An unsaved previous session remains")
        a.informativeText = String(localized: "Do you want to save the previous session (ROM: \(romName))? If you don’t save, it will be discarded.")
        a.addButton(withTitle: String(localized: "Save…"))
        a.addButton(withTitle: String(localized: "Don’t Save"))
        a.addButton(withTitle: String(localized: "Cancel"))
        switch a.runModal() {
        case .alertFirstButtonReturn:
            let name = URL(fileURLWithPath: romName).deletingPathExtension().lastPathComponent
            guard let dest = askProjectDestination(name: name) else { return false }
            do { try SessionResume.moveTempProject(sessionPaths, to: dest) } catch {
                showError(String(localized: "Couldn’t save"), "\(dest.path)\n\(error.localizedDescription)")
                return false
            }
            library.refresh()
        case .alertSecondButtonReturn:
            removeTempProject()
        default:
            return false
        }
        if pointsHere { writeResume(nil) }
        return true
    }

    /// Save panel for a .nesrec; an existing item at the destination is moved to the Trash.
    private func askProjectDestination(name: String) -> URL? {
        let save = NSSavePanel()
        save.title = String(localized: "Save Project")
        save.nameFieldStringValue = name + ".nesrec"
        save.allowedContentTypes = [.nesrec]
        save.canCreateDirectories = true
        guard save.runModal() == .OK, let dir = save.url else { return nil }
        if sessionPaths.isTempProject(dir.path) { showError(String(localized: "Couldn’t save"), String(localized: "You can’t save to the temporary location.")); return nil }
        if FileManager.default.fileExists(atPath: dir.path) {
            if (try? FileManager.default.trashItem(at: dir, resultingItemURL: nil)) == nil {
                showError(String(localized: "Couldn’t save"), String(localized: "Couldn’t replace the existing item: \(dir.path)")); return nil
            }
        }
        return dir
    }

    /// "Save" of a temporary session = Save As: full save, close, move the temporary project to
    /// the chosen place and reopen it there (same take position). Returns false if cancelled / failed.
    @discardableResult
    func saveTempAs() -> Bool {
        guard current?.isTemp == true else { return false }
        let name = URL(fileURLWithPath: status.romPath).deletingPathExtension().lastPathComponent
        guard let dest = askProjectDestination(name: name) else { return false }
        let practicing = status.practicing
        let failure = emu.sync(timeout: 60) { e -> String? in
            guard let s = e.session else { return String(localized: "No session") }
            do { try s.save() } catch { return (error as? LocalizedError)?.errorDescription ?? "\(error)" }
            e.install(nil)
            return nil
        } ?? String(localized: "Saving didn’t respond")
        if let failure {
            showError(String(localized: "Couldn’t save (your work is still in the temporary location)"), failure)
            return false
        }
        current = nil
        var target = dest
        var moveError: Error?
        do { try SessionResume.moveTempProject(sessionPaths, to: dest) } catch {
            moveError = error
            target = sessionPaths.tempProject
        }
        // The full save above wrote the cursor and take mode: the project reopens where it was.
        do {
            let s = try EngineSession.open(projectDir: target, romOverride: nil, dropCorruptStates: false)
            install(s, recovered: false, resume: ResumeRecord(projectPath: target.path, isTemp: moveError != nil,
                                                              practiceSlot: practicing ? -1 : nil),
                    resumeNotice: false)
        } catch {
            showError(String(localized: "Couldn’t open the project"), "\(target.path)\n\((error as? LocalizedError)?.errorDescription ?? "\(error)")")
        }
        if let moveError {
            showError(String(localized: "Couldn’t save (your work is still in the temporary location)"), "\(dest.path)\n\(moveError.localizedDescription)")
            return false
        }
        library.refresh()
        flash(String(localized: "Saved"))
        return true
    }

    // MARK: menus, library screen, thumbnails (docs/design/UI_REDESIGN.md)

    /// Controller / keyboard navigation (InputManager.onNav): the Quick Menu when open, else the
    /// L+R chord / Esc open it, else the library when it is on screen.
    func handleNav(_ n: NavInput) {
        if quickMenu.isOpen { quickMenu.handle(n); return }
        let libraryOn = !status.hasSession || showLibrary
        switch n {
        case .menu:
            quickMenu.open()
        case .escape:
            if libraryOn && libraryNav.projectsOf != nil { libraryNav.projectsOf = nil }
            else if libraryOn && status.hasSession { hideLibraryScreen() }
            else { quickMenu.open() }
        default:
            if libraryOn { libraryNav.handle(n, model: self) }
        }
    }

    /// Navigation mode, the Metal pill and its glyph follow the session / pause / menu / library.
    func updateUIMode() {
        let libraryOn = !status.hasSession || showLibrary
        input.setNavMode(quickMenu.isOpen || libraryOn)
        let infos = input.controllerMonitor.controllers
        let glyph = MenuPillLayout.glyph(hasController: !infos.isEmpty,
                                         playStation: infos.min(by: { $0.slot < $1.slot })?.family == .playStation)
        if pillGlyph != glyph { pillGlyph = glyph }
        let pill = MenuPillOverlay.shared
        pill.update(glyph: glyph, scale: mainWindow?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2)
        pill.setVisible(status.hasSession && !status.paused && !quickMenu.isOpen && !showLibrary)
    }

    /// Game › Choose Game (⇧⌘L): the library over the paused game.
    func showLibraryScreen() {
        if quickMenu.isOpen { quickMenu.close(resume: false) }
        if status.hasSession { setPaused(true); captureThumbnail() }
        libraryNav.focus = status.hasSession ? -1 : 0
        withAnimation(QMStyle.anim) { showLibrary = true }
        library.refresh()
        updateUIMode()
    }

    func hideLibraryScreen() {
        guard showLibrary else { return }
        libraryNav.projectsOf = nil
        withAnimation(QMStyle.anim) { showLibrary = false }
        updateUIMode()
    }

    /// Bumped when a thumbnail was written (the library redraws).
    func captureThumbnail() {
        guard persistSessions, let c = current, status.hasSession else { return }
        var px: [UInt32]?
        _ = emu.frames.readIfNewer(than: .max) { p, _ in
            px = Array(UnsafeBufferPointer(start: p, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT)))
        }
        guard let px else { return }
        ThumbnailStore.save(pixels: px, project: c.projectPath, romSHA: c.romSHA256, on: resumeQueue) { [weak self] in
            self?.objectWillChange.send()
        }
    }

    // MARK: export

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
            // The renderer is an independent snapshot: encode off the emulation thread.
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

/// The status for views that show frame counters (timecodes, timeline playhead, progress):
/// published on every status update without invalidating everything that observes AppModel.
final class PlaybackClock: ObservableObject {
    @Published var status = EmuStatus()
}

extension EmuStatus {
    /// Everything but the counters that advance every frame (whether the take is empty is kept).
    var coarse: EmuStatus {
        var c = self
        c.frame = 0
        c.takeLength = takeLength > 0 ? 1 : 0
        c.practiceFrame = 0
        return c
    }
}
