// Paused state (R): only the filmstrip seek bar with the time, plus the menu pill
// (docs/design/UI_REDESIGN.md, "Seek bar while paused"). L2 / R2 scrub, A resumes, the A/B lane
// stays (FilmstripTimeline.swift).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SeekBarOverlay: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 12) {
            ClockView { st in
                Text(verbatim: st.practicing ? Engine.timecode(forFrame: st.practiceFrame) : Engine.timecode(forFrame: st.frame))
                    .font(.system(size: 13, weight: .semibold, design: .monospaced))
            }
            FilmstripTimeline()
            ClockView { st in
                Text(verbatim: rightTime(st)).font(.system(size: 13, design: .monospaced)).foregroundStyle(.white.opacity(0.6))
            }
            TimelineSlotPicker()
        }
        .padding(.horizontal, 14).padding(.vertical, 10)
        .background(RoundedRectangle(cornerRadius: QMStyle.radius).fill(QMStyle.panel))
        .overlay(RoundedRectangle(cornerRadius: QMStyle.radius).stroke(Color.white.opacity(0.08), lineWidth: 1))
        .foregroundStyle(.white)
        .environment(\.colorScheme, .dark)
        .padding(.horizontal, QMStyle.margin).padding(.bottom, 16)
    }

    private func rightTime(_ st: EmuStatus) -> String {
        if st.practicing { return st.practiceLength > 0 ? Engine.timecode(forFrame: st.practiceLength) : "--:--.--" }
        return Engine.timecode(forFrame: st.takeLength)
    }
}

/// Content made from the per-frame status: observes AppModel.clock only (see AppModel.status).
struct ClockView<Content: View>: View {
    @ObservedObject private var clock = AppModel.shared.clock
    @ViewBuilder let content: (EmuStatus) -> Content
    var body: some View { content(clock.status) }
}
