// Settings diagram of a game controller: every physical element drawn where it sits on the
// device (own simple shapes, no artwork), its current assignment as a badge, live highlight of
// held buttons, click -> pick an action. Pure view: data in, `onAssign` out (also rendered
// offscreen by the unit tests).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct ControllerDiagramView: View {
    let family: ControllerFamily
    let slot: Int
    let config: InputCatalog.Config
    /// Face-button labels reported by the connected device (element -> "A"), optional.
    var labels: [String: String] = [:]
    /// Controller ids ("gc0:face.east", "gc0:lstick.up"...) held right now.
    var pressed: Set<String> = []
    /// (physical id, action or nil = none). nil = read-only.
    var onAssign: ((String, String?) -> Void)? = nil

    @State private var editing: String?

    private var layout: ControllerDiagramLayout { ControllerDiagramLayout(family: family) }

    var body: some View {
        let l = layout
        ZStack(alignment: .topLeading) {
            ControllerBodyShape(family: family)
                .fill(Color.secondary.opacity(0.13))
            ControllerBodyShape(family: family)
                .stroke(Color.secondary.opacity(0.55), lineWidth: 1.5)
            if let t = l.touchpad {
                RoundedRectangle(cornerRadius: 8).fill(Color.secondary.opacity(0.10))
                    .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.secondary.opacity(0.4)))
                    .frame(width: t.width, height: t.height).position(x: t.midX, y: t.midY)
            }
            // Stick wells and the D-pad hub.
            ForEach([l.leftStick, l.rightStick].indices, id: \.self) { i in
                let s = i == 0 ? l.leftStick : l.rightStick
                Circle().fill(Color.secondary.opacity(0.18))
                    .overlay(Circle().stroke(Color.secondary.opacity(0.5)))
                    .frame(width: s.radius * 2, height: s.radius * 2).position(s.center)
            }
            Rectangle().fill(Color.secondary.opacity(0.35))
                .frame(width: ControllerDiagramLayout.dpadArm, height: ControllerDiagramLayout.dpadArm)
                .position(l.dpadCenter)
            ForEach(l.elements) { e in elementView(e) }
            badges(l)
        }
        .frame(width: ControllerDiagramLayout.canvas.width, height: ControllerDiagramLayout.canvas.height)
    }

    // MARK: elements

    private func physicalID(_ element: String) -> String { "gc\(slot):\(element)" }

    private func label(_ element: String) -> String { labels[element] ?? family.label(element) }

    @ViewBuilder
    private func elementView(_ e: DiagramElement) -> some View {
        let isDown = pressed.contains(physicalID(e.element))
        let isEditing = editing == e.element
        let assigned = !ControllerAssignments.actions(element: e.element, slot: slot, config: config).isEmpty
        let fill: Color = isDown ? Color.accentColor : (assigned ? Color(nsColor: .controlBackgroundColor) : Color.secondary.opacity(0.08))
        let stroke: Color = isEditing ? Color.accentColor : Color.secondary.opacity(assigned ? 0.75 : 0.4)
        let fg: Color = isDown ? .white : (assigned ? .primary : .secondary)
        Button {
            if onAssign != nil { editing = e.element }
        } label: {
            shape(e, fill: fill, stroke: stroke, lineWidth: isEditing ? 2.5 : 1, fg: fg)
                .frame(width: e.size.width, height: e.size.height)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(helpText(e.element))
        .popover(isPresented: Binding(get: { editing == e.element }, set: { if !$0 { editing = nil } })) {
            AssignmentPicker(title: ControllerAssignments.title(element: e.element, family: family, labels: labels),
                             current: ControllerAssignments.actions(element: e.element, slot: slot, config: config),
                             slot: slot) { action in
                onAssign?(physicalID(e.element), action)
                editing = nil
            }
        }
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
                .foregroundStyle(fill == Color.accentColor ? Color.accentColor : fg.opacity(0.85))
                .padding(1)
                .background(Circle().fill(fill == Color.accentColor ? Color.accentColor.opacity(0.25) : Color.clear))
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

/// The controller silhouette: a rounded body plus two grips.
struct ControllerBodyShape: Shape {
    let family: ControllerFamily

    func path(in rect: CGRect) -> Path {
        let body = Path(roundedRect: CGRect(x: 92, y: 70, width: 376, height: 132), cornerRadius: 50)
        let lx: CGFloat = family.isSymmetric ? 175 : 168
        let left = Path(ellipseIn: CGRect(x: lx - 64, y: 132, width: 128, height: 120))
        let right = Path(ellipseIn: CGRect(x: 560 - lx - 64, y: 132, width: 128, height: 120))
        return body.union(left).union(right)
    }
}

/// Popover list: "None" + every action, grouped; the current one(s) checked.
struct AssignmentPicker: View {
    let title: String
    let current: [String]
    let slot: Int
    let pick: (String?) -> Void

    var body: some View {
        let groups: [InputAction.Group] = slot == 1 ? [.player2, .player1, .hotkey] : [.player1, .player2, .hotkey]
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.headline)
            Text("Action for this button").font(.caption).foregroundStyle(.secondary)
            ScrollView {
                VStack(alignment: .leading, spacing: 1) {
                    row(String(localized: "None"), checked: current.isEmpty) { pick(nil) }
                    ForEach(groups, id: \.self) { g in
                        Text(g.title).font(.caption.weight(.semibold)).foregroundStyle(.secondary).padding(.top, 6)
                        ForEach(InputCatalog.allActions.filter { $0.group == g }) { a in
                            row(a.label, checked: current.contains(a.id)) { pick(a.id) }
                        }
                    }
                }
            }
            .frame(width: 300, height: 340)
            if current.count > 1 {
                Text("Choosing one makes it the only action assigned to this button.").font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(12)
    }

    private func row(_ text: String, checked: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack {
                Image(systemName: "checkmark").opacity(checked ? 1 : 0).frame(width: 14)
                Text(text)
                Spacer()
            }
            .contentShape(Rectangle())
            .padding(.vertical, 3).padding(.horizontal, 4)
        }
        .buttonStyle(.plain)
    }
}
