// CRT display settings (Quick Menu: Settings › Display › CRT / CRT Details): the physical CRT display model ported from nesterm
// (Sources/Core/CRT, docs/CRT_PORT.md). Display-only: never affects emulation, recording or hashes.
// Parameters, ranges and defaults are nesterm's (web/index.html, web/app.mjs, physical-worker.mjs).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Where the Syphon output takes its picture from.
enum CRTSyphonMode: String, CaseIterable, Identifiable {
    case follow, off, on
    var id: String { rawValue }
}

/// Main thread only (read by the renderers through `snapshot`, which is lock-protected).
final class CRTSettingsModel: ObservableObject {
    static let shared = CRTSettingsModel()

    @AppStorage("crtEnabled") private var storedEnabled = false { didSet { changed() } }
    @AppStorage("crtLines") var lines = 240 { didSet { changed() } }
    @AppStorage("crtBeamGrowth") var beamGrowth = true { didSet { changed() } }
    @AppStorage("crtPersistence") var persistence = true { didSet { changed() } }
    @AppStorage("crtSupply") var supply = true { didSet { changed() } }
    @AppStorage("crtAntenna") var antennaDbuv = 65.0 { didSet { changed() } }
    @AppStorage("crtSyphon") var syphonModeRaw = CRTSyphonMode.follow.rawValue { didSet { changed() } }

    /// --crt: on for this run only (scripted checks / measurements), not saved.
    var launchOverride = false { didSet { changed() } }

    var enabled: Bool {
        get { storedEnabled || launchOverride }
        set { storedEnabled = newValue; if !newValue { launchOverride = false } }
    }
    var reducedLines: Bool {
        get { lines != 240 }
        set { lines = newValue ? 160 : 240 }
    }
    var syphonMode: CRTSyphonMode { CRTSyphonMode(rawValue: syphonModeRaw) ?? .follow }

    var settings: CRTRenderer.Settings {
        CRTRenderer.Settings(lines: CRTRenderer.Settings.validLines(lines) ? lines : 240, beamGrowth: beamGrowth,
                             persistence: persistence, supply: supply, antennaDbuv: antennaDbuv, ambientLux: 40)
    }

    /// nesterm's shipped defaults (all effects on, 65 dBuV, native 240 lines).
    func resetToNestermDefaults() {
        lines = 240; beamGrowth = true; persistence = true; supply = true; antennaDbuv = 65
    }

    // Thread-safe copy for the render / Syphon threads.
    struct Snapshot: Equatable {
        var enabled = false
        var settings = CRTRenderer.Settings()
        var syphon = CRTSyphonMode.follow
        var syphonUsesCRT: Bool { syphon == .on || (syphon == .follow && enabled) }
    }
    private let lock = NSLock()
    private var current = Snapshot()
    var snapshot: Snapshot { lock.lock(); defer { lock.unlock() }; return current }

    private init() { changed(notify: false) }

    private func changed(notify: Bool = true) {
        let s = Snapshot(enabled: enabled, settings: settings, syphon: syphonMode)
        lock.lock(); current = s; lock.unlock()
        if notify { objectWillChange.send() }
    }
}
