// Settings → "CRT Display": settings of the physical CRT display model ported from nesterm
// (Sources/Core/CRT, docs/CRT_PORT.md). Display-only: never affects emulation, recording or hashes.
// Parameters, ranges and defaults are nesterm's (web/index.html, web/app.mjs, physical-worker.mjs).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Where the Syphon output takes its picture from.
enum CRTSyphonMode: String, CaseIterable, Identifiable {
    case follow, off, on
    var id: String { rawValue }
    var label: String {
        switch self {
        case .follow: return String(localized: "Follow the display")
        case .off: return String(localized: "Always original (no CRT)")
        case .on: return String(localized: "Always CRT")
        }
    }
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

/// Settings section (Display & Audio tab).
struct CRTSettingsSection: View {
    @ObservedObject var crt = CRTSettingsModel.shared
    var body: some View {
        Section("CRT Display") {
            Toggle("CRT display", isOn: Binding(get: { crt.enabled }, set: { crt.enabled = $0 }))
            Group {
                Toggle("Thicker scanlines where brighter", isOn: $crt.beamGrowth)
                Toggle("Phosphor persistence", isOn: $crt.persistence)
                Toggle("Bright screens widen and dim the picture", isOn: $crt.supply)
                LabeledContent("Signal strength (source picture)") {
                    HStack {
                        Slider(value: $crt.antennaDbuv, in: 20...90, step: 1).frame(width: 180)
                        Text(verbatim: "\(Int(crt.antennaDbuv)) dBµV").monospacedDigit().frame(width: 64, alignment: .trailing)
                    }
                }
                Picker("Scanlines", selection: Binding(get: { crt.reducedLines }, set: { crt.reducedLines = $0 })) {
                    Text("Standard: 240 lines").tag(false)
                    Text("Reduced-line experiment").tag(true)
                }
                if crt.reducedLines {
                    LabeledContent("Experiment: 110–220 lines") {
                        HStack {
                            Slider(value: Binding(get: { Double(crt.lines) }, set: { crt.lines = Int($0) }), in: 110...220, step: 1).frame(width: 180)
                            Text(verbatim: "\(crt.lines)").monospacedDigit().frame(width: 40, alignment: .trailing)
                        }
                    }
                }
                Picker("Stream output (Syphon) picture", selection: $crt.syphonModeRaw) {
                    ForEach(CRTSyphonMode.allCases) { Text($0.label).tag($0.rawValue) }
                }
                HStack {
                    Spacer()
                    Button("Reset to nesterm Defaults") { crt.resetToNestermDefaults() }.controlSize(.small)
                }
            }
            .disabled(!crt.enabled)
            Text("A Metal port of nesterm’s “CRT (physical model, experimental)”: NES pixel codes → RF/IF → demodulation → screen (slot mask, scattering, persistence). It is an uncalibrated experimental model, not a reproduction of any real TV. It only affects the display; recording, game progress and reproducibility are unaffected. The picture is 4:3 (the pixel aspect setting isn’t used) and the screen’s internal resolution is up to 1600×1200. Frames changed by flash reduction are drawn from the reduced RGB image.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }
}
