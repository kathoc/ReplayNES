// 表示 → 「ブラウン管 (CRT)」: settings of the physical CRT display model ported from nesterm
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
        case .follow: return "表示に合わせる"
        case .off: return "常にオリジナル（CRT なし）"
        case .on: return "常にブラウン管 (CRT)"
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

/// Settings section (表示・音声 tab).
struct CRTSettingsSection: View {
    @ObservedObject var crt = CRTSettingsModel.shared
    var body: some View {
        Section("ブラウン管 (CRT)") {
            Toggle("ブラウン管 (CRT) 表示", isOn: Binding(get: { crt.enabled }, set: { crt.enabled = $0 }))
            Group {
                Toggle("明るい所ほど走査線が太る", isOn: $crt.beamGrowth)
                Toggle("蛍光体の残光", isOn: $crt.persistence)
                Toggle("明るい画面で幅が広がり暗くなる", isOn: $crt.supply)
                LabeledContent("電波の強さ（元の画像）") {
                    HStack {
                        Slider(value: $crt.antennaDbuv, in: 20...90, step: 1).frame(width: 180)
                        Text("\(Int(crt.antennaDbuv)) dBµV").monospacedDigit().frame(width: 64, alignment: .trailing)
                    }
                }
                Picker("走査線", selection: Binding(get: { crt.reducedLines }, set: { crt.reducedLines = $0 })) {
                    Text("標準：240本").tag(false)
                    Text("低走査線の仮想実験").tag(true)
                }
                if crt.reducedLines {
                    LabeledContent("実験：110〜220本") {
                        HStack {
                            Slider(value: Binding(get: { Double(crt.lines) }, set: { crt.lines = Int($0) }), in: 110...220, step: 1).frame(width: 180)
                            Text("\(crt.lines)").monospacedDigit().frame(width: 40, alignment: .trailing)
                        }
                    }
                }
                Picker("配信出力 (Syphon) の映像", selection: $crt.syphonModeRaw) {
                    ForEach(CRTSyphonMode.allCases) { Text($0.label).tag($0.rawValue) }
                }
                HStack {
                    Spacer()
                    Button("nesterm の既定値に戻す") { crt.resetToNestermDefaults() }.controlSize(.small)
                }
            }
            .disabled(!crt.enabled)
            Text("nesterm の「CRT（物理モデル・実験）」を Metal に移植した表示です。NES の画素コード → RF/IF → 復調 → 管面（スロットマスク・散乱・残光）。未校正の実験モデルで、実在のテレビの再現ではありません。表示だけの処理で、記録・ゲームの進行・再現性には影響しません。画面は 4:3（ピクセル比の設定は使いません）、管面の内部解像度は最大 1600×1200 です。フラッシュ低減が画面を変更しているフレームは、低減後の RGB 画像から描画します。")
                .font(.caption).foregroundStyle(.secondary)
        }
    }
}
