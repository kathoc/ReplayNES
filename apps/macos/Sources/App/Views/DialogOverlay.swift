// The in-window dialog (Dialogs.swift) in the Quick Menu's visual language: over everything in
// the window, the rest dimmed; a dark panel with a title, a short message, an optional on/off row
// or text field, a row of button tiles with the focus ring and the hint bar of the connected
// controller. Fixed layout: nothing scrolls (long messages are clipped to a few lines).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct DialogOverlay: View {
    @ObservedObject var center: DialogCenter
    @ObservedObject var monitor: ControllerMonitor
    @FocusState private var fieldFocused: Bool

    var body: some View {
        GeometryReader { geo in
            if let d = center.current {
                ZStack {
                    // Takes every click: the pill, the menu and the library behind are inert.
                    Color.black.opacity(0.55)
                        .contentShape(Rectangle())
                        .onTapGesture {}
                    panel(d, width: max(320, min(geo.size.width - 2 * QMStyle.margin, 560)))
                        .transition(.opacity.combined(with: .scale(scale: 0.97)))
                }
                .frame(width: geo.size.width, height: geo.size.height)
            }
        }
        .environment(\.colorScheme, .dark)
    }

    private func panel(_ d: AppDialog, width: CGFloat) -> some View {
        let nav = center.nav
        return VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 10) {
                Image(systemName: icon(d.kind)).font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(d.kind == .warning ? QMStyle.brand : Color.white)
                Text(d.title).font(QMStyle.title).lineLimit(2).minimumScaleFactor(0.8)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if !d.message.isEmpty {
                Text(d.message).font(.system(size: 14)).foregroundStyle(.white.opacity(0.75))
                    .lineLimit(7).minimumScaleFactor(0.8)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let t = d.toggle {
                HStack(spacing: 12) {
                    Text(t).font(QMStyle.label).lineLimit(1).minimumScaleFactor(0.8)
                    Spacer(minLength: 12)
                    Switch(on: nav.toggleOn)
                }
                .padding(.horizontal, 14)
                .frame(height: QMStyle.rowHeight)
                .modifier(FocusFrame(focused: nav.focus == DialogNav.toggleRow, enabled: true, radius: 10))
                .contentShape(Rectangle())
                .onHover { if $0 { center.hover(DialogNav.toggleRow) } }
                .onTapGesture { center.hover(DialogNav.toggleRow); center.flipToggle() }
            }
            if d.text != nil {
                TextField("", text: $center.text)
                    .textFieldStyle(.plain)
                    .font(.system(size: 15))
                    .padding(.horizontal, 12).frame(height: 36)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Color.black.opacity(0.35)))
                    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.white.opacity(0.2), lineWidth: 1))
                    .focused($fieldFocused)
                    .onSubmit { center.submit() }
                    .onExitCommand { center.cancel() }
                    .onAppear { fieldFocused = true }
                    .onChange(of: center.serial) { _, _ in fieldFocused = true }
            }
            HStack(spacing: QMStyle.gap) {
                ForEach(Array(d.buttons.enumerated()), id: \.offset) { i, title in
                    Text(title).font(QMStyle.label).lineLimit(1).minimumScaleFactor(0.75)
                        .foregroundStyle(i == d.destructiveIndex ? QMStyle.brand : Color.white)
                        .padding(.horizontal, 10)
                        .frame(maxWidth: .infinity, minHeight: 44)
                        .modifier(FocusFrame(focused: nav.focus == i, enabled: true, radius: 10, raise: 1.02))
                        .contentShape(Rectangle())
                        .onHover { if $0 { center.hover(i) } }
                        .onTapGesture { center.hover(i); center.answer(i) }
                }
            }
            footer(d, nav)
        }
        .padding(QMStyle.margin)
        .frame(width: width, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: QMStyle.radius).fill(QMStyle.panel))
        .overlay(RoundedRectangle(cornerRadius: QMStyle.radius).stroke(Color.white.opacity(0.08), lineWidth: 1))
        .shadow(color: .black.opacity(0.5), radius: 24, y: 8)
        .foregroundStyle(.white)
    }

    private func footer(_ d: AppDialog, _ nav: DialogNav) -> some View {
        let g = HintGlyphs.current(monitor.controllers)
        var hints: [(String, String, () -> Void)] = []
        let confirmVerb = nav.focus == DialogNav.toggleRow ? String(localized: "Change")
            : d.buttons.count == 1 ? d.buttons[0] : String(localized: "Select")
        hints.append((g.confirm, confirmVerb, { center.handle(.confirm) }))
        if d.buttons.count > 1, d.buttons.indices.contains(nav.cancelIndex) {
            hints.append((g.controller ? g.back : "esc", d.buttons[nav.cancelIndex], { center.cancel() }))
        }
        return HStack(spacing: 14) {
            Spacer(minLength: 0)
            ForEach(Array(hints.enumerated()), id: \.offset) { _, h in
                HStack(spacing: 5) {
                    KeyCap(h.0)
                    Text(h.1).font(QMStyle.hint).foregroundStyle(.white.opacity(0.8)).lineLimit(1).fixedSize()
                }
                .contentShape(Rectangle())
                .onTapGesture(perform: h.2)
            }
        }
        .frame(height: 22)
    }

    private func icon(_ k: AppDialog.Kind) -> String {
        switch k {
        case .info: return "info.circle"
        case .warning: return "exclamationmark.triangle"
        case .question: return "questionmark.circle"
        }
    }
}
