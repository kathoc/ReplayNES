// Automatic updates (Sparkle 2, EdDSA-signed appcast on GitHub Releases).
//
// Privacy: Sparkle only contacts the feed URL (Info.plist SUFeedURL) to check for updates and
// the release asset URL to download one. No system profile is sent (SUEnableSystemProfiling is
// left off). Automatic checks start only after the user agrees to Sparkle's standard permission
// prompt (shown on the second launch) and can be turned off in the Quick Menu (Settings › System › Updates).
//
// Installing never bypasses the app's quit path: Sparkle's installer asks the app to quit with a
// normal Apple quit event, so AppDelegate.applicationShouldTerminate still persists the session
// for resuming (no save prompt; see SessionResume.swift) and stops the emulation thread before the bundle is
// replaced and relaunched.
//
// Test hook (scripts / manual verification only):
//   --update-check-now     check in the background right after launch and, once an update has
//                          been downloaded and validated (EdDSA), install and relaunch
//                          immediately instead of waiting for the next quit. Combine with
//                          `-SUFeedURL http://127.0.0.1:<port>/appcast.xml -SUAutomaticallyUpdate YES`
//                          to exercise the full update cycle against a local server.
// SPDX-License-Identifier: GPL-2.0-or-later
import Combine
import Sparkle
import SwiftUI

final class UpdaterModel: NSObject, ObservableObject, SPUUpdaterDelegate {
    static let shared = UpdaterModel()

    @Published private(set) var canCheckForUpdates = false
    @Published var automaticallyChecks = false {
        didSet { if updater?.automaticallyChecksForUpdates != automaticallyChecks { updater?.automaticallyChecksForUpdates = automaticallyChecks } }
    }
    @Published var automaticallyDownloads = false {
        didSet { if updater?.automaticallyDownloadsUpdates != automaticallyDownloads { updater?.automaticallyDownloadsUpdates = automaticallyDownloads } }
    }

    private var controller: SPUStandardUpdaterController?
    private var updater: SPUUpdater? { controller?.updater }
    private var installImmediately = false
    private var bag: [AnyCancellable] = []

    /// Starts Sparkle. Called once from applicationDidFinishLaunching.
    func start(arguments: [String]) {
        guard controller == nil else { return }
        installImmediately = arguments.contains("--update-check-now")
        let c = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: self, userDriverDelegate: nil)
        controller = c
        let u = c.updater
        u.publisher(for: \.canCheckForUpdates).receive(on: DispatchQueue.main)
            .sink { [weak self] in self?.canCheckForUpdates = $0 }.store(in: &bag)
        // Sparkle changes these itself (permission prompt, "automatically install" checkbox).
        u.publisher(for: \.automaticallyChecksForUpdates).receive(on: DispatchQueue.main)
            .sink { [weak self] in if self?.automaticallyChecks != $0 { self?.automaticallyChecks = $0 } }.store(in: &bag)
        u.publisher(for: \.automaticallyDownloadsUpdates).receive(on: DispatchQueue.main)
            .sink { [weak self] in if self?.automaticallyDownloads != $0 { self?.automaticallyDownloads = $0 } }.store(in: &bag)
        if installImmediately { checkInBackgroundWhenIdle(attempts: 40) }
    }

    /// Sparkle briefly holds a session at startup (probing for a resumable install, or its own
    /// scheduled check) and rejects a background check during it, so retry until it is idle.
    private func checkInBackgroundWhenIdle(attempts: Int) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self, let u = self.updater, attempts > 0 else { return }
            if u.sessionInProgress { self.checkInBackgroundWhenIdle(attempts: attempts - 1); return }
            u.checkForUpdatesInBackground()
        }
    }

    /// Menu: Check for Updates… (Sparkle standard UI).
    func checkForUpdates() { controller?.checkForUpdates(nil) }

    var lastCheck: Date? { updater?.lastUpdateCheckDate }

    // MARK: SPUUpdaterDelegate

    func updater(_ updater: SPUUpdater, willInstallUpdateOnQuit item: SUAppcastItem,
                 immediateInstallationBlock immediateInstallHandler: @escaping () -> Void) -> Bool {
        guard installImmediately else { return false }   // default: install when the user quits
        NSLog("ReplayNES: --update-check-now: installing %@ now", item.displayVersionString)
        DispatchQueue.main.async { immediateInstallHandler() }
        return true
    }

    func updater(_ updater: SPUUpdater, didAbortWithError error: Error) {
        if installImmediately { NSLog("ReplayNES: update aborted: %@", error.localizedDescription) }
    }
}

/// App menu item: Check for Updates…
struct CheckForUpdatesCommand: View {
    @ObservedObject var updates: UpdaterModel
    var body: some View {
        Button("Check for Updates…") { updates.checkForUpdates() }
            .disabled(!updates.canCheckForUpdates)
    }
}
