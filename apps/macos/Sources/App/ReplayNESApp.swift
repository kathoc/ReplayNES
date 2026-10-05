// App entry, menus, launch arguments, document opening, clean shutdown.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = AppModel.shared
    private var pendingOpen: [URL] = []
    private var launched = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        model.start()
        launched = true
        // Launch arguments (also used by scripted smoke tests):
        //   --rom <path>        play a ROM without a project (in-memory)
        //   --project <path>    open a .nesrec project
        //   --autoplay          with --rom: start running in record mode immediately
        //   --snapshot <png> [--snapshot-delay s] [--quit-after-snapshot]  capture window + stats
        let args = ProcessInfo.processInfo.arguments
        func arg(_ name: String) -> String? {
            guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
            return args[i + 1]
        }
        if let rom = arg("--rom") {
            model.createSession(rom: URL(fileURLWithPath: rom), projectDir: nil, autoplay: args.contains("--autoplay"))
        } else if let p = arg("--project") {
            model.openProject(URL(fileURLWithPath: p))
        } else if !pendingOpen.isEmpty {
            open(pendingOpen)
        } else {
            DispatchQueue.main.async { self.model.checkCrashRecovery() }
        }
        pendingOpen = []
        if let snap = arg("--snapshot") {
            let delay = Double(arg("--snapshot-delay") ?? "3") ?? 3
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                self.model.writeSnapshot(to: URL(fileURLWithPath: snap))
                if args.contains("--quit-after-snapshot") {
                    self.model.shutdown()
                    exit(0)
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
        guard model.confirmDiscardIfNeeded() else { return }
        if url.pathExtension.lowercased() == "nesrec" {
            model.openProject(url)
        } else {
            model.createSession(rom: url, projectDir: nil)
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if !model.confirmDiscardIfNeeded() { return .terminateCancel }
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
        .defaultSize(width: 1240, height: 820)
        .commands { AppCommands(model: AppModel.shared) }

        Window("テイク一覧", id: "takes") {
            TakesPanel().environmentObject(AppModel.shared)
        }
        .defaultSize(width: 720, height: 420)

        Settings {
            SettingsView().environmentObject(AppModel.shared)
        }
    }
}

struct AppCommands: Commands {
    @ObservedObject var model: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("新規プロジェクト…") { model.newProject() }.keyboardShortcut("n")
            Button("ROMを開いて試す（保存しない）…") { model.quickPlay() }.keyboardShortcut("n", modifiers: [.command, .shift])
            Button("プロジェクトを開く…") { model.openProjectPanel() }.keyboardShortcut("o")
            Divider()
            Button("プロジェクトを閉じる") { model.closeProject() }.disabled(!model.status.hasSession)
        }
        CommandGroup(replacing: .saveItem) {
            Button("保存") { model.saveSync() }.keyboardShortcut("s").disabled(!model.status.hasSession)
            Button("別名で保存…") { model.saveAs() }.keyboardShortcut("s", modifiers: [.command, .shift]).disabled(!model.status.hasSession)
            Divider()
            Button("MP4に書き出す…") { model.showExport = true }.keyboardShortcut("e")
                .disabled(!model.status.hasSession || model.status.takeLength == 0)
        }
        CommandMenu("操作") {
            let has = model.status.hasSession
            Button(model.status.paused ? "再開" : "一時停止") { model.togglePause() }.keyboardShortcut("p").disabled(!has)
            Button("コマ送り") { model.frameAdvance() }.keyboardShortcut(.rightArrow, modifiers: [.command]).disabled(!has)
            Button("1コマ戻る") { model.stepBack() }.keyboardShortcut(.leftArrow, modifiers: [.command]).disabled(!has)
            Button("先頭へ") { model.seek(to: 0) }.keyboardShortcut(.home, modifiers: [.command]).disabled(!has)
            Menu("スロー") {
                ForEach(SlowRate.allCases) { r in
                    Button(r.label) { model.setSlow(r) }.keyboardShortcut(KeyEquivalent(Character("\(r == .normal ? 1 : r == .half ? 2 : 3)")), modifiers: [.command])
                }
            }.disabled(!has)
            Divider()
            Button(model.status.recording ? "再生モードにする" : "録画モードにする") { model.setRecording(!model.status.recording) }
                .keyboardShortcut("m", modifiers: [.command, .shift]).disabled(!has)
            Button("ここから録り直す") { model.rerecordHere() }.keyboardShortcut(.return, modifiers: [.command]).disabled(!has)
            Button("前の試行へ戻す") { model.undoTake() }.keyboardShortcut("z", modifiers: [.command, .option])
                .disabled(!has || model.status.undoDepth == 0)
            Button("ブックマークを追加") { model.addBookmark() }.keyboardShortcut("d").disabled(!has)
            Divider()
            Button("ソフトリセット") { model.softReset() }.keyboardShortcut("r").disabled(!has || !model.status.recording)
            Button("電源再投入") { model.powerCycle() }.keyboardShortcut("r", modifiers: [.command, .shift]).disabled(!has || !model.status.recording)
        }
        CommandGroup(after: .sidebar) {
            Button(model.showLatency ? "レイテンシ表示を隠す" : "レイテンシ表示") { model.showLatency.toggle() }.keyboardShortcut("l")
            Button(model.showSidebar ? "サイドバーを隠す" : "サイドバーを表示") { model.showSidebar.toggle() }.keyboardShortcut("s", modifiers: [.command, .option])
            Button("テイク一覧") { openWindow(id: "takes") }.keyboardShortcut("t", modifiers: [.command, .shift])
            Divider()
        }
    }
}
