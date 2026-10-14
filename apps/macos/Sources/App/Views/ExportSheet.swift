// The Make Video dialog (docs/design/VTR_UX.md, "Make Video dialog"): one screen of at most six
// rows - Range (All / Up to here / Section n), Quality (Standard (YouTube) / High / Small), the
// length and an estimated size, Details › (codec, size, pixel aspect, overscan, flash reduction,
// CRT, bit rate: the existing export settings) and Export / Cancel. Rendering runs offline on a
// fresh core (rn_renderer) on a background thread after Export: the dialog closes, play can go on,
// the status label shows the progress and a toast offers Open / Share at the end. Opened again
// while a video is being made it shows the progress (Cancel). The controller navigates it
// (AppModel.exportNav): up / down the rows, left / right the choices, confirm, cancel.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

struct ExportSheet: View {
    @ObservedObject private var clock = AppModel.shared.clock   // cursor / tape length (AppModel.status)
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var settings = ExportSettings()
    @State private var cropOverscan = true
    @State private var range: MakeVideo.Range = .all
    @State private var applyFlash = true
    @State private var applyCRT = false
    @State private var showDetails = false
    @State private var focus: Row = .range
    @State private var formError: String?

    /// Pad focus: the rows, then the two buttons.
    enum Row: Int, CaseIterable { case range, quality, details, export, cancel }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Make Video").font(.title2.bold())
            if let job = model.exportJob {
                ExportProgressView(job: job) { model.exportJob = nil; dismiss() }
            } else if showDetails {
                details
            } else {
                main
            }
        }
        .padding(20)
        .frame(width: 560)
        .onReceive(model.exportNav) { navInput($0) }
        .onAppear {
            applyFlash = model.flashLevel != .off
            applyCRT = CRTSettingsModel.shared.enabled
        }
    }

    // MARK: main screen

    /// The A/B sections on the active take with both A and B.
    private var sections: [TimelineRange] {
        let st = clock.status
        return TimelineEditing.visibleRanges(slots: model.practiceSlots, takes: model.takes, activeTake: st.activeTake,
                                             takeLength: st.takeLength).filter { $0.b != nil }
    }

    private var ranges: [(MakeVideo.Range, String)] {
        [(.all, String(localized: "All (range)")), (.toHere, String(localized: "Up to here"))]
            + sections.map { (.section($0.slot), String(localized: "Section \($0.slot + 1)")) }
    }

    private var frames: (start: UInt64, end: UInt64)? {
        let st = clock.status
        var section: TimelineRange?
        if case .section(let n) = range { section = sections.first { $0.slot == n } }
        return MakeVideo.frames(range, cursor: st.frame, takeLength: st.takeLength, section: section)
    }

    private var preset: Int { MakeVideo.preset(for: settings.quality) }

    @ViewBuilder private var main: some View {
        let rs = ranges
        let fr = frames
        VStack(alignment: .leading, spacing: 10) {
            row(.range, String(localized: "Range")) {
                ChoiceChips(labels: rs.map(\.1), selected: rs.firstIndex { $0.0 == range } ?? 0) { range = rs[$0].0 }
            }
            row(.quality, String(localized: "Quality (video)")) {
                ChoiceChips(labels: (0..<MakeVideo.qualityPresets).map { MakeVideo.qualityLabel($0) }, selected: preset) {
                    settings.quality = MakeVideo.quality($0)
                }
            }
            row(nil, String(localized: "Length")) {
                Text(verbatim: fr.map { f in
                    let n = f.end - f.start
                    return MakeVideo.summary(frames: n, bytes: geometry.predictedBytes(hevc: settings.codec == .hevc, quality: settings.quality, frames: n))
                } ?? "–")
                .font(.callout.monospacedDigit())
            }
            row(.details, nil) {
                Button(String(localized: "Details ›")) { showDetails = true }.buttonStyle(.link)
            }
        }
        if let formError {
            Label(formError, systemImage: "exclamationmark.triangle.fill").font(.caption).foregroundStyle(.orange)
        }
        HStack {
            Spacer()
            Button(String(localized: "Cancel (make video)")) { dismiss() }.keyboardShortcut(.cancelAction)
                .overlay(focusRing(.cancel))
            Button(String(localized: "Export")) { chooseAndStart() }.keyboardShortcut(.defaultAction)
                .disabled(fr == nil)
                .overlay(focusRing(.export))
        }
    }

    /// A labelled row; `id` = its pad focus (nil: not focusable).
    private func row<C: View>(_ id: Row?, _ label: String?, @ViewBuilder _ content: () -> C) -> some View {
        HStack(alignment: .center, spacing: 12) {
            Text(verbatim: label ?? "").font(.callout.weight(.medium)).frame(width: 90, alignment: .leading)
            content()
            Spacer(minLength: 0)
        }
        .padding(.vertical, 6).padding(.horizontal, 8)
        .background(RoundedRectangle(cornerRadius: 8).stroke(Color.accentColor, lineWidth: 2).opacity(id != nil && focus == id ? 1 : 0))
    }

    private func focusRing(_ r: Row) -> some View {
        RoundedRectangle(cornerRadius: 7).stroke(Color.accentColor, lineWidth: 2).padding(-3).opacity(focus == r ? 1 : 0)
            .allowsHitTesting(false)
    }

    // MARK: details (the existing export settings)

    private var geometry: ExportGeometry {
        var s = settings
        s.cropTop = cropOverscan ? 8 : 0
        s.cropBottom = cropOverscan ? 8 : 0
        s.cropLeft = cropOverscan ? 8 : 0
        s.cropRight = cropOverscan ? 8 : 0
        return ExportGeometry(s)
    }

    @ViewBuilder private var details: some View {
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
            let g = geometry
            LabeledContent("Output") {
                let canvas = "\(g.canvasWidth)×\(g.canvasHeight)", picture = "\(g.dstWidth)×\(g.dstHeight)"
                Text("\(canvas) (picture \(picture), vertical ×\(g.verticalScale), nearest neighbor)")
                    .font(.caption)
            }
            let n = frames.map { $0.end - $0.start } ?? 0
            Picker("Bit rate", selection: $settings.quality) {
                ForEach(ExportSettings.Quality.allCases) { q in
                    Text(qualityLabel(q, geometry: g, frames: n)).tag(q)
                }
            }
            Text("Actual size depends on the picture").font(.caption).foregroundStyle(.secondary)
        }
        Text("The export is made by re-running the recorded input from the start in a separate emulator. The project is not changed, and you can keep playing while it exports.")
            .font(.caption).foregroundStyle(.secondary)
        HStack {
            Spacer()
            Button(String(localized: "Back")) { showDetails = false }.keyboardShortcut(.cancelAction)
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

    // MARK: controller

    /// Controller navigation (the sheet's window has the keyboard itself).
    private func navInput(_ n: NavInput) {
        let ok = n == .confirm, cancel = n == .back || n == .escape
        if let job = model.exportJob {
            if job.finished { if ok || cancel { model.exportJob = nil; dismiss() } } else if cancel { job.cancel() }
            return
        }
        if showDetails {
            if cancel || ok { showDetails = false }
            return
        }
        switch n {
        case .back, .escape: dismiss()
        case .up, .down:
            // Rows top to bottom, then the buttons (one level).
            let order: [Row] = [.range, .quality, .details, .export]
            let at = order.firstIndex(of: focus == .cancel ? .export : focus) ?? 0
            let next = max(0, min(order.count - 1, at + (n == .down ? 1 : -1)))
            focus = order[next]
        case .left, .right:
            let d = n == .right ? 1 : -1
            switch focus {
            case .range:
                let rs = ranges
                let i = rs.firstIndex { $0.0 == range } ?? 0
                range = rs[max(0, min(rs.count - 1, i + d))].0
            case .quality:
                settings.quality = MakeVideo.quality(max(0, min(MakeVideo.qualityPresets - 1, preset + d)))
            case .export, .cancel:
                focus = d < 0 ? .cancel : .export   // Cancel is on the left
            case .details: break
            }
        case .confirm:
            switch focus {
            case .details: showDetails = true
            case .cancel: dismiss()
            case .export, .range, .quality: if frames != nil { chooseAndStart() }
            }
        default: break
        }
    }

    /// The current setting's level; Standard when the setting is off but the export option is turned on.
    private var exportFlashLevel: FlashLevel { model.flashLevel == .off ? .standard : model.flashLevel }

    private func chooseAndStart() {
        guard let fr = frames else { return }
        var s = settings
        s.flashReduction = applyFlash ? exportFlashLevel : .off
        s.crt = applyCRT ? CRTSettingsModel.shared.settings : nil
        s.cropTop = cropOverscan ? 8 : 0
        s.cropBottom = cropOverscan ? 8 : 0
        s.cropLeft = cropOverscan ? 8 : 0
        s.cropRight = cropOverscan ? 8 : 0
        s.startFrame = fr.start
        s.endFrame = fr.end
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
        dismiss()   // the status label shows the progress; a toast says when it is saved
    }
}

/// One choice of several as chips (click, or left / right with the row focused).
struct ChoiceChips: View {
    let labels: [String]
    let selected: Int
    let pick: (Int) -> Void

    var body: some View {
        FlowLayout(spacing: 6) {
            ForEach(labels.indices, id: \.self) { i in
                let on = i == selected
                Text(verbatim: labels[i])
                    .font(.callout)
                    .lineLimit(1)
                    .fixedSize()
                    .padding(.horizontal, 12).padding(.vertical, 5)
                    .frame(minWidth: 64)
                    .background(Capsule().fill(on ? Color.accentColor : Color.secondary.opacity(0.16)))
                    .foregroundStyle(on ? Color.white : Color.primary)
                    .contentShape(Capsule())
                    .onTapGesture { pick(i) }
                    .accessibilityAddTraits(on ? [.isButton, .isSelected] : .isButton)
            }
        }
    }
}

/// Left to right, wrapping to the next line (the Range row with many sections).
struct FlowLayout: Layout {
    var spacing: CGFloat = 6

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let rows = arrange(width: proposal.width ?? .infinity, subviews)
        let w = rows.map { $0.width }.max() ?? 0
        let h = rows.reduce(0) { $0 + $1.height } + spacing * CGFloat(max(0, rows.count - 1))
        return CGSize(width: w, height: h)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var y = bounds.minY
        for row in arrange(width: bounds.width, subviews) {
            var x = bounds.minX
            for i in row.items {
                let s = subviews[i].sizeThatFits(.unspecified)
                subviews[i].place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(s))
                x += s.width + spacing
            }
            y += row.height + spacing
        }
    }

    private func arrange(width: CGFloat, _ subviews: Subviews) -> [(items: [Int], width: CGFloat, height: CGFloat)] {
        var rows: [(items: [Int], width: CGFloat, height: CGFloat)] = []
        var cur: (items: [Int], width: CGFloat, height: CGFloat) = ([], 0, 0)
        for i in subviews.indices {
            let s = subviews[i].sizeThatFits(.unspecified)
            let add = cur.items.isEmpty ? s.width : cur.width + spacing + s.width
            if add > width && !cur.items.isEmpty {
                rows.append(cur)
                cur = ([i], s.width, s.height)
            } else {
                cur = (cur.items + [i], add, max(cur.height, s.height))
            }
        }
        if !cur.items.isEmpty { rows.append(cur) }
        return rows
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
                    Label("Video saved", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    Text("\(r.frames) frames · \(String(format: "%.2f", r.duration)) s · \(r.audioSamples) audio samples")
                        .font(.caption)
                    Text(String(format: String(localized: "Replay hash %016llx"), r.rendererHash)).font(.caption.monospaced()).foregroundStyle(.secondary)
                    Text(verbatim: r.encoder).font(.caption).foregroundStyle(.secondary)
                    HStack {
                        Button("Open") { NSWorkspace.shared.open(r.url) }
                        ShareLink(item: r.url) { Text("Share") }
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
