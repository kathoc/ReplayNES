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

    @Published var status = EmuStatus()
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
    @AppStorage("pauseAfterRewind") var pauseAfterRewind = true { didSet { pushPrefs() } }
    @AppStorage("autosaveInterval") var autosaveInterval = 2.0 { didSet { pushPrefs() } }
    /// 等倍 (largest integer scale that fits, pixel-perfect) vs FILL (fill the window, aspect kept).
    @AppStorage("integerScale") var integerScale = true { didSet { objectWillChange.send() } }
    @AppStorage("displayPAR87") var displayPAR87 = false { didSet { objectWillChange.send() } }
    @AppStorage("hideOverscan") var hideOverscan = true { didSet { objectWillChange.send() } }
    @AppStorage("volume") var volume = 0.8 { didSet { emu.audio.volume = Float(volume) } }
    /// Hidden by default since 0.2.0 (new key so earlier "shown" settings do not carry over).
    @AppStorage("sidebarVisible") var showSidebar = false { didSet { objectWillChange.send() } }
    /// While paused, controller D-pad ←/→ step one frame back/forward (not sent to the game).
    @AppStorage("dpadStepWhenPaused") var dpadStepWhenPaused = true { didSet { input.setPausedStepEnabled(dpadStepWhenPaused) } }
    /// Photosensitive flash reduction level (FlashLevel raw value). Default 標準 (on): safety first.
    @AppStorage("flashReduction") var flashReduction = FlashLevel.standard.rawValue { didSet { pushPrefs(); objectWillChange.send() } }
    @AppStorage("showFlashIndicator") var showFlashIndicator = true { didSet { objectWillChange.send() } }

    var flashLevel: FlashLevel { FlashLevel(rawValue: flashReduction) ?? .standard }

    /// Crash marker of versions before resume.json (read once at launch, see resumeLastSession).
    @AppStorage("openProjectPath") private var openProjectPath = ""

    // Session persistence (SessionResume.swift): every session is on disk and resumed at launch.
    private(set) var sessionPaths = SessionPaths.standard
    private var sessionLock: SessionLock?
    /// false: second instance, scripted run without --session-root, or the folder is unusable.
    /// Then quick play stays in memory and nothing is resumed (the behaviour before 0.3).
    private(set) var persistSessions = false
    private struct SessionIdentity { let projectPath: String; let isTemp: Bool; let romSHA256: String }
    /// The installed session (main-thread view of what the emulation thread owns).
    private var current: SessionIdentity?
    private var lastResume: ResumeRecord?
    private var resumeTimer: Timer?
    private let resumeQueue = DispatchQueue(label: "replaynes.resume", qos: .utility)

    /// The temporary project of a session without a project (window title, menus).
    func isTempSession(_ projectPath: String) -> Bool { persistSessions && sessionPaths.isTempProject(projectPath) }

    var displayOptions: DisplayOptions {
        DisplayOptions(integerScale: integerScale, pixelAspect87: displayPAR87, hideOverscan: hideOverscan)
    }

    weak var mainWindow: NSWindow?
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
            if self.status != st { self.status = st }
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
        input.onControllersChanged = { [weak self] c in self?.controllers = c }
        input.onDisconnect = { [weak self] name in
            guard let self else { return }
            self.emu.perform { emu in emu.paused = true }
            self.flash("\(name) が切断されたため一時停止しました")
        }
        input.onPausedStep = { [weak emu] dir, down in emu?.perform { e in e.pausedStep(dir, down: down) } }
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
                            undoAvailable: st.undoDepth > 0, showPracticePanel: showPracticePanel,
                            integerScale: integerScale, showSidebar: showSidebar, flashReduction: flashReduction,
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
    /// Practice button / menu: shows or hides the OSD; while practicing it leaves practice.
    func togglePracticePanel() {
        if status.practicing { practiceStop(); return }
        showPracticePanel.toggle()
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
            } catch { e.reportError("ブックマークへ移動できませんでした", error) }
        }
    }
    func removeBookmark(_ id: UInt64) {
        emu.perform { e in
            do { try e.session?.removeBookmark(id); e.markStructureDirty() } catch { e.reportError("ブックマークを削除できませんでした", error) }
        }
    }
    func renameBookmark(_ id: UInt64, _ name: String) {
        emu.perform { e in
            do { try e.session?.renameBookmark(id, name: name); e.markStructureDirty() } catch { e.reportError("名前を変更できませんでした", error) }
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
                e.notice("テイク #\(id) に切り替えました")
            } catch { e.reportError("テイクを切り替えられませんでした", error) }
        }
    }

    // MARK: project lifecycle

    /// Before another ROM / project replaces the current session (or it is closed): asks to save
    /// unsaved work. Returns false if the user cancelled. A temporary session with recorded content
    /// asks 保存… (= Save As) / 保存しない (its temporary project is deleted when replaced).
    func confirmDiscardIfNeeded() -> Bool {
        let info = emu.sync(timeout: 10) { e -> (unsaved: Bool, dir: String, takeLength: UInt64, content: Bool)? in
            guard let s = e.session else { return nil }
            return (s.hasUnsavedChanges, s.projectDir, s.takeLength, SessionResume.hasRecordedContent(s))
        } ?? nil
        guard let info else { return true }
        if current?.isTemp == true {
            guard info.content else { return true }
            let a = NSAlert()
            a.messageText = "保存しますか？"
            a.informativeText = "このセッションはまだプロジェクトとして保存されていません（一時保存中）。保存しない場合、一時保存されたデータは破棄されます。"
            a.addButton(withTitle: "保存…")
            a.addButton(withTitle: "保存しない")
            a.addButton(withTitle: "キャンセル")
            switch a.runModal() {
            case .alertFirstButtonReturn: return saveTempAs()
            case .alertSecondButtonReturn: return true
            default: return false
            }
        }
        let inMemory = info.dir.isEmpty
        guard info.unsaved || (inMemory && info.takeLength > 0) else { return true }
        let a = NSAlert()
        a.messageText = "現在のプロジェクトに保存されていない変更があります"
        a.informativeText = "保存しますか？"
        a.addButton(withTitle: "保存")
        a.addButton(withTitle: "保存しない")
        a.addButton(withTitle: "キャンセル")
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
        open.title = "ROMを選択"
        open.message = "新しいプロジェクトで使うNES ROM (.nes) を選んでください。ROMはプロジェクトにコピーされません。"
        open.allowedContentTypes = [.nesROM, .data]
        open.allowsMultipleSelection = false
        guard open.runModal() == .OK, let rom = open.url else { return }
        let save = NSSavePanel()
        save.title = "プロジェクトの保存先"
        save.nameFieldStringValue = rom.deletingPathExtension().lastPathComponent + ".nesrec"
        save.allowedContentTypes = [.nesrec]
        save.canCreateDirectories = true
        guard save.runModal() == .OK, let dir = save.url else { return }
        if FileManager.default.fileExists(atPath: dir.path) {
            do { try FileManager.default.trashItem(at: dir, resultingItemURL: nil) } catch {
                showError("既存のプロジェクトを置き換えられません", error.localizedDescription); return
            }
        }
        createSession(rom: rom, projectDir: dir)
    }

    /// Plays a ROM without a project (in-memory; "別名で保存" writes it later).
    func quickPlay() {
        guard confirmDiscardIfNeeded() else { return }
        let open = NSOpenPanel()
        open.title = "ROMを開いて試す（プロジェクトなし・一時保存）"
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
                    let hint = e.status == RN_ERR_ROM_INVALID ? "\nこのROMは読み込めません（未対応のマッパーまたは不正なファイル）。" : ""
                    self.showError("プロジェクトを作成できませんでした", e.message + hint)
                }
            } catch {
                DispatchQueue.main.async { self.showError("プロジェクトを作成できませんでした", "\(error)") }
            }
        }
    }

    // MARK: library

    /// Starts a new project for a library ROM right away, auto-saved as
    /// Projects/<ROM名> <yyyy-MM-dd HHmm>.nesrec (no save panel). Returns false if cancelled.
    @discardableResult
    func playFromLibrary(_ rom: LibraryROM) -> Bool {
        guard confirmDiscardIfNeeded() else { return false }
        guard library.ensureFolders() else {
            showError("ライブラリのフォルダを用意できません", library.folderError ?? "")
            return false
        }
        let dir = LibraryScanner.newProjectURL(projectsDir: library.paths.projects, romName: rom.name, date: Date())
        createSession(rom: rom.url, projectDir: dir)
        return true
    }

    /// 「続きから」: opens a library project (the usual save prompt first).
    @discardableResult
    func continueProject(_ url: URL) -> Bool {
        guard confirmDiscardIfNeeded() else { return false }
        openProject(url)
        return true
    }

    func openProjectPanel() {
        guard confirmDiscardIfNeeded() else { return }
        let open = NSOpenPanel()
        open.title = "プロジェクトを開く"
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
                    self.showError("プロジェクトを開けませんでした", "\(error)")
                    if let resume { self.resumeFailed(resume) }
                }
            }
        }
    }

    private func reportDroppedPracticeSlots(_ mask: UInt32) {
        let names = (0..<EngineSession.practiceSlotCount).filter { mask & (1 << UInt32($0)) != 0 }.map { "区間 \($0 + 1)" }
        showError("壊れていた練習区間を破棄しました",
                  "次の練習区間（A/B）は読み込めなかったため削除されました: \(names.joined(separator: "、"))\n\nテイク（録画）は影響を受けていません。必要ならAとBを設定し直してください。")
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
                a.messageText = "ROMが見つかりません"
                a.informativeText = "このプロジェクトのROM「\(romName)」が元の場所にありません。\n元の場所: \(romPath)\n\n同じROM（SHA-256 が一致するファイル）を指定してください。\nSHA-256: \(romSHA)"
            } else {
                a.messageText = "ROMが一致しません"
                a.informativeText = "指定したROMはこのプロジェクトで使われたROMと内容が異なります。\n必要なROM: \(romName)\nSHA-256: \(romSHA)\n\n詳細: \(e.message)"
            }
            a.addButton(withTitle: "ROMを指定…")
            a.addButton(withTitle: "キャンセル")
            guard a.runModal() == .alertFirstButtonReturn else { return false }
            let open = NSOpenPanel()
            open.title = "「\(romName)」の場所を指定"
            open.allowedContentTypes = [.nesROM, .data]
            guard open.runModal() == .OK, let newRom = open.url else { return false }
            openProject(url, romOverride: newRom, dropCorrupt: dropCorrupt, dropCorruptPractice: dropCorruptPractice, resume: resume)
            return true
        case RN_ERR_CORE_MISMATCH:
            let projCore = manifest["coreCompatID"] as? String ?? "?"
            a.messageText = "別のエミュレーションコアで記録されたプロジェクトです"
            a.informativeText = "再現性を守るため、このバージョンでは開けません（自動変換は行いません）。\nプロジェクトのコア: \(projCore)\nこのアプリのコア: \(Engine.coreCompatID)\n\n記録したときのバージョンの ReplayNES で開いてください。詳細は docs/COMPATIBILITY.md を参照。"
            a.runModal()
            return false
        case RN_ERR_CORRUPT where Self.isPracticeCorruption(e.message) && !dropCorruptPractice:
            a.messageText = "練習区間（A/B）のデータが破損しています"
            a.informativeText = e.message + "\n\n壊れた練習区間だけを破棄して開けます（その区間のA/Bは失われます）。テイク（録画）は変更されません。"
            a.addButton(withTitle: "壊れた練習区間を破棄して開く")
            a.addButton(withTitle: "キャンセル")
            guard a.runModal() == .alertFirstButtonReturn else { return false }
            openProject(url, romOverride: romOverride, dropCorrupt: dropCorrupt, dropCorruptPractice: true, resume: resume)
            return true
        case RN_ERR_CORRUPT:
            a.messageText = "プロジェクトのファイルが破損しています"
            a.informativeText = e.message
            if !dropCorrupt {
                a.informativeText += "\n\nチェックポイント（高速化用のステート）の破損であれば、それらを破棄して開けます。入力履歴（正本）は変更されません。"
                a.addButton(withTitle: "壊れたチェックポイントを破棄して開く")
                a.addButton(withTitle: "キャンセル")
                guard a.runModal() == .alertFirstButtonReturn else { return false }
                openProject(url, romOverride: romOverride, dropCorrupt: true, dropCorruptPractice: dropCorruptPractice, resume: resume)
                return true
            }
            a.runModal()
            return false
        case RN_ERR_UNSUPPORTED_FORMAT:
            a.messageText = "新しいバージョンの ReplayNES で作られたプロジェクトです"
            a.informativeText = "アプリを更新してください。\n\(e.message)"
            a.runModal()
            return false
        default:
            a.messageText = "プロジェクトを開けませんでした"
            a.informativeText = "\(e.statusName): \(e.message)"
            a.runModal()
            return false
        }
    }

    /// `resume`: restore that take mode / position (paused) and the practice panel; `resumeNotice`
    /// says so (false when Save As reopens the moved temporary project).
    private func install(_ s: EngineSession, recovered: Bool, resume: ResumeRecord? = nil, resumeNotice: Bool = true) {
        let dir = s.projectDir
        let isTemp = persistSessions && sessionPaths.isTempProject(dir)
        // A temporary session being replaced was confirmed (saved elsewhere, or 保存しない / empty).
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
        showPracticePanel = resume?.practiceSlot != nil
        emu.perform { e in
            e.tempSession = isTemp
            e.install(s)
            if let resume { e.applyResume(resume) }
        }
        writeResume(dir.isEmpty ? nil : record)
        if !dir.isEmpty && !isTemp { NSDocumentController.shared.noteNewRecentDocumentURL(URL(fileURLWithPath: dir)) }
        if resume != nil {
            if resumeNotice { flash("前回の続きから再開しました") }
        } else if recovered {
            let a = NSAlert()
            a.messageText = "未保存の作業を復元しました"
            a.informativeText = "前回は正常に終了しなかったため、ジャーナルから最後の自動保存までの記録を復元しました。内容を確認して保存してください。"
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
                e.reportError("保存できませんでした（作業内容はメモリ上に保持されています）", error)
                return false
            }
        } ?? false
        if ok { flash("保存しました") }
        return ok
    }

    @discardableResult
    func saveAs() -> Bool {
        if current?.isTemp == true { return saveTempAs() }
        guard status.hasSession else { return false }
        let save = NSSavePanel()
        save.title = "プロジェクトを保存"
        save.nameFieldStringValue = (URL(fileURLWithPath: status.romPath).deletingPathExtension().lastPathComponent) + ".nesrec"
        save.allowedContentTypes = [.nesrec]
        guard save.runModal() == .OK, let dir = save.url else { return false }
        if FileManager.default.fileExists(atPath: dir.path) {
            if (try? FileManager.default.trashItem(at: dir, resultingItemURL: nil)) == nil {
                showError("保存できませんでした", "既存の項目を置き換えられません: \(dir.path)"); return false
            }
        }
        let ok = emu.sync(timeout: 60) { e -> Bool in
            guard let s = e.session else { return false }
            do { try s.saveAs(dir); e.markStructureDirty(); return true } catch {
                e.reportError("保存できませんでした", error)
                return false
            }
        } ?? false
        if ok {
            if let c = current { current = SessionIdentity(projectPath: dir.path, isTemp: false, romSHA256: c.romSHA256) }
            lastResume = nil // rewritten with the new path on the next timer tick
            flash("保存しました")
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
        let wasTemp = current?.isTemp == true
        releaseSession()
        if wasTemp { removeTempProject() }
        writeResume(nil)
    }

    /// Stops the emulation thread from using the installed session and closes it (files released).
    private func releaseSession() {
        _ = emu.sync(timeout: 30) { e in e.install(nil) }
        current = nil
    }

    // MARK: session persistence / resume (SessionResume.swift)

    /// Called once at launch, before anything is opened. `root` overrides the folder (tests);
    /// `enabled` false keeps quick play in memory and never resumes.
    func setupSessionPersistence(root: URL?, enabled: Bool) {
        if let root { sessionPaths = SessionPaths(root: root) }
        guard enabled else { return }
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
        resumeTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.updateResumeRecord() }
        NotificationCenter.default.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.persistNow()
        }
    }

    /// Launch: reopens the last session where it was, paused (unless the launch opens something).
    func resumeLastSession(explicitOpen: Bool) {
        let legacy = openProjectPath
        openProjectPath = ""
        guard persistSessions else { return }
        switch SessionResume.decide(paths: sessionPaths, explicitOpen: explicitOpen, legacyProjectPath: legacy) {
        case .none:
            return
        case .projectMissing(let r):
            writeResume(nil)
            showError("前回のプロジェクトが見つかりません",
                      "「\(URL(fileURLWithPath: r.projectPath).lastPathComponent)」は移動または削除されたため、前回の続きから再開できませんでした。\n元の場所: \(r.projectPath)\n\nライブラリから選び直すか、「プロジェクトを開く…」で開いてください。")
        case .resume(let r):
            openProject(URL(fileURLWithPath: r.projectPath), resume: r)
        }
    }

    /// The last session could not be reopened (the reason was already shown). Nothing is deleted:
    /// resume.json and the temporary project stay, so the next launch tries again until the user
    /// starts something else (and decides about the temporary data then).
    private func resumeFailed(_ r: ResumeRecord) {
        let a = NSAlert()
        a.alertStyle = .warning
        a.messageText = "前回の続きから再開できませんでした"
        a.informativeText = r.isTemp
            ? "保存されていない前回のセッションは削除せずに残してあります（次回の起動時にもう一度再開を試みます）。\n\nROMを元の場所に戻すか、ライブラリから選んでください。別のゲームを始めるときに、前回のセッションを保存するか破棄するかを選べます。\n一時保存の場所: \(sessionPaths.tempProject.path)"
            : "プロジェクト「\(URL(fileURLWithPath: r.projectPath).lastPathComponent)」は変更されていません。ライブラリの「続きから」や「プロジェクトを開く…」からもう一度開けます。"
        a.runModal()
    }

    /// ⌘Q / window close / Sparkle relaunch: no save prompt. Everything is persisted (temporary
    /// project: full save; project: autosave journal) and resume.json is final. Returns false if
    /// persisting failed and the user chose not to quit.
    func prepareForQuit() -> Bool {
        guard persistSessions else { return confirmDiscardIfNeeded() }
        guard let c = current else { return true }
        let isTemp = c.isTemp
        let err: String? = emu.sync(timeout: 60) { e in e.flushForResume(fullSave: isTemp) } ?? "保存処理が応答しませんでした"
        updateResumeRecord()
        resumeQueue.sync {}
        guard let err else { return true }
        let a = NSAlert()
        a.alertStyle = .critical
        a.messageText = "作業内容を保存できませんでした"
        a.informativeText = "このまま終了すると、最後の自動保存より後の記録が失われる可能性があります。\n\n\(err)"
        a.addButton(withTitle: "終了しない")
        a.addButton(withTitle: "終了する")
        return a.runModal() == .alertSecondButtonReturn
    }

    /// App sent to the background: persist without waiting for the next autosave tick.
    private func persistNow() {
        guard persistSessions, current != nil else { return }
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
            showError("一時保存を削除できませんでした", "\(sessionPaths.tempProject.path)\n\(error.localizedDescription)")
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
        a.messageText = "保存されていない前回のセッションが残っています"
        a.informativeText = "前回のセッション（ROM: \(romName)）を保存しますか？保存しない場合は破棄されます。"
        a.addButton(withTitle: "保存…")
        a.addButton(withTitle: "保存しない")
        a.addButton(withTitle: "キャンセル")
        switch a.runModal() {
        case .alertFirstButtonReturn:
            let name = URL(fileURLWithPath: romName).deletingPathExtension().lastPathComponent
            guard let dest = askProjectDestination(name: name) else { return false }
            do { try SessionResume.moveTempProject(sessionPaths, to: dest) } catch {
                showError("保存できませんでした", "\(dest.path)\n\(error.localizedDescription)")
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
        save.title = "プロジェクトを保存"
        save.nameFieldStringValue = name + ".nesrec"
        save.allowedContentTypes = [.nesrec]
        save.canCreateDirectories = true
        guard save.runModal() == .OK, let dir = save.url else { return nil }
        if sessionPaths.isTempProject(dir.path) { showError("保存できませんでした", "一時保存の場所には保存できません。"); return nil }
        if FileManager.default.fileExists(atPath: dir.path) {
            if (try? FileManager.default.trashItem(at: dir, resultingItemURL: nil)) == nil {
                showError("保存できませんでした", "既存の項目を置き換えられません: \(dir.path)"); return nil
            }
        }
        return dir
    }

    /// 「保存」 of a temporary session = Save As: full save, close, move the temporary project to
    /// the chosen place and reopen it there (same take position). Returns false if cancelled / failed.
    @discardableResult
    func saveTempAs() -> Bool {
        guard current?.isTemp == true else { return false }
        let name = URL(fileURLWithPath: status.romPath).deletingPathExtension().lastPathComponent
        guard let dest = askProjectDestination(name: name) else { return false }
        let practicing = status.practicing
        let failure = emu.sync(timeout: 60) { e -> String? in
            guard let s = e.session else { return "セッションがありません" }
            do { try s.save() } catch { return (error as? LocalizedError)?.errorDescription ?? "\(error)" }
            e.install(nil)
            return nil
        } ?? "保存処理が応答しませんでした"
        if let failure {
            showError("保存できませんでした（作業内容は一時保存に残っています）", failure)
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
            showError("プロジェクトを開けませんでした", "\(target.path)\n\((error as? LocalizedError)?.errorDescription ?? "\(error)")")
        }
        if let moveError {
            showError("保存できませんでした（作業内容は一時保存に残っています）", "\(dest.path)\n\(moveError.localizedDescription)")
            return false
        }
        library.refresh()
        flash("保存しました")
        return true
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
