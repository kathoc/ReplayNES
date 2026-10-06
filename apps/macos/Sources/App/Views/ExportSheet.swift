// MP4 export sheet. Rendering runs offline on a fresh core (rn_renderer) on a background
// thread; the project is never modified and play can continue meanwhile.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

struct ExportSheet: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var settings = ExportSettings()
    @State private var cropOverscan = true
    @State private var wholeTake = true
    @State private var startFrame = 0
    @State private var endFrame = 0
    @State private var applyFlash = true
    @State private var applyCRT = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("MP4に書き出す").font(.title2.bold())
            if let job = model.exportJob {
                ExportProgressView(job: job) { model.exportJob = nil; dismiss() }
            } else {
                form
            }
        }
        .padding(20)
        .frame(width: 520)
        .onAppear {
            endFrame = Int(model.status.takeLength)
            applyFlash = model.flashLevel != .off
            applyCRT = CRTSettingsModel.shared.enabled
        }
    }

    private var geometry: ExportGeometry {
        var s = settings
        s.cropTop = cropOverscan ? 8 : 0
        s.cropBottom = cropOverscan ? 8 : 0
        return ExportGeometry(s)
    }

    @ViewBuilder private var form: some View {
        Form {
            Picker("コーデック", selection: $settings.codec) {
                ForEach(ExportSettings.Codec.allCases) { Text($0.label).tag($0) }
            }
            Picker("サイズ", selection: $settings.preset) {
                ForEach(ExportSettings.SizePreset.all) { Text($0.label).tag($0) }
            }
            Toggle("オーバースキャンを隠す（上下 8px をクロップ）", isOn: $cropOverscan)
            Picker("ピクセル比", selection: $settings.pixelAspect87) {
                Text("1:1（正方形ピクセル）").tag(false)
                Text("8:7（ブラウン管の見た目）").tag(true)
            }
            Toggle(isOn: $applyFlash) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("フラッシュ低減を適用")
                    Text("激しい点滅を抑えた映像で書き出します（強さ: \(exportFlashLevel.label)。設定 → 表示・音声で変更）。オフにすると記録どおりの映像です。")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: $applyCRT) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("ブラウン管効果を適用")
                    Text("表示 → ブラウン管 (CRT) の設定で、同じ Metal パイプラインを使って 1 フレームずつ描画します（4:3・内部解像度は最大 1600×1200、1280×960 などの 4:3 サイズ向き）。書き出しは遅くなります。")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle("テイク全体", isOn: $wholeTake)
            if !wholeTake {
                HStack {
                    TextField("開始フレーム", value: $startFrame, format: .number)
                    TextField("終了フレーム", value: $endFrame, format: .number)
                }
            }
            let g = geometry
            LabeledContent("出力") {
                Text("\(g.canvasWidth)×\(g.canvasHeight)（画像 \(g.dstWidth)×\(g.dstHeight)、縦 \(g.verticalScale) 倍・最近傍）")
                    .font(.caption)
            }
            LabeledContent("長さ") {
                let frames = wholeTake ? model.status.takeLength : UInt64(max(0, endFrame - startFrame))
                Text("\(Engine.timecode(forFrame: frames))（\(frames) フレーム, 60.0988 fps, AAC 48 kHz）").font(.caption)
            }
        }
        Text("書き出しは記録された入力を最初から別のエミュレーターで再実行して作ります。プロジェクトは変更されず、書き出し中もプレイを続けられます。")
            .font(.caption).foregroundStyle(.secondary)
        HStack {
            Spacer()
            Button("キャンセル") { dismiss() }.keyboardShortcut(.cancelAction)
            Button("書き出す…") { chooseAndStart() }.keyboardShortcut(.defaultAction)
                .disabled(model.status.takeLength == 0)
        }
    }

    /// The current setting's level; 標準 when the setting is off but the export option is turned on.
    private var exportFlashLevel: FlashLevel { model.flashLevel == .off ? .standard : model.flashLevel }

    private func chooseAndStart() {
        var s = settings
        s.flashReduction = applyFlash ? exportFlashLevel : .off
        s.crt = applyCRT ? CRTSettingsModel.shared.settings : nil
        s.cropTop = cropOverscan ? 8 : 0
        s.cropBottom = cropOverscan ? 8 : 0
        if wholeTake {
            s.startFrame = 0; s.endFrame = 0
        } else {
            s.startFrame = UInt64(max(0, startFrame))
            s.endFrame = UInt64(max(0, min(endFrame, Int(model.status.takeLength))))
        }
        do { try s.validate() } catch {
            model.showError("書き出せません", error.localizedDescription); return
        }
        let panel = NSSavePanel()
        panel.title = "MP4の保存先"
        panel.allowedContentTypes = [.mpeg4Movie]
        let base = model.status.projectPath.isEmpty ? model.status.romPath : model.status.projectPath
        panel.nameFieldStringValue = URL(fileURLWithPath: base).deletingPathExtension().lastPathComponent + ".mp4"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        model.startExport(s, to: url)
    }
}

struct ExportProgressView: View {
    @ObservedObject var job: ExportJob
    let close: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(job.url.lastPathComponent).font(.headline)
            if job.finished {
                if let r = job.result {
                    Label("書き出しが完了しました", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    Text("\(r.frames) フレーム · \(String(format: "%.2f", r.duration)) 秒 · 音声 \(r.audioSamples) サンプル")
                        .font(.caption)
                    Text(String(format: "再生ハッシュ %016llx", r.rendererHash)).font(.caption.monospaced()).foregroundStyle(.secondary)
                    HStack {
                        Button("Finderで表示") { NSWorkspace.shared.activateFileViewerSelecting([r.url]) }
                        Spacer()
                        Button("閉じる", action: close).keyboardShortcut(.defaultAction)
                    }
                } else {
                    Label(job.error ?? "失敗しました", systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    HStack { Spacer(); Button("閉じる", action: close).keyboardShortcut(.defaultAction) }
                }
            } else {
                ProgressView(value: job.total == 0 ? 0 : Double(job.done) / Double(job.total))
                Text("\(job.done) / \(job.total) フレーム").font(.caption.monospaced())
                HStack { Spacer(); Button("キャンセル") { job.cancel() }.keyboardShortcut(.cancelAction) }
            }
        }
    }
}
