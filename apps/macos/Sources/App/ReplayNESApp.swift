// App entry, menus, launch arguments, document opening, clean shutdown.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = AppModel.shared
    private var pendingOpen: [URL] = []
    private var launched = false
    /// Something was opened by the launch itself (arguments / documents): no resume.
    private var openedAtLaunch = false

    func applicationWillFinishLaunching(_ notification: Notification) {
        // Keep the menu bar compact: no "Show Tab Bar", no dictation / emoji items in 編集.
        NSWindow.allowsAutomaticWindowTabbing = false
        UserDefaults.standard.set(true, forKey: "NSDisabledDictationMenuItem")
        UserDefaults.standard.set(true, forKey: "NSDisabledCharacterPaletteMenuItem")
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        MenuBarCleaner.shared.start()
        model.start()
        launched = true
        // Launch arguments (also used by scripted smoke tests):
        //   --rom <path>        play a ROM without a project (in-memory)
        //   --project <path>    open a .nesrec project
        //   --autoplay          with --rom: start running in record mode immediately
        //   --snapshot <png> [--snapshot-delay s] [--quit-after-snapshot]  capture window + stats
        //   --inject-keys "<sec>:<keyCode>:<d|u>,..."  synthetic key events (TestHooks.swift)
        //   --no-updater        do not start Sparkle (also implied by --snapshot / --inject-keys)
        //   --update-check-now  see Updater.swift
        //   --library-root <dir>  use <dir> instead of ~/Documents/ReplayNES for the ROM library
        //   --library-play <name> start a new library project for that ROM (TestHooks.swift)
        //   --syphon            enable the Syphon streaming output for this run (not saved)
        //   --session-root <dir>  use <dir> instead of ~/Library/Application Support/ReplayNES/Session
        //                       for the temporary project + resume record (scripted runs without it
        //                       keep quick play in memory and never resume)
        //   --inject-pad / --test-actions / --snapshot-at   scripted checks (TestHooks.swift)
        let args = ProcessInfo.processInfo.arguments
        let scripted = ["--snapshot", "--inject-keys", "--inject-pad", "--test-actions", "--snapshot-at"].contains { args.contains($0) }
        if !args.contains("--no-updater") && !scripted {
            UpdaterModel.shared.start(arguments: args)
        }
        func arg(_ name: String) -> String? {
            guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
            return args[i + 1]
        }
        // ROM library: create ~/Documents/ReplayNES/{ROM,Projects}. A failure is reported, never
        // silently replaced by another location.
        StreamOutputModel.shared.start(frames: model.emu.frames, forceEnable: args.contains("--syphon"))
        if let root = arg("--library-root") { model.library.setRoot(URL(fileURLWithPath: root)) }
        let sessionRoot = arg("--session-root")
        model.setupSessionPersistence(root: sessionRoot.map { URL(fileURLWithPath: $0) }, enabled: !scripted || sessionRoot != nil)
        if let err = model.library.start() {
            DispatchQueue.main.async {
                self.model.showError("ライブラリのフォルダを作成できませんでした",
                                     err + "\n\n「書類」フォルダへのアクセスを許可しているか確認してください（システム設定 → プライバシーとセキュリティ → ファイルとフォルダ）。ライブラリ画面の「再試行」でもう一度作成できます。")
            }
        }
        if let name = arg("--library-play") {
            openedAtLaunch = true
            model.scheduleLibraryPlay(name)
        } else if let rom = arg("--rom") {
            openedAtLaunch = true
            model.createSession(rom: URL(fileURLWithPath: rom), projectDir: nil, autoplay: args.contains("--autoplay"))
        } else if let p = arg("--project") {
            openedAtLaunch = true
            model.openProject(URL(fileURLWithPath: p))
        } else if !pendingOpen.isEmpty {
            open(pendingOpen)
        } else {
            // Next turn: a document open event delivered right after launch wins over the resume.
            DispatchQueue.main.async { self.model.resumeLastSession(explicitOpen: self.openedAtLaunch) }
        }
        pendingOpen = []
        if let keys = arg("--inject-keys") { model.scheduleInjectedKeys(keys) }
        if let pad = arg("--inject-pad") { model.scheduleInjectedPad(pad) }
        if let acts = arg("--test-actions") { model.scheduleTestActions(acts) }
        if let snaps = arg("--snapshot-at") { model.scheduleSnapshots(snaps) }
        if let snap = arg("--snapshot") {
            let delay = Double(arg("--snapshot-delay") ?? "3") ?? 3
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                self.model.writeSnapshot(to: URL(fileURLWithPath: snap)) {
                    if args.contains("--quit-after-snapshot") {
                        self.model.shutdown()
                        exit(0)
                    }
                }
            }
        }
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        if !launched { pendingOpen += urls; return }
        open(urls)
    }

    private func open(_ urls: [URL]) {
        guard let url = urls.first else { return }
        openedAtLaunch = true
        guard model.confirmDiscardIfNeeded() else { return }
        if url.pathExtension.lowercased() == "nesrec" {
            model.openProject(url)
        } else {
            model.createSession(rom: url, projectDir: nil)
        }
    }

    /// Also the path Sparkle uses before installing an update (it sends a normal quit event).
    /// No save prompt: the session is persisted (temporary project / autosave journal) and
    /// resumed at the next launch; the emulation thread is stopped before the bundle is replaced.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if !model.prepareForQuit() { return .terminateCancel }
        model.shutdown()
        return .terminateNow
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main
struct ReplayNESApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate

    var body: some Scene {
        Window("ReplayNES", id: "main") {
            ContentView().environmentObject(AppModel.shared)
        }
        .defaultSize(width: 1100, height: 820)
        .commands {
            AppCommands(model: AppModel.shared, menu: AppModel.shared.menu, stream: StreamOutputModel.shared)
        }

        Window("ライブラリ", id: "library") {
            LibraryWindow().environmentObject(AppModel.shared)
        }
        .defaultSize(width: 860, height: 560)
        .commandsRemoved()

        Window("テイク一覧", id: "takes") {
            TakesPanel().environmentObject(AppModel.shared)
        }
        .defaultSize(width: 720, height: 420)
        .commandsRemoved()

        Window("操作ガイド", id: "guide") {
            GuideView()
        }
        .defaultSize(width: 620, height: 640)
        .commandsRemoved()

        Settings {
            SettingsView().environmentObject(AppModel.shared)
        }
    }
}

/// Menu bar: ReplayNES / ファイル / 編集 (text fields only) / 再生 / 表示 / ウインドウ / ヘルプ.
/// Default items that do nothing useful here are removed (see also AppDelegate.applicationWillFinishLaunching).
struct AppCommands: Commands {
    // Not observed: AppModel publishes every frame, which would rebuild (and flicker) the menu bar.
    let model: AppModel
    @ObservedObject var menu: MenuState
    let stream: StreamOutputModel
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        // ReplayNES: About, Check for Updates…, Settings…, Quit.
        CommandGroup(after: .appInfo) {
            CheckForUpdatesCommand(updates: UpdaterModel.shared)
        }
        CommandGroup(replacing: .systemServices) {}
        CommandGroup(replacing: .appVisibility) {}

        // ファイル
        CommandGroup(replacing: .newItem) {
            // ⌘L is the latency overlay; ⇧⌘L is free.
            Button("ライブラリ…") { openWindow(id: "library") }.keyboardShortcut("l", modifiers: [.command, .shift])
            Divider()
            Button("新規プロジェクト…") { model.newProject() }.keyboardShortcut("n")
            Button("プロジェクトを開く…") { model.openProjectPanel() }.keyboardShortcut("o")
            Button("ROMを開いて試す（プロジェクトなし）…") { model.quickPlay() }.keyboardShortcut("n", modifiers: [.command, .shift])
        }
        CommandGroup(replacing: .saveItem) {
            Button("保存") { model.saveSync() }.keyboardShortcut("s").disabled(!menu.v.hasSession)
            Button("別名で保存…") { model.saveAs() }.keyboardShortcut("s", modifiers: [.command, .shift]).disabled(!menu.v.hasSession)
            Divider()
            Button("MP4に書き出す…") { model.showExport = true }.keyboardShortcut("e")
                .disabled(!menu.v.hasSession || menu.v.takeEmpty)
            Divider()
            Button("プロジェクトを閉じる") { model.closeProject() }.disabled(!menu.v.hasSession)
        }
        CommandGroup(replacing: .printItem) {}
        CommandGroup(replacing: .importExport) {}

        // 編集: only what text fields (names) need.
        CommandGroup(replacing: .undoRedo) {}
        CommandGroup(replacing: .textEditing) {}
        CommandGroup(replacing: .textFormatting) {}

        // 再生
        CommandMenu("再生") {
            let st = menu.v
            let has = st.hasSession
            Button(st.paused ? "再開" : "一時停止") { model.togglePause() }.keyboardShortcut("p").disabled(!has)
            Button("コマ送り") { model.frameAdvance() }.keyboardShortcut(.rightArrow, modifiers: [.command]).disabled(!has)
            Button("1コマ戻る") { model.stepBack() }.keyboardShortcut(.leftArrow, modifiers: [.command]).disabled(!has)
            Button("1秒戻る") { model.jump(seconds: -1) }.keyboardShortcut("[", modifiers: [.command]).disabled(!has)
            Button("1秒進む（録画済みの範囲）") { model.jump(seconds: 1) }.keyboardShortcut("]", modifiers: [.command])
                .disabled(!has || st.practicing)
            Button(st.practicing ? "Aへ戻る" : "先頭へ") { model.seek(to: 0) }.keyboardShortcut(.upArrow, modifiers: [.command]).disabled(!has)
            Toggle("スロー（1/2）", isOn: Binding(get: { st.slowOn }, set: { model.setSlow($0 ? .half : .normal) }))
                .keyboardShortcut("2").disabled(!has)
            Divider()
            Toggle("録画モード", isOn: Binding(get: { st.recording && !st.practicing }, set: { _ in model.toggleRecord() }))
                .keyboardShortcut("m", modifiers: [.command, .shift]).disabled(!has || st.practicing)
            Button(st.practicing ? "練習をやめる" : (st.showPracticePanel ? "練習パネルを隠す" : "練習モード（A/B リピート）…")) {
                model.togglePracticePanel()
            }
            .keyboardShortcut("p", modifiers: [.command, .shift]).disabled(!has)
            Button("選択中の区間のAをここに（タイムライン）") { model.timelineMarkA() }
                .keyboardShortcut("i", modifiers: [.command, .option]).disabled(!has || st.practicing)
            Button("選択中の区間のBをここに（タイムライン）") { model.timelineMarkB() }
                .keyboardShortcut("o", modifiers: [.command, .option]).disabled(!has || st.practicing)
            Divider()
            Button("ブックマークを追加") { model.addBookmark() }.keyboardShortcut("d").disabled(!has || st.practicing)
            Button("前の試行へ戻す") { model.undoTake() }.keyboardShortcut("z", modifiers: [.command, .option])
                .disabled(!has || !st.undoAvailable || st.practicing)
            Menu("リセット・電源") {
                Button("ソフトリセット") { model.softReset() }.keyboardShortcut("r")
                Button("電源再投入") { model.powerCycle() }.keyboardShortcut("r", modifiers: [.command, .shift])
            }
            .disabled(!has || (!st.recording && !st.practicing))
        }

        // 表示
        CommandGroup(replacing: .toolbar) {
            Button("フルスクリーン") { (NSApp.keyWindow ?? model.mainWindow)?.toggleFullScreen(nil) }
                .keyboardShortcut("f", modifiers: [.command, .control])
            Divider()
        }
        CommandGroup(before: .sidebar) {
            Picker("表示サイズ", selection: Binding(get: { menu.v.integerScale }, set: { model.integerScale = $0 })) {
                Text("等倍（くっきり整数倍）").tag(true)
                Text("FILL（ウインドウいっぱい）").tag(false)
            }
            Button("等倍 / FILL を切り替え") { model.integerScale.toggle() }.keyboardShortcut("f")
            Divider()
            Toggle("サイドバー", isOn: Binding(get: { menu.v.showSidebar }, set: { model.showSidebar = $0 })).keyboardShortcut("s", modifiers: [.command, .option])
            Button("テイク一覧") { openWindow(id: "takes") }.keyboardShortcut("t", modifiers: [.command, .shift])
            Divider()
            Picker("フラッシュ低減", selection: Binding(get: { menu.v.flashReduction }, set: { model.flashReduction = $0 })) {
                ForEach(FlashLevel.allCases) { Text($0.label).tag($0.rawValue) }
            }
            Toggle("配信出力 (Syphon)", isOn: Binding(get: { menu.v.streamOn }, set: { stream.setOn($0) }))
            Toggle("レイテンシ表示", isOn: Binding(get: { menu.v.showLatency }, set: { model.showLatency = $0 })).keyboardShortcut("l")
            Divider()
        }

        // ヘルプ
        CommandGroup(replacing: .help) {
            Button("操作ガイド") { openWindow(id: "guide") }.keyboardShortcut("?", modifiers: [.command])
        }
    }
}
