// The VTR transport's small pieces over the game (docs/design/VTR_UX.md): the bottom-left status
// label (non-default states only: Playback, Practice, Rewind, Fast-forward, Slow, End of tape, the
// video being made), the "↶ Undo" chip for 10 s after a retry, the "▶ Make Video" chip at the end
// of the tape and the toast with buttons ("Video saved" · Open · Share).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The status label; nothing when there is nothing to say (normal play). Practicing, it is the
/// "■ Stop Practice" button (click / tap; Y on the pad), then the rest of the label.
struct TapeStatusLabel: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        if model.status.hasSession && model.status.practicing {
            HStack(spacing: 8) {
                TapeChip(title: String(localized: "■ Stop Practice")) { model.practiceStop() }
                let rest = model.status.tapeLabelBesidePractice
                if !rest.isEmpty { TapeLabelText(text: rest) }
            }
        } else {
            label
        }
    }

    @ViewBuilder private var label: some View {
        let base = model.status.hasSession ? model.status.tapeLabel : ""
        if let job = model.exportJob, !job.finished {
            ExportProgressLabel(job: job, base: base)
        } else if !base.isEmpty {
            TapeLabelText(text: base)
        }
    }
}

/// "… · Making video… 42%" while an export runs.
private struct ExportProgressLabel: View {
    @ObservedObject var job: ExportJob
    let base: String

    var body: some View {
        let pct = job.total == 0 ? 0 : Int(Double(job.done) * 100 / Double(job.total))
        let making = String(localized: "Making video… \(pct)%")
        TapeLabelText(text: base.isEmpty ? making : base + " · " + making)
    }
}

struct TapeLabelText: View {
    let text: String
    var body: some View {
        Text(verbatim: text)
            .font(.system(size: 12, weight: .semibold))
            .foregroundStyle(.white.opacity(0.92))
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, 9).padding(.vertical, 4)
            .background(Capsule().fill(Color(red: 0.08, green: 0.08, blue: 0.09).opacity(0.72)))
            .allowsHitTesting(false)
    }
}

/// A small clickable capsule (the Undo / Make Video chips).
struct TapeChip: View {
    let title: String
    var glyph: String?
    let action: () -> Void
    @State private var hovered = false

    var body: some View {
        HStack(spacing: 6) {
            Text(verbatim: title).font(.system(size: 13, weight: .semibold))
            if let glyph { KeyCap(glyph) }
        }
        .foregroundStyle(Color.white.opacity(0.95))
        .padding(.leading, 11).padding(.trailing, glyph == nil ? 11 : 5)
        .frame(height: 28)
        .background(Capsule().fill(Color(red: 0.08, green: 0.08, blue: 0.09).opacity(hovered ? 0.92 : 0.78)))
        .overlay(Capsule().stroke(Color.white.opacity(hovered ? 0.4 : 0.2), lineWidth: 1))
        .fixedSize()
        .contentShape(Capsule())
        .onHover { hovered = $0 }
        .onTapGesture(perform: action)
        .accessibilityAddTraits(.isButton)
        .environment(\.colorScheme, .dark)
    }
}

/// "↶ Undo": back to before the last retry. With a controller it shows the button bound to
/// hk.undo_take (L3 by default), else the key.
struct UndoChip: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject private var monitor = AppModel.shared.input.controllerMonitor

    var body: some View {
        _ = monitor.controllers
        return TapeChip(title: String(localized: "↶ Undo"), glyph: model.controlName(for: "hk.undo_take")) { model.undoRetry() }
    }
}

/// "▶ Make Video" (paused at the end of the tape): opens the Make Video dialog.
struct MakeVideoChip: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        TapeChip(title: String(localized: "▶ Make Video")) { model.showExport = true }
    }
}

/// A toast with buttons (AppModel.flashActions).
struct ActionToastView: View {
    let toast: ActionToast
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 14) {
            Text(verbatim: toast.text).font(.callout)
            ForEach(toast.actions) { a in
                if let url = a.share {
                    ShareLink(item: url) { Text(verbatim: a.label) }
                } else {
                    Button(a.label) { a.run(); model.actionToast = nil }
                }
            }
        }
        .buttonStyle(.link)
        .padding(.horizontal, 16).padding(.vertical, 9)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 18))
        .environment(\.colorScheme, .dark)
    }
}
