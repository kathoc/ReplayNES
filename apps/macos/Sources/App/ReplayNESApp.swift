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
        // Keep the menu bar compact: no "Show Tab Bar", no dictation / emoji items in Edit.
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
        //   --crt               enable the CRT display for this run (not saved)
        //   --inject-pad / --test-actions / --snapshot-at / --snapshot-windows   scripted checks (TestHooks.swift)
        let args = ProcessInfo.processInfo.arguments
        let scripted = ["--snapshot", "--inject-keys", "--inject-pad", "--test-actions", "--snapshot-at", "--snapshot-windows"].contains { args.contains($0) }
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
        if args.contains("--crt") { CRTSettingsModel.shared.launchOverride = true }
        if let root = arg("--library-root") { model.library.setRoot(URL(fileURLWithPath: root)) }
        let sessionRoot = arg("--session-root")
        model.setupSessionPersistence(root: sessionRoot.map { URL(fileURLWithPath: $0) }, enabled: !scripted || sessionRoot != nil)
        if let err = model.library.start() {
            DispatchQueue.main.async {
                self.model.showError(String(localized: "Couldn’t create the library folder"),
                                     err + "\n\n" + String(localized: "Make sure ReplayNES is allowed to access the Documents folder (System Settings → Privacy & Security → Files and Folders). You can try again with “Retry” in the library window."))
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
        if let snaps = arg("--snapshot-windows") { model.scheduleWindowSnapshots(snaps) }
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

    init() {
        UILanguage.apply()
    }

    var body: some Scene {
        Window("ReplayNES", id: "main") {
            ContentView().environmentObject(AppModel.shared)
        }
        .defaultSize(width: 1100, height: 820)
        .commands {
            AppCommands(model: AppModel.shared, menu: AppModel.shared.menu, stream: StreamOutputModel.shared)
        }

        Window("Library", id: "library") {
            LibraryWindow().environmentObject(AppModel.shared)
        }
        .defaultSize(width: 860, height: 560)
        .commandsRemoved()

        Window("Takes", id: "takes") {
            TakesPanel().environmentObject(AppModel.shared)
        }
        .defaultSize(width: 720, height: 420)
        .commandsRemoved()

        Window("Controls Guide", id: "guide") {
            GuideView()
        }
        .defaultSize(width: 620, height: 640)
        .commandsRemoved()

        Settings {
            SettingsView().environmentObject(AppModel.shared)
        }
    }
}

/// Menu bar: ReplayNES / File / Edit (text fields only) / Playback / View / Window / Help.
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

        // File
        CommandGroup(replacing: .newItem) {
            // ⌘L is the latency overlay; ⇧⌘L is free.
            Button("Library…") { openWindow(id: "library") }.keyboardShortcut("l", modifiers: [.command, .shift])
            Divider()
            Button("New Project…") { model.newProject() }.keyboardShortcut("n")
            Button("Open Project…") { model.openProjectPanel() }.keyboardShortcut("o")
            Button("Try a ROM (No Project)…") { model.quickPlay() }.keyboardShortcut("n", modifiers: [.command, .shift])
        }
        CommandGroup(replacing: .saveItem) {
            Button("Save") { model.saveSync() }.keyboardShortcut("s").disabled(!menu.v.hasSession)
            Button("Save As…") { model.saveAs() }.keyboardShortcut("s", modifiers: [.command, .shift]).disabled(!menu.v.hasSession)
            Divider()
            Button("Export MP4…") { model.showExport = true }.keyboardShortcut("e")
                .disabled(!menu.v.hasSession || menu.v.takeEmpty)
            Divider()
            Button("Close Project") { model.closeProject() }.disabled(!menu.v.hasSession)
        }
        CommandGroup(replacing: .printItem) {}
        CommandGroup(replacing: .importExport) {}

        // Edit: only what text fields (names) need.
        CommandGroup(replacing: .undoRedo) {}
        CommandGroup(replacing: .textEditing) {}
        CommandGroup(replacing: .textFormatting) {}

        // Playback
        CommandMenu("Playback") {
            let st = menu.v
            let has = st.hasSession
            Button(st.paused ? String(localized: "Resume") : String(localized: "Pause")) { model.togglePause() }.keyboardShortcut("p").disabled(!has)
            Button("Frame Advance") { model.frameAdvance() }.keyboardShortcut(.rightArrow, modifiers: [.command]).disabled(!has)
            Button("Step Back One Frame") { model.stepBack() }.keyboardShortcut(.leftArrow, modifiers: [.command]).disabled(!has)
            Button("Back 1 Second") { model.jump(seconds: -1) }.keyboardShortcut("[", modifiers: [.command]).disabled(!has)
            Button("Forward 1 Second (Recorded Range)") { model.jump(seconds: 1) }.keyboardShortcut("]", modifiers: [.command])
                .disabled(!has || st.practicing)
            Button(st.practicing ? String(localized: "Back to A") : String(localized: "Go to Start")) { model.seek(to: 0) }.keyboardShortcut(.upArrow, modifiers: [.command]).disabled(!has)
            Toggle("Slow Motion (1/2)", isOn: Binding(get: { st.slowOn }, set: { model.setSlow($0 ? .half : .normal) }))
                .keyboardShortcut("2").disabled(!has)
            Divider()
            Toggle("Record Mode", isOn: Binding(get: { st.recording && !st.practicing }, set: { _ in model.toggleRecord() }))
                .keyboardShortcut("m", modifiers: [.command, .shift]).disabled(!has || st.practicing)
            Button(st.practicing ? String(localized: "Stop Practicing") : (st.showPracticePanel ? String(localized: "Hide Practice Panel") : String(localized: "Practice Mode (A/B Repeat)…"))) {
                model.togglePracticePanel()
            }
            .keyboardShortcut("p", modifiers: [.command, .shift]).disabled(!has)
            Button("Set A of Selected Section Here (Timeline)") { model.timelineMarkA() }
                .keyboardShortcut("i", modifiers: [.command, .option]).disabled(!has || st.practicing)
            Button("Set B of Selected Section Here (Timeline)") { model.timelineMarkB() }
                .keyboardShortcut("o", modifiers: [.command, .option]).disabled(!has || st.practicing)
            Divider()
            Button("Add Bookmark") { model.addBookmark() }.keyboardShortcut("d").disabled(!has || st.practicing)
            Button("Back to Previous Take") { model.undoTake() }.keyboardShortcut("z", modifiers: [.command, .option])
                .disabled(!has || !st.undoAvailable || st.practicing)
            Menu("Reset & Power") {
                Button("Soft Reset") { model.softReset() }.keyboardShortcut("r")
                Button("Power Cycle") { model.powerCycle() }.keyboardShortcut("r", modifiers: [.command, .shift])
            }
            .disabled(!has || (!st.recording && !st.practicing))
        }

        // View
        CommandGroup(replacing: .toolbar) {
            Button("Full Screen") { (NSApp.keyWindow ?? model.mainWindow)?.toggleFullScreen(nil) }
                .keyboardShortcut("f", modifiers: [.command, .control])
            Divider()
        }
        CommandGroup(before: .sidebar) {
            Picker("Display Size", selection: Binding(get: { menu.v.integerScale }, set: { model.integerScale = $0 })) {
                Text("Pixel-Perfect (Integer Scale)").tag(true)
                Text("FILL (Fit Window)").tag(false)
            }
            Button("Toggle Pixel-Perfect / FILL") { model.integerScale.toggle() }.keyboardShortcut("f")
            Divider()
            Toggle("Sidebar", isOn: Binding(get: { menu.v.showSidebar }, set: { model.showSidebar = $0 })).keyboardShortcut("s", modifiers: [.command, .option])
            Button("Takes") { openWindow(id: "takes") }.keyboardShortcut("t", modifiers: [.command, .shift])
            Divider()
            Picker("Flash Reduction", selection: Binding(get: { menu.v.flashReduction }, set: { model.flashReduction = $0 })) {
                ForEach(FlashLevel.allCases) { Text($0.label).tag($0.rawValue) }
            }
            Toggle("Stream Output (Syphon)", isOn: Binding(get: { menu.v.streamOn }, set: { stream.setOn($0) }))
            Toggle("Show Latency", isOn: Binding(get: { menu.v.showLatency }, set: { model.showLatency = $0 })).keyboardShortcut("l")
            Divider()
        }

        // Help
        CommandGroup(replacing: .help) {
            Button("Controls Guide") { openWindow(id: "guide") }.keyboardShortcut("?", modifiers: [.command])
        }
    }
}
