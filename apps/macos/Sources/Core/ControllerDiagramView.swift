// Settings diagram of a game controller: every physical element drawn where it sits on the
// device (own simple shapes, no artwork) over the shared core's outline (rnf_diagram_decor: no
// grips; the Steam Deck its own), its current assignment as a badge, live highlight of held
// buttons, the controller focus ring; a click selects an element (the Quick Menu opens its action
// picker page). Pure view: data in, `onSelect` out (also rendered offscreen by the unit tests).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct ControllerDiagramView: View {
    let family: ControllerFamily
    let slot: Int
    let config: InputCatalog.Config
    /// Face-button labels reported by the device (element -> "A"), optional.
    var labels: [String: String] = [:]
    /// Controller ids ("gc0:face.east", "gc0:lstick.up"...) held right now.
    var pressed: Set<String> = []
    /// The element with the controller focus (D-pad / left stick on the page): a focus ring.
    var focused: String? = nil
    /// A click on an element (its name, e.g. "face.east"): the action picker. nil = read-only.
    var onSelect: ((String) -> Void)? = nil

    static let brand = Color(red: 1, green: 0x3B / 255, blue: 0x3B / 255)

    private var layout: ControllerDiagramLayout { ControllerDiagramLayout(family: family) }

    var body: some View {
        let l = layout
        ZStack(alignment: .topLeading) {
            // The outline (rnf_diagram_decor): a rounded body without grips, the Deck's screen, pads.
            ForEach(Array(l.decor.enumerated()), id: \.offset) { _, d in decorView(d) }
            // Stick wells and the D-pad hub.
            ForEach([l.leftStick, l.rightStick].indices, id: \.self) { i in
                let s = i == 0 ? l.leftStick : l.rightStick
                Circle().fill(Color.white.opacity(0.10))
                    .overlay(Circle().stroke(Color.white.opacity(0.35)))
                    .frame(width: s.radius * 2, height: s.radius * 2).position(s.center)
            }
            Rectangle().fill(Color.white.opacity(0.22))
                .frame(width: ControllerDiagramLayout.dpadArm, height: ControllerDiagramLayout.dpadArm)
                .position(l.dpadCenter)
            ForEach(l.elements) { e in elementView(e) }
            badges(l)
            if let f = focused, let e = l.element(f) { focusRing(e) }
        }
        .frame(width: ControllerDiagramLayout.canvas.width, height: ControllerDiagramLayout.canvas.height)
    }

    @ViewBuilder
    private func decorView(_ d: ControllerDiagramLayout.Decor) -> some View {
        let shape = RoundedRectangle(cornerRadius: d.radius, style: .circular)
        switch d.kind {
        case .body:
            shape.fill(LinearGradient(colors: [Color(red: 0.20, green: 0.21, blue: 0.25), Color(red: 0.15, green: 0.16, blue: 0.19)],
                                      startPoint: .top, endPoint: .bottom))
                .overlay(shape.stroke(Color.white.opacity(0.30), lineWidth: 1.5))
                .frame(width: d.rect.width, height: d.rect.height).position(x: d.rect.midX, y: d.rect.midY)
        case .screen:
            shape.fill(Color(red: 0.05, green: 0.06, blue: 0.07))
                .overlay(shape.stroke(Color.white.opacity(0.16), lineWidth: 1))
                .frame(width: d.rect.width, height: d.rect.height).position(x: d.rect.midX, y: d.rect.midY)
        case .pad:
            shape.fill(Color.white.opacity(0.08))
                .overlay(shape.stroke(Color.white.opacity(0.22), lineWidth: 1))
                .frame(width: d.rect.width, height: d.rect.height).position(x: d.rect.midX, y: d.rect.midY)
        }
    }

    private func focusRing(_ e: DiagramElement) -> some View {
        let round = e.kind == .face || e.kind == .home || e.kind == .stickClick || e.kind == .stickDirection
        let w = e.size.width + 8, h = e.size.height + 8
        return Group {
            if round {
                Capsule().stroke(Self.brand, lineWidth: 2.5).frame(width: max(w, h), height: max(w, h))
            } else {
                RoundedRectangle(cornerRadius: 9).stroke(Self.brand, lineWidth: 2.5).frame(width: w, height: h)
            }
        }
        .shadow(color: Self.brand.opacity(0.6), radius: 4)
        .position(e.center)
        .allowsHitTesting(false)
    }

    // MARK: elements

    private func physicalID(_ element: String) -> String { "gc\(slot):\(element)" }

    private func label(_ element: String) -> String { labels[element] ?? family.label(element) }

    @ViewBuilder
    private func elementView(_ e: DiagramElement) -> some View {
        let isDown = pressed.contains(physicalID(e.element))
        let isFocused = focused == e.element
        let assigned = !ControllerAssignments.actions(element: e.element, slot: slot, config: config).isEmpty
        let fill: Color = isDown ? Self.brand : (assigned ? Color.white.opacity(0.16) : Color.white.opacity(0.05))
        let stroke: Color = Color.white.opacity(assigned ? 0.6 : 0.3)
        let fg: Color = isDown ? .white : (assigned ? Color.white : Color.white.opacity(0.55))
        shape(e, fill: fill, stroke: stroke, lineWidth: 1, fg: fg)
            .frame(width: e.size.width, height: e.size.height)
            .contentShape(Rectangle())
            .scaleEffect(isFocused ? 1.08 : 1)
            .onTapGesture { onSelect?(e.element) }
            .help(helpText(e.element))
            .position(e.center)
    }

    @ViewBuilder
    private func shape(_ e: DiagramElement, fill: Color, stroke: Color, lineWidth: CGFloat, fg: Color) -> some View {
        switch e.kind {
        case .face, .home, .stickClick:
            Circle().fill(fill).overlay(Circle().stroke(stroke, lineWidth: lineWidth))
                .overlay(Text(label(e.element)).font(.system(size: e.kind == .face ? 12 : 7, weight: .bold))
                    .minimumScaleFactor(0.5).lineLimit(1).padding(1).foregroundStyle(fg))
        case .dpad:
            Rectangle().fill(fill).overlay(Rectangle().stroke(stroke, lineWidth: lineWidth))
                .overlay(arrow(e.element).font(.system(size: 8)).foregroundStyle(fg))
        case .stickDirection:
            arrow(e.element).font(.system(size: 10, weight: .bold))
                .foregroundStyle(fill == Self.brand ? Self.brand : fg.opacity(0.85))
                .padding(1)
                .background(Circle().fill(fill == Self.brand ? Self.brand.opacity(0.25) : Color.clear))
                .overlay(Circle().stroke(lineWidth == 1 ? Color.clear : stroke, lineWidth: lineWidth))
        case .shoulder, .trigger, .small:
            RoundedRectangle(cornerRadius: e.kind == .trigger ? 9 : 7).fill(fill)
                .overlay(RoundedRectangle(cornerRadius: e.kind == .trigger ? 9 : 7).stroke(stroke, lineWidth: lineWidth))
                .overlay(Text(label(e.element)).font(.system(size: e.kind == .small ? 8 : 11, weight: .semibold))
                    .minimumScaleFactor(0.6).lineLimit(1).foregroundStyle(fg))
        }
    }

    private func helpText(_ element: String) -> String {
        let labels = ControllerAssignments.actions(element: element, slot: slot, config: config).map { id in
            InputCatalog.allActions.first { $0.id == id }?.label ?? id
        }
        return ControllerAssignments.title(element: element, family: family, labels: self.labels) + ": "
            + (labels.isEmpty ? String(localized: "None") : labels.joined(separator: " / "))
    }

    private func arrow(_ element: String) -> Image {
        let dir = element.split(separator: ".").last.map(String.init) ?? "up"
        return Image(systemName: "arrowtriangle.\(dir).fill")
    }

    // MARK: badges

    @ViewBuilder
    private func badges(_ l: ControllerDiagramLayout) -> some View {
        let groups = Dictionary(uniqueKeysWithValues: ["dpad", "lstick", "rstick"].map {
            ($0, ControllerAssignments.group($0, slot: slot, config: config))
        })
        ForEach(l.elements) { e in
            if e.group.map({ groups[$0] == .custom }) ?? true,
               let text = ControllerAssignments.badge(element: e.element, slot: slot, config: config) {
                Badge(text: text, hotkey: isHotkey(e.element), side: e.badgeSide).position(e.badgeAnchor)
            }
        }
        ForEach(["dpad", "lstick", "rstick"], id: \.self) { g in
            if case .movement(let text) = groups[g], let p = l.groupAnchors[g] {
                Badge(text: text, hotkey: false, side: .below).position(p)
            }
        }
    }

    private func isHotkey(_ element: String) -> Bool {
        ControllerAssignments.actions(element: element, slot: slot, config: config).contains { $0.hasPrefix("hk.") }
    }
}

/// Assignment badge anchored at its position: it grows away from the element on `side`.
private struct Badge: View {
    let text: String
    let hotkey: Bool
    let side: DiagramElement.Side

    var body: some View {
        let align: Alignment = switch side {
        case .left: .trailing
        case .right: .leading
        case .above: .bottom
        case .below: .top
        }
        Color.clear.frame(width: 0, height: 0)
            .overlay(alignment: align) {
                Text(text)
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 5).padding(.vertical, 1.5)
                    .background(Capsule().fill(hotkey ? Color.orange : Color.blue.opacity(0.85)))
                    .fixedSize()
            }
    }
}
