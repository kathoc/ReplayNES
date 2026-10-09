// Paused state (R): only the filmstrip seek bar with the time, plus the menu pill
// (docs/design/UI_REDESIGN.md, "Seek bar while paused"). The cancel button resumes, L / R and the
// D-pad step, L2 / R2 rewind / fast-forward; the A/B lane stays (FilmstripTimeline.swift) with the
// selected slot's markers (SeekMarkers.swift). A hint row lists the buttons of the markers' state.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SeekBarOverlay: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject private var markers = AppModel.shared.markers
    @ObservedObject private var monitor = AppModel.shared.input.controllerMonitor
    @ObservedObject private var strip = FilmstripModel.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            bar
            hints
        }
        .padding(.horizontal, 14).padding(.vertical, 10)
        .background(RoundedRectangle(cornerRadius: QMStyle.radius).fill(QMStyle.panel))
        .overlay(RoundedRectangle(cornerRadius: QMStyle.radius).stroke(Color.white.opacity(0.08), lineWidth: 1))
        .foregroundStyle(.white)
        .environment(\.colorScheme, .dark)
        .padding(.horizontal, QMStyle.margin).padding(.bottom, 16)
    }

    private var bar: some View {
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
    }

    /// The buttons of the state: the bar (cancel Resume · L R Step · confirm Marker · ↑ Markers ·
    /// Y Practice · X A/B n), a marker (←→ Select · confirm Move · X Delete · cancel Back) or editing one
    /// (←→ ±1 · L R Move · confirm Done · cancel Undo). Keyboard only: Space resumes.
    private var hints: some View {
        let g = HintGlyphs.current(monitor.controllers)
        var items: [([PadGlyph.Kind], String)] = []
        if g.controller {
            let on = markers.active
            if on && markers.editing {
                items = [([.dpadLR], "±1"), ([.key(g.l), .key(g.r)], String(localized: "Move")),
                         ([.key(g.confirm)], String(localized: "Done")), ([.key(g.back)], String(localized: "Undo"))]
            } else if on && markers.focus >= 0 {
                items = [([.dpadLR], "A ⇄ B"), ([.key(g.confirm)], String(localized: "Move")),
                         ([.key(g.x)], String(localized: "Delete")), ([.key(g.back)], String(localized: "Back"))]
            } else {
                items = [([.key(g.back)], String(localized: "Resume")), ([.key(g.l), .key(g.r)], String(localized: "Step"))]
                if on && markers.frames.count < 2 { items.append(([.key(g.confirm)], String(localized: "Marker"))) }
                if on && !markers.frames.isEmpty { items.append(([.dpadUp], String(localized: "Markers"))) }
                if on && !markers.frames.isEmpty { items.append(([.key(g.y)], String(localized: "Practice"))) }
                if on { items.append(([.key(g.x)], "A/B \(min(7, max(0, strip.selectedSlot)) + 1)")) }
            }
        } else {
            items = [([.key(String(localized: "Space"))], String(localized: "Resume"))]
        }
        return HStack(spacing: 18) {
            ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                HStack(spacing: 4) {
                    ForEach(Array(item.0.enumerated()), id: \.offset) { _, k in PadGlyph(kind: k) }
                    Text(verbatim: item.1).font(QMStyle.hint).foregroundStyle(.white.opacity(0.75)).fixedSize().padding(.leading, 2)
                }
            }
            Spacer(minLength: 8)
            if markers.active, let h = markers.hint {
                Text(verbatim: h).font(QMStyle.hint).foregroundStyle(.white.opacity(0.6)).lineLimit(1)
            }
        }
        .frame(height: 20)
    }

    private func rightTime(_ st: EmuStatus) -> String {
        if st.practicing { return st.practiceLength > 0 ? Engine.timecode(forFrame: st.practiceLength) : "--:--.--" }
        return Engine.timecode(forFrame: st.takeLength)
    }
}

/// A button in a hint row: a key cap with its label, or the D-pad with the arms that act lit.
struct PadGlyph: View {
    enum Kind { case key(String), dpadLR, dpadUp }
    let kind: Kind
    var body: some View {
        switch kind {
        case .key(let t): KeyCap(t)
        case .dpadLR, .dpadUp:
            let lr: Bool = { if case .dpadLR = kind { return true } else { return false } }()
            Canvas { ctx, size in
                let a = size.width / 3
                let arms: [(CGRect, Bool)] = [(CGRect(x: a, y: 0, width: a, height: a), !lr),       // up
                                              (CGRect(x: a, y: 2 * a, width: a, height: a), false), // down
                                              (CGRect(x: 0, y: a, width: a, height: a), lr),        // left
                                              (CGRect(x: 2 * a, y: a, width: a, height: a), lr)]    // right
                for (r, on) in arms {
                    ctx.fill(Path(roundedRect: r, cornerRadius: 1.5), with: .color(on ? QMStyle.brand : Color.white.opacity(0.35)))
                }
                ctx.fill(Path(CGRect(x: a, y: a, width: a, height: a)), with: .color(Color.white.opacity(0.35)))
            }
            .frame(width: 18, height: 18)
            .padding(1)
        }
    }
}

/// Content made from the per-frame status: observes AppModel.clock only (see AppModel.status).
struct ClockView<Content: View>: View {
    @ObservedObject private var clock = AppModel.shared.clock
    @ViewBuilder let content: (EmuStatus) -> Content
    var body: some View { content(clock.status) }
}
