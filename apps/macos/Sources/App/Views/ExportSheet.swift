// MP4 export sheet. Rendering runs offline on a fresh core (rn_renderer) on a background
// thread; the project is never modified and play can continue meanwhile. The controller's
// confirm / cancel work its buttons (AppModel.exportNav): Export… / Cancel, then Close when it is
// done or failed (while it runs the pad stays the game's; Cancel is the mouse / Esc). Problems with the settings are shown inline (no alert).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

struct ExportSheet: View {
    @ObservedObject private var clock = AppModel.shared.clock   // take length while recording (AppModel.status)
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var settings = ExportSettings()
    @State private var cropOverscan = true
    @State private var wholeTake = true
    @State private var startFrame = 0
    @State private var endFrame = 0
    @State private var applyFlash = true
    @State private var applyCRT = false
    @State private var formError: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Export to MP4").font(.title2.bold())
            if let job = model.exportJob {
                ExportProgressView(job: job) { model.exportJob = nil; dismiss() }
            } else {
                form
            }
        }
        .padding(20)
        .frame(width: 520)
        .onReceive(model.exportNav) { navInput($0) }
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
        s.cropLeft = cropOverscan ? 8 : 0
        s.cropRight = cropOverscan ? 8 : 0
        return ExportGeometry(s)
    }

    @ViewBuilder private var form: some View {
        Form {
            Picker("Codec", selection: $settings.codec) {
                ForEach(ExportSettings.Codec.allCases) { Text($0.label).tag($0) }
            }
            Picker("Size", selection: $settings.preset) {
                ForEach(ExportSettings.SizePreset.all) { Text($0.label).tag($0) }
            }
            Toggle("Hide overscan (crop 8 px on each side)", isOn: $cropOverscan)
            Picker("Pixel Aspect Ratio", selection: $settings.pixelAspect87) {
                Text("1:1 (square pixels)").tag(false)
                Text("8:7 (as on a CRT TV)").tag(true)
            }
            Toggle(isOn: $applyFlash) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Apply Flash Reduction")
                    Text("Exports the video with intense flashing reduced (level: \(exportFlashLevel.label); change it in Settings › Display). When off, the video is exactly as recorded.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle(isOn: $applyCRT) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Apply CRT Effect")
                    Text("Renders each frame with the CRT Display settings, using the same Metal pipeline (4:3, internal resolution up to 1600×1200; best with 4:3 sizes such as 1280×960). Exporting is slower.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Toggle("Whole Take", isOn: $wholeTake)
            if !wholeTake {
                HStack {
                    TextField("Start Frame", value: $startFrame, format: .number)
                    TextField("End Frame", value: $endFrame, format: .number)
                }
            }
            let g = geometry
            LabeledContent("Output") {
                let canvas = "\(g.canvasWidth)×\(g.canvasHeight)", picture = "\(g.dstWidth)×\(g.dstHeight)"
                Text("\(canvas) (picture \(picture), vertical ×\(g.verticalScale), nearest neighbor)")
                    .font(.caption)
            }
            let frames = wholeTake ? model.status.takeLength : UInt64(max(0, endFrame - startFrame))
            Picker("Bit rate", selection: $settings.quality) {
                ForEach(ExportSettings.Quality.allCases) { q in
                    Text(qualityLabel(q, geometry: g, frames: frames)).tag(q)
                }
            }
            Text("Actual size depends on the picture").font(.caption).foregroundStyle(.secondary)
            LabeledContent("Length") {
                Text("\(Engine.timecode(forFrame: frames)) (\(frames) frames, 60.0988 fps, AAC 48 kHz)").font(.caption)
            }
        }
        Text("The export is made by re-running the recorded input from the start in a separate emulator. The project is not changed, and you can keep playing while it exports.")
            .font(.caption).foregroundStyle(.secondary)
        if let formError {
            Label(formError, systemImage: "exclamationmark.triangle.fill").font(.caption).foregroundStyle(.orange)
        }
        HStack {
            Spacer()
            Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
            Button("Export…") { chooseAndStart() }.keyboardShortcut(.defaultAction)
                .disabled(model.status.takeLength == 0)
        }
    }

    /// "Standard (YouTube) · 12.0 Mbit/s · ≈ 91 MB (estimate)": the size estimated for exactly this
    /// range, canvas, codec and bit rate (rnf_export_predict_size); simple pictures give smaller files.
    private func qualityLabel(_ q: ExportSettings.Quality, geometry g: ExportGeometry, frames: UInt64) -> String {
        let hevc = settings.codec == .hevc
        let mbit = String(format: "%.1f", Double(g.videoBitrate(hevc: hevc, quality: q)) / 1e6)
        let size = ByteCountFormatter.string(fromByteCount: g.predictedBytes(hevc: hevc, quality: q, frames: frames), countStyle: .file)
        return String(localized: "\(q.label) · \(mbit) Mbit/s · ≈ \(size) (estimate)")
    }

    /// Controller confirm / cancel (the sheet's window has the keyboard itself).
    private func navInput(_ n: NavInput) {
        let ok = n == .confirm, cancel = n == .back || n == .escape
        guard ok || cancel else { return }
        if let job = model.exportJob {
            if job.finished { model.exportJob = nil; dismiss() } else if cancel { job.cancel() }
        } else if cancel {
            dismiss()
        } else if model.status.takeLength > 0 {
            chooseAndStart()
        }
    }

    /// The current setting's level; Standard when the setting is off but the export option is turned on.
    private var exportFlashLevel: FlashLevel { model.flashLevel == .off ? .standard : model.flashLevel }

    private func chooseAndStart() {
        var s = settings
        s.flashReduction = applyFlash ? exportFlashLevel : .off
        s.crt = applyCRT ? CRTSettingsModel.shared.settings : nil
        s.cropTop = cropOverscan ? 8 : 0
        s.cropBottom = cropOverscan ? 8 : 0
        s.cropLeft = cropOverscan ? 8 : 0
        s.cropRight = cropOverscan ? 8 : 0
        if wholeTake {
            s.startFrame = 0; s.endFrame = 0
        } else {
            s.startFrame = UInt64(max(0, startFrame))
            s.endFrame = UInt64(max(0, min(endFrame, Int(model.status.takeLength))))
        }
        do { try s.validate() } catch {
            formError = String(localized: "Can’t export") + ": " + error.localizedDescription
            return
        }
        formError = nil
        let panel = NSSavePanel()
        panel.title = String(localized: "Where to Save the MP4")
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
                    Label("Export finished", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    Text("\(r.frames) frames · \(String(format: "%.2f", r.duration)) s · \(r.audioSamples) audio samples")
                        .font(.caption)
                    Text(String(format: String(localized: "Replay hash %016llx"), r.rendererHash)).font(.caption.monospaced()).foregroundStyle(.secondary)
                    Text(verbatim: r.encoder).font(.caption).foregroundStyle(.secondary)
                    HStack {
                        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([r.url]) }
                        Spacer()
                        Button("Close", action: close).keyboardShortcut(.defaultAction)
                    }
                } else {
                    Label(job.error ?? String(localized: "Failed"), systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    HStack { Spacer(); Button("Close", action: close).keyboardShortcut(.defaultAction) }
                }
            } else {
                ProgressView(value: job.total == 0 ? 0 : Double(job.done) / Double(job.total))
                Text("\(job.done) / \(job.total) frames").font(.caption.monospaced())
                HStack { Spacer(); Button("Cancel") { job.cancel() }.keyboardShortcut(.cancelAction) }
            }
        }
    }
}
