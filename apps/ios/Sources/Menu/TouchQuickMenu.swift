// The Quick Menu on iPhone: the same pages, items and controller navigation as macOS (shared
// QuickMenuController + Core/QuickMenuModel.swift; items: QuickMenuPages.swift), drawn for touch:
// large tap targets, a back button and breadcrumb, lists that may scroll on a small screen, X / Y
// actions as buttons on the item, the focused item's one-line description at the bottom. The
// controller focus (D-pad) is shown as a ring, so a pad and touch can be mixed freely.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct TouchQuickMenu: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var menu: QuickMenuController
    @ObservedObject var monitor: ControllerMonitor
    @ObservedObject private var crt = CRTSettingsModel.shared
    @ObservedObject private var entitlements = Entitlements.shared

    var body: some View {
        GeometryReader { geo in
            ZStack {
                Color.black.opacity(0.62).contentShape(Rectangle()).onTapGesture { menu.close() }
                panel(geo)
            }
        }
        .environment(\.colorScheme, .dark)
        .foregroundStyle(.white)
    }

    private func panel(_ geo: GeometryProxy) -> some View {
        _ = menu.revision
        let p = menu.page
        let content = QuickMenuPages.content(p, model: model, menu: menu)
        let focus = min(menu.focusIndex(p), max(0, content.items.count - 1))
        let landscape = geo.size.width > geo.size.height
        let width = min(geo.size.width - 24, landscape ? 720 : 520)
        return VStack(alignment: .leading, spacing: 12) {
            header(p, content)
            if QMPage.settingsTabs.contains(p) { settingsTabs(p) }
            // The upper half (a FlipPad over the lower half) is short: the panel may take more of it.
            let maxBody = geo.size.height * (model.upperHalfLayout ? 0.74 : landscape ? 0.62 : 0.66)
            ScrollViewReader { proxy in
                ScrollView(.vertical, showsIndicators: p.scrolls) {
                    pageBody(p, content, focus: focus, width: width - 28, landscape: landscape)
                        .padding(.bottom, 4)
                }
                .frame(maxHeight: p.scrolls ? Self.peekHeight(rows: content.items.count, max: maxBody) : maxBody)
                .fixedSize(horizontal: false, vertical: true)
                // The D-pad focus is scrolled into view on every page (a scrolling list, and any page taller
                // than the panel: the upper half with a FlipPad); a scrolling list opens at it.
                .onChange(of: focus) { _, f in
                    guard let id = focusID(p, content, f) else { return }
                    withAnimation(.easeOut(duration: 0.12)) { proxy.scrollTo(id) }
                }
                .onAppear {
                    guard p.scrolls, content.items.indices.contains(focus) else { return }
                    proxy.scrollTo(content.items[focus].id, anchor: .center)
                }
            }
            footer(content, focus: focus)
        }
        .padding(14)
        .frame(width: width)
        .background(RoundedRectangle(cornerRadius: 18).fill(Color(red: 0.08, green: 0.08, blue: 0.09).opacity(0.95)))
        .overlay(RoundedRectangle(cornerRadius: 18).stroke(Color.white.opacity(0.08), lineWidth: 1))
        .contentShape(Rectangle())
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("quickMenu")
        .position(x: geo.size.width / 2, y: geo.size.height / 2)
    }

    /// The view id of the focused item (rows, grid cells, the controller page's rows; the diagram).
    private func focusID(_ p: QMPage, _ content: QMContent, _ f: Int) -> String? {
        if p == .controller {
            let n = menu.diagramElements(model).count
            let rows = QuickMenuPages.controllerRows(model, slot: menu.listPageIndex(.controller))
            return f >= n && rows.indices.contains(f - n) ? rows[f - n].id : "controllerDiagram"
        }
        return content.items.indices.contains(f) ? content.items[f].id : nil
    }

    /// Row pitch of the rows layout (row(): minHeight 48, VStack spacing 6).
    static let rowPitch: CGFloat = 54

    /// A scrolling list's height: whole rows plus half of the next one (it shows the list goes on),
    /// within `max`; all rows when they fit.
    static func peekHeight(rows: Int, max: CGFloat) -> CGFloat {
        let all = CGFloat(rows) * rowPitch - 6 + 4
        if all <= max { return all }
        let whole = Swift.max(1, Int((max - rowPitch / 2) / rowPitch))
        return CGFloat(whole) * rowPitch + rowPitch / 2 - 6
    }

    // MARK: header / footer

    private func header(_ p: QMPage, _ content: QMContent) -> some View {
        HStack(spacing: 8) {
            if p != .top {
                Button { menu.back() } label: {
                    Image(systemName: "chevron.left").font(.system(size: 17, weight: .semibold)).frame(width: 36, height: 36)
                        .background(Color.white.opacity(0.1), in: Circle())
                        .touchTarget(width: 36, height: 36)
                }
                .accessibilityLabel(String(localized: "Back"))
                .accessibilityIdentifier("qmBack")
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(p == .top ? String(localized: "Menu") : p.iosTitle).font(.system(size: 20, weight: .semibold)).lineLimit(1)
                    .accessibilityIdentifier("qmTitle")
                if p.depth > 1 || content.caption != nil {
                    Text(content.caption ?? p.path.dropFirst().dropLast().map(\.iosTitle).joined(separator: " › "))
                        .font(.system(size: 12)).foregroundStyle(.white.opacity(0.55)).lineLimit(1)
                }
            }
            Spacer(minLength: 6)
            if p.paged && content.pageCount > 1 {
                pagerButton("chevron.left") { flip(p, content, -1) }
                pagerButton("chevron.right") { flip(p, content, 1) }
            }
            if let x = content.x { smallButton(x.label) { x.run(); menu.touch() } }
            if let y = content.y { smallButton(y.label) { y.run(); menu.touch() } }
            Button { menu.close() } label: {
                Image(systemName: "xmark").font(.system(size: 15, weight: .bold)).frame(width: 36, height: 36)
                    .background(Color.white.opacity(0.1), in: Circle())
                    .touchTarget(width: 36, height: 36)
            }
            .accessibilityLabel(String(localized: "Close"))
            .accessibilityIdentifier("qmClose")
        }
    }

    /// Display / Controls / Sound / System (L / R on a controller).
    private func settingsTabs(_ p: QMPage) -> some View {
        HStack(spacing: 4) {
            ForEach(QMPage.settingsTabs, id: \.self) { t in
                Text(t.iosTitle).font(.system(size: 13, weight: t == p ? .semibold : .regular)).lineLimit(1).minimumScaleFactor(0.8)
                    .frame(maxWidth: .infinity, minHeight: 32)
                    .background(RoundedRectangle(cornerRadius: 8).fill(t == p ? Color.white.opacity(0.22) : .clear))
                    .touchTarget(height: 32)
                    .onTapGesture { menu.switchTab(to: t) }
                    .accessibilityAddTraits(t == p ? [.isButton, .isSelected] : .isButton)
                    .accessibilityIdentifier("qmTab.\(t.rawValue)")
            }
        }
        .padding(3)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.white.opacity(0.07)))
    }

    private func footer(_ content: QMContent, focus: Int) -> some View {
        let item = content.items.indices.contains(focus) ? content.items[focus] : nil
        return Text(item?.detail ?? " ").font(.system(size: 12)).foregroundStyle(.white.opacity(0.6)).lineLimit(2)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func flip(_ p: QMPage, _ content: QMContent, _ by: Int) {
        let n = content.pageCount
        menu.setListPage((menu.listPageIndex(p) + by + n) % n, on: p)
    }

    // MARK: pages

    @ViewBuilder
    private func pageBody(_ p: QMPage, _ content: QMContent, focus: Int, width: CGFloat, landscape: Bool) -> some View {
        switch p.layout {
        case .tiles:
            grid(content.items, columns: landscape ? 6 : 3, focus: focus, p: p) { item, f in tile(item, focused: f, height: 92) }
        case .actions:
            grid(content.items, columns: landscape ? 3 : 2, focus: focus, p: p) { item, f in tile(item, focused: f, height: 76) }
        case .cards:
            if content.items.isEmpty {
                Text(L("Nothing here yet")).foregroundStyle(.white.opacity(0.5)).frame(maxWidth: .infinity, minHeight: 80)
            } else {
                grid(content.items, columns: landscape ? 4 : 2, focus: focus, p: p) { item, f in card(item, focused: f) }
            }
        case .rows:
            VStack(spacing: 6) {
                ForEach(Array(content.items.enumerated()), id: \.element.id) { i, item in
                    row(item, focused: i == focus && hasPad)
                        .contentShape(Rectangle())
                        .onTapGesture { menu.setFocus(i, on: p); tapRow(item, p) }
                        .accessibilityElement(children: .contain)
                        .accessibilityIdentifier("qm.\(item.id)")
                        .id(item.id)
                }
            }
        case .custom:
            controllerPage(width: width)
        }
    }

    private var hasPad: Bool { !monitor.controllers.isEmpty }

    private func grid<V: View>(_ items: [QMItem], columns: Int, focus: Int, p: QMPage,
                               @ViewBuilder cell: @escaping (QMItem, Bool) -> V) -> some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 8), count: columns), spacing: 8) {
            ForEach(Array(items.enumerated()), id: \.element.id) { i, item in
                cell(item, i == focus && hasPad)
                    .contentShape(Rectangle())
                    .onTapGesture { menu.setFocus(i, on: p); activate(item, p) }
                    .accessibilityElement(children: .contain)
                    .accessibilityAddTraits(.isButton)
                    .accessibilityIdentifier("qm.\(item.id)")
                    .id(item.id)
            }
        }
        // The D-pad moves on this grid (QuickMenuController.drawnColumns), not the Mac's columns.
        .onAppear { menu.drawnColumns[p] = columns }
        .onChange(of: columns) { _, c in menu.drawnColumns[p] = c }
    }

    private func activate(_ item: QMItem, _ p: QMPage) {
        if let f = item.locked, item.page != nil || item.confirm == nil { model.showLocked(f); return }
        menu.activate(item, on: p)
    }

    /// Rows: a tap changes the value where that is natural (toggles, links, actions); choices and
    /// sliders have their own controls.
    private func tapRow(_ item: QMItem, _ p: QMPage) {
        switch item.value {
        case .choice, .slider: return
        default: activate(item, p)
        }
    }

    private func tile(_ item: QMItem, focused: Bool, height: CGFloat) -> some View {
        VStack(spacing: 8) {
            ZStack(alignment: .topTrailing) {
                Image(systemName: item.icon).font(.system(size: 24, weight: .medium)).frame(maxWidth: .infinity)
                if item.locked != nil { lockBadge }
            }
            Text(item.title).font(.system(size: 13, weight: .medium)).lineLimit(1).minimumScaleFactor(0.7)
            if let s = item.subtitle { Text(s).font(.system(size: 11)).foregroundStyle(.white.opacity(0.55)) }
        }
        .padding(8)
        .frame(maxWidth: .infinity, minHeight: height)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color.white.opacity(focused ? 0.16 : 0.07)))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(focused ? Color.white.opacity(0.8) : .clear, lineWidth: 2))
        .opacity(item.enabled ? 1 : 0.38)
    }

    private var lockBadge: some View {
        Image(systemName: "lock.fill").font(.system(size: 11, weight: .bold)).foregroundStyle(.yellow)
    }

    private func card(_ item: QMItem, focused: Bool) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack(alignment: .topLeading) {
                Group {
                    if let img = item.image {
                        Image(decorative: img, scale: 1).resizable().interpolation(.none).aspectRatio(256.0 / 224.0, contentMode: .fit)
                    } else {
                        ZStack {
                            LinearGradient(colors: [(item.accent ?? .gray).opacity(0.35), Color.black.opacity(0.3)], startPoint: .topLeading, endPoint: .bottomTrailing)
                            Image(systemName: item.icon).font(.system(size: 22, weight: .medium)).foregroundStyle(.white.opacity(0.8))
                        }
                        .aspectRatio(256.0 / 224.0, contentMode: .fit)
                    }
                }
                .clipShape(RoundedRectangle(cornerRadius: 8))
                if let b = item.badge {
                    Text(b).font(.system(size: 10, weight: .bold)).padding(.horizontal, 6).padding(.vertical, 2)
                        .background(Color.red.opacity(0.85), in: Capsule()).padding(5)
                }
            }
            HStack(spacing: 4) {
                VStack(alignment: .leading, spacing: 1) {
                    Text(item.title).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                    if let s = item.subtitle { Text(s).font(.system(size: 11).monospacedDigit()).foregroundStyle(.white.opacity(0.55)).lineLimit(1) }
                }
                Spacer(minLength: 0)
                if item.x != nil || item.y != nil {
                    Menu {
                        if let y = item.y { Button(y.label) { y.run(); menu.touch() } }
                        if let x = item.x { Button(x.label, role: .destructive) { x.run(); menu.touch() } }
                    } label: {
                        Image(systemName: "ellipsis.circle").font(.system(size: 18)).frame(width: 30, height: 30)
                            .touchTarget(width: 30, height: 30)
                    }
                    .accessibilityIdentifier("qmMore.\(item.id)")
                }
            }
            if let a = item.accent { Rectangle().fill(a).frame(height: 3).clipShape(RoundedRectangle(cornerRadius: 1.5)) }
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color.white.opacity(focused ? 0.16 : 0.07)))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(focused ? Color.white.opacity(0.8) : .clear, lineWidth: 2))
        .opacity(item.enabled ? 1 : 0.38)
    }

    private func row(_ item: QMItem, focused: Bool) -> some View {
        HStack(spacing: 10) {
            Image(systemName: item.icon).font(.system(size: 15, weight: .medium)).frame(width: 24)
            Text(item.title).font(.system(size: 15, weight: .medium)).lineLimit(1).minimumScaleFactor(0.8)
            if item.locked != nil { lockBadge }
            Spacer(minLength: 8)
            value(item)
            if item.page != nil {
                Image(systemName: "chevron.right").font(.system(size: 12, weight: .semibold)).foregroundStyle(.white.opacity(0.4))
                    .accessibilityIdentifier("qmLink.\(item.id)")
            }
            if let x = item.x { smallButton(x.label) { x.run(); menu.touch() } }
        }
        .padding(.horizontal, 12)
        .frame(minHeight: 48)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.white.opacity(focused ? 0.16 : 0.06)))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(focused ? Color.white.opacity(0.8) : .clear, lineWidth: 2))
        .opacity(item.enabled ? 1 : 0.38)
        .disabled(!item.enabled)
    }

    @ViewBuilder
    private func value(_ item: QMItem) -> some View {
        switch item.value {
        case .none:
            EmptyView()
        case .toggle(let on):
            Capsule().fill(on ? Color.green.opacity(0.85) : Color.white.opacity(0.2))
                .frame(width: 46, height: 28)
                .overlay(Circle().fill(.white).frame(width: 24, height: 24).offset(x: on ? 9 : -9))
                .animation(.easeOut(duration: 0.12), value: on)
                .accessibilityElement()
                .accessibilityIdentifier("qmToggle.\(item.id)")
                .accessibilityValue(on ? "1" : "0")
        case .choice(let options, let index) where options.count > 3:
            Menu {
                ForEach(Array(options.enumerated()), id: \.offset) { i, o in
                    Button { item.setChoice?(i); menu.touch() } label: {
                        if i == index { Label(o, systemImage: "checkmark") } else { Text(o) }
                    }
                }
            } label: {
                HStack(spacing: 4) {
                    Text(options.indices.contains(index) ? options[index] : "").font(.system(size: 13, weight: .semibold)).lineLimit(1)
                    Image(systemName: "chevron.up.chevron.down").font(.system(size: 10, weight: .semibold))
                }
                .padding(.horizontal, 10).frame(minHeight: 30)
                .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.14)))
                .touchTarget(height: 30)
            }
            .accessibilityIdentifier("qmChoice.\(item.id)")
        case .choice(let options, let index):
            HStack(spacing: 2) {
                ForEach(Array(options.enumerated()), id: \.offset) { i, o in
                    Text(o).font(.system(size: 12, weight: i == index ? .semibold : .regular)).lineLimit(1)
                        .padding(.horizontal, 8).frame(minHeight: 30)
                        .background(RoundedRectangle(cornerRadius: 7).fill(i == index ? Color.white.opacity(0.25) : .clear))
                        .touchTarget(height: 30)
                        .onTapGesture { item.setChoice?(i); menu.touch() }
                        .accessibilityAddTraits(i == index ? [.isButton, .isSelected] : .isButton)
                        .accessibilityIdentifier("qmSeg.\(item.id).\(i)")
                }
            }
            .padding(2).background(RoundedRectangle(cornerRadius: 9).fill(Color.white.opacity(0.08)))
        case .slider(let v, let range, let text):
            HStack(spacing: 6) {
                Slider(value: Binding(get: { (v - range.lowerBound) / max(0.0001, range.upperBound - range.lowerBound) },
                                      set: { item.setFraction?($0); menu.touch() }), in: 0...1)
                    .frame(width: 130)
                    .accessibilityIdentifier("qmSlider.\(item.id)")
                Text(verbatim: text).font(.system(size: 12).monospacedDigit()).frame(minWidth: 44, alignment: .trailing)
            }
        case .text(let t):
            Text(verbatim: t).font(.system(size: 13)).foregroundStyle(.white.opacity(0.7)).lineLimit(1)
        case .buttons(let pairs):
            HStack(spacing: 8) {
                ForEach(Array(pairs.enumerated()), id: \.offset) { _, p in
                    HStack(spacing: 3) {
                        Text(p.glyph).font(.system(size: 12, weight: .bold)).padding(.horizontal, 6).padding(.vertical, 2)
                            .background(Color.white.opacity(0.2), in: RoundedRectangle(cornerRadius: 5))
                        Text(p.verb).font(.system(size: 12))
                    }
                }
            }
        }
    }

    private func controllerPage(width: CGFloat) -> some View {
        let slot = menu.listPageIndex(.controller)
        let family = QuickMenuPages.diagramFamily(model, slot: slot)
        let info = monitor.controllers.first { $0.slot == slot }
        let els = menu.diagramElements(model)
        let focus = max(0, menu.focusIndex(.controller))
        return VStack(spacing: 8) {
            // The diagram is drawn on its fixed canvas (RNF_DIAGRAM_CANVAS_*, 560 x 262) and scaled to the width,
            // as on the Mac: framed without scaling, its sides and top were cut off (2026-11-21).
            let canvas = ControllerDiagramLayout.canvas
            let scale = min(width, 480) / canvas.width
            ControllerDiagramView(family: family, slot: slot, config: model.inputConfig, labels: info?.labels ?? [:],
                                  pressed: monitor.pressed, focused: hasPad && els.indices.contains(focus) ? els[focus].element : nil,
                                  onSelect: { element in menu.openAssign("gc\(slot):\(element)") })
                .scaleEffect(scale)
                .frame(width: canvas.width * scale, height: canvas.height * scale)
                .id("controllerDiagram")
            Text(L("Tap a button on the picture to change what it does")).font(.system(size: 12)).foregroundStyle(.white.opacity(0.55))
            // The kind and the button layout remembered for this controller (D-pad: below the diagram).
            ForEach(Array(QuickMenuPages.controllerRows(model, slot: slot).enumerated()), id: \.element.id) { k, item in
                row(item, focused: hasPad && focus == els.count + k)
                    .contentShape(Rectangle())
                    .onTapGesture { menu.setFocus(els.count + k, on: .controller); item.confirm?(); menu.touch() }
                    .accessibilityElement(children: .contain)
                    .accessibilityIdentifier("qm.\(item.id)")
                    .id(item.id)
            }
        }
        .frame(maxWidth: .infinity)
        .onAppear { monitor.setLive(true); model.input.editingSlot = slot }
        .onChange(of: slot) { _, s in model.input.editingSlot = s }
        .onDisappear { monitor.setLive(false) }
    }

    private func smallButton(_ label: String, _ run: @escaping () -> Void) -> some View {
        Button(action: run) {
            Text(label).font(.system(size: 12, weight: .semibold)).padding(.horizontal, 10).frame(height: 30)
                .background(Color.white.opacity(0.12), in: Capsule())
                .touchTarget(height: 30)
        }
    }

    private func pagerButton(_ icon: String, _ run: @escaping () -> Void) -> some View {
        Button(action: run) {
            Image(systemName: icon).font(.system(size: 14, weight: .semibold)).frame(width: 32, height: 32)
                .background(Color.white.opacity(0.1), in: Circle())
                .touchTarget(width: 32, height: 32)
        }
    }
}
