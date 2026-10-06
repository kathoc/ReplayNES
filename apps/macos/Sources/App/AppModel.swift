// App state, project lifecycle (new/open/save/recovery) and export jobs. Main thread only.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
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

    // Preferences
    @AppStorage("showLatency") var showLatency = false { didSet { objectWillChange.send() } }
    @AppStorage("pauseAfterRewind") var pauseAfterRewind = true { didSet { pushPrefs() } }
    @AppStorage("autosaveInterval") var autosaveInterval = 5.0 { didSet { pushPrefs() } }
    @AppStorage("integerScale") var integerScale = true { didSet { objectWillChange.send() } }
    @AppStorage("displayPAR87") var displayPAR87 = false { didSet { objectWillChange.send() } }
    @AppStorage("hideOverscan") var hideOverscan = true { didSet { objectWillChange.send() } }
    @AppStorage("volume") var volume = 0.8 { didSet { emu.audio.volume = Float(volume) } }
    @AppStorage("showSidebar") var showSidebar = true { didSet { objectWillChange.send() } }
    /// Photosensitive flash reduction level (FlashLevel raw value). Default 標準 (on): safety first.
    @AppStorage("flashReduction") var flashReduction = FlashLevel.standard.rawValue { didSet { pushPrefs(); objectWillChange.send() } }
    @AppStorage("showFlashIndicator") var showFlashIndicator = true { didSet { objectWillChange.send() } }

    var flashLevel: FlashLevel { FlashLevel(rawValue: flashReduction) ?? .standard }

    /// Set while a project is open; still set at next launch => the app did not quit cleanly.
    @AppStorage("openProjectPath") private var openProjectPath = ""

    var displayOptions: DisplayOptions {
        DisplayOptions(integerScale: integerScale, pixelAspect87: displayPAR87, hideOverscan: hideOverscan)
    }

    weak var mainWindow: NSWindow?
    private var statsTimer: Timer?
    private var noticeWork: DispatchWorkItem?

    private init() {
        emu = EmulationController(input: input)
        inputConfig = input.config
        emu.onStatus = { [weak self] st in
            guard let self else { return }
            if self.status != st { self.status = st }
        }
        emu.onStructure = { [weak self] b, t in self?.bookmarks = b; self?.takes = t }
        emu.onError = { [weak self] title, msg in self?.showError(title, msg) }
        emu.onNotice = { [weak self] text in self?.flash(text) }
        input.onConfigChanged = { [weak self] c in self?.inputConfig = c }
        input.onControllersChanged = { [weak self] c in self?.controllers = c }
        input.onDisconnect = { [weak self] name in
            guard let self else { return }
            self.emu.perform { emu in emu.paused = true }
            self.flash("\(name) が切断されたため一時停止しました")
        }
        input.keyboardEnabled = { [weak self] in
            guard let w = NSApp.keyWindow, w === self?.mainWindow else { return false }
            return !(w.firstResponder is NSText)
        }
    }

    func start() {
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

    func togglePause() { emu.perform { e in e.paused.toggle(); e.advanceRemaining = 0 } }
    func setPaused(_ p: Bool) { emu.perform { e in e.paused = p } }
    func frameAdvance(_ n: Int = 1) { emu.perform { e in e.paused = true; e.advanceRemaining += max(0, n) } }
    func stepBack(_ n: UInt64 = 1) {
        emu.perform { e in
            guard let s = e.session else { return }
            e.seekCommand(s.frame >= n ? s.frame - n : 0)
        }
    }
    func setSlow(_ r: SlowRate) { emu.perform { e in e.slow = r } }
    func setRewindHeld(_ h: Bool) { emu.perform { e in e.uiRewindHeld = h } }
    func setFastForwardHeld(_ h: Bool) { emu.perform { e in e.uiFastForwardHeld = h } }
    func scrub(to f: UInt64) { emu.perform { e in e.paused = true; e.scrubTarget = f } }
    func seek(to f: UInt64) { emu.perform { e in e.seekCommand(f) } }
    func softReset() { emu.perform { e in e.requestEvent(UInt8(RN_EV_SOFT_RESET)) } }
    func powerCycle() { emu.perform { e in e.requestEvent(UInt8(RN_EV_POWER_CYCLE)) } }
    func setRecording(_ r: Bool) { emu.perform { e in e.setRecording(r) } }
    func rerecordHere() { emu.perform { e in e.rerecordHere() } }
    func undoTake() { emu.perform { e in e.undoTake() } }
    func addBookmark() { emu.perform { e in e.addBookmark(name: nil) } }

    func gotoBookmark(_ id: UInt64) {
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

    /// Asks to save unsaved work. Returns false if the user cancelled.
    func confirmDiscardIfNeeded() -> Bool {
        let info = emu.sync(timeout: 10) { e -> (Bool, Bool, UInt64)? in
            guard let s = e.session else { return nil }
            return (s.hasUnsavedChanges, s.projectDir.isEmpty, s.takeLength)
        } ?? nil
        guard let info else { return true }
        let inMemory = info.1
        guard info.0 || (inMemory && info.2 > 0) else { return true }
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
        open.title = "ROMを開いて試す（未保存）"
        open.allowedContentTypes = [.nesROM, .data]
        guard open.runModal() == .OK, let rom = open.url else { return }
        createSession(rom: rom, projectDir: nil)
    }

    func createSession(rom: URL, projectDir: URL?, autoplay: Bool = false) {
        let dirExisted = projectDir.map { FileManager.default.fileExists(atPath: $0.path) } ?? true
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let s = try EngineSession.create(rom: rom, projectDir: projectDir)
                DispatchQueue.main.async {
                    self.install(s, recovered: false)
                    if autoplay { self.setPaused(false) }
                    if projectDir != nil { self.library.refresh() }
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

    func openProject(_ url: URL, romOverride: URL? = nil, dropCorrupt: Bool = false) {
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let s = try EngineSession.open(projectDir: url, romOverride: romOverride, dropCorruptStates: dropCorrupt)
                DispatchQueue.main.async { self.install(s, recovered: s.recovered) }
            } catch let e as RNError {
                DispatchQueue.main.async { self.handleOpenError(e, url: url, romOverride: romOverride, dropCorrupt: dropCorrupt) }
            } catch {
                DispatchQueue.main.async { self.showError("プロジェクトを開けませんでした", "\(error)") }
            }
        }
    }

    private func handleOpenError(_ e: RNError, url: URL, romOverride: URL?, dropCorrupt: Bool) {
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
            guard a.runModal() == .alertFirstButtonReturn else { return }
            let open = NSOpenPanel()
            open.title = "「\(romName)」の場所を指定"
            open.allowedContentTypes = [.nesROM, .data]
            guard open.runModal() == .OK, let newRom = open.url else { return }
            openProject(url, romOverride: newRom, dropCorrupt: dropCorrupt)
        case RN_ERR_CORE_MISMATCH:
            let projCore = manifest["coreCompatID"] as? String ?? "?"
            a.messageText = "別のエミュレーションコアで記録されたプロジェクトです"
            a.informativeText = "再現性を守るため、このバージョンでは開けません（自動変換は行いません）。\nプロジェクトのコア: \(projCore)\nこのアプリのコア: \(Engine.coreCompatID)\n\n記録したときのバージョンの ReplayNES で開いてください。詳細は docs/COMPATIBILITY.md を参照。"
            a.runModal()
        case RN_ERR_CORRUPT:
            a.messageText = "プロジェクトのファイルが破損しています"
            a.informativeText = e.message
            if !dropCorrupt {
                a.informativeText += "\n\nチェックポイント（高速化用のステート）の破損であれば、それらを破棄して開けます。入力履歴（正本）は変更されません。"
                a.addButton(withTitle: "壊れたチェックポイントを破棄して開く")
                a.addButton(withTitle: "キャンセル")
                if a.runModal() == .alertFirstButtonReturn { openProject(url, romOverride: romOverride, dropCorrupt: true) }
            } else {
                a.runModal()
            }
        case RN_ERR_UNSUPPORTED_FORMAT:
            a.messageText = "新しいバージョンの ReplayNES で作られたプロジェクトです"
            a.informativeText = "アプリを更新してください。\n\(e.message)"
            a.runModal()
        default:
            a.messageText = "プロジェクトを開けませんでした"
            a.informativeText = "\(e.statusName): \(e.message)"
            a.runModal()
        }
    }

    private func install(_ s: EngineSession, recovered: Bool) {
        let dir = s.projectDir
        emu.perform { e in e.install(s) }
        openProjectPath = dir
        if !dir.isEmpty { NSDocumentController.shared.noteNewRecentDocumentURL(URL(fileURLWithPath: dir)) }
        if recovered {
            let a = NSAlert()
            a.messageText = "未保存の作業を復元しました"
            a.informativeText = "前回は正常に終了しなかったため、ジャーナルから最後の自動保存までの記録を復元しました。内容を確認して保存してください。"
            a.runModal()
        }
    }

    @discardableResult
    func saveSync() -> Bool {
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
        if ok { openProjectPath = dir.path; flash("保存しました") }
        return ok
    }

    /// Clean shutdown: stop the emulation thread and clear the crash marker.
    func shutdown() {
        StreamOutputModel.shared.stop()
        emu.shutdown()
        openProjectPath = ""
    }

    func closeProject() {
        guard confirmDiscardIfNeeded() else { return }
        emu.perform { e in e.install(nil) }
        openProjectPath = ""
    }

    /// Called at launch: offers to reopen a project that was open when the app last died.
    func checkCrashRecovery() {
        let path = openProjectPath
        guard !path.isEmpty, FileManager.default.fileExists(atPath: path) else { openProjectPath = ""; return }
        let a = NSAlert()
        a.messageText = "前回 ReplayNES は正常に終了しませんでした"
        a.informativeText = "プロジェクト「\(URL(fileURLWithPath: path).lastPathComponent)」を開いて、自動保存された作業を復元しますか？"
        a.addButton(withTitle: "開いて復元")
        a.addButton(withTitle: "開かない")
        if a.runModal() == .alertFirstButtonReturn {
            openProject(URL(fileURLWithPath: path))
        } else {
            openProjectPath = ""
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
