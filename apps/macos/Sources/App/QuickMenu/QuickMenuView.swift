// Quick Menu overlay (docs/design/UI_REDESIGN.md, "Quick Menu" / "Visual language"): the game
// stays visible behind it, dimmed to ~35 %; a dark translucent panel with the page, a breadcrumb
// on sub-levels, one line describing the focused item and a hint bar with the connected
// controller's buttons. Every page has a fixed height: nothing scrolls.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

enum QMStyle {
    static let panel = Color(red: 0x14 / 255, green: 0x14 / 255, blue: 0x16 / 255).opacity(0.92)
    static let brand = Color(red: 1, green: 0x3B / 255, blue: 0x3B / 255)
    static let tile = Color.white.opacity(0.06)
    static let tileFocused = Color.white.opacity(0.13)
    static let radius: CGFloat = 16
    static let tileRadius: CGFloat = 12
    static let margin: CGFloat = 24
    static let gap: CGFloat = 10
    static let title = Font.system(size: 22, weight: .semibold)
    static let label = Font.system(size: 15, weight: .medium)
    static let hint = Font.system(size: 12)
    static let anim = Animation.easeOut(duration: 0.12)
    static let maxWidth: CGFloat = 900

    /// Content height of each layout (fixed, so pages never scroll or jump).
    static let topTileHeight: CGFloat = 128
    static let actionHeight: CGFloat = 92
    static let rowHeight: CGFloat = 44
    static let rowGap: CGFloat = 6
    static let practiceCardHeight: CGFloat = 148
    static let cardHeight: CGFloat = 132
    static let diagramHeight: CGFloat = 300

    static func contentHeight(_ p: QMPage) -> CGFloat {
        switch p.layout {
        case .tiles: return topTileHeight
        case .actions: return 2 * actionHeight + gap
        case .rows: return 6 * rowHeight + 5 * rowGap
        case .cards(_, let rows): return CGFloat(rows) * (p == .practice ? practiceCardHeight : cardHeight) + CGFloat(rows - 1) * gap
        case .custom: return diagramHeight
        }
    }
}

/// Measured panel fit, for the scripted page walk (TestHooks: menuWalk).
final class QuickMenuProbe {
    static let shared = QuickMenuProbe()
    var page: QMPage = .top
    var panelHeight: CGFloat = 0
    var areaHeight: CGFloat = 0
    var fits: Bool { panelHeight <= areaHeight - 2 * QMStyle.margin + 0.5 }
}

struct QuickMenuOverlay: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var menu: QuickMenuController
    @ObservedObject var monitor: ControllerMonitor
    @ObservedObject private var crt = CRTSettingsModel.shared
    @ObservedObject private var stream = StreamOutputModel.shared
    @ObservedObject private var updates = UpdaterModel.shared

    var body: some View {
        GeometryReader { geo in
            ZStack {
                // The game behind at ~35 % (no blur: cheap on every GPU).
                Color.black.opacity(0.65)
                    .contentShape(Rectangle())
                    .onTapGesture { menu.close() }
                panel(area: geo.size)
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
        .environment(\.colorScheme, .dark)
    }

    private func panel(area: CGSize) -> some View {
        _ = menu.revision
        let p = menu.page
        let content = QuickMenuPages.content(p, model: model, menu: menu)
        let width = max(360, min(area.width - 2 * QMStyle.margin, QMStyle.maxWidth))
        let inner = width - 2 * QMStyle.margin
        let focus = min(menu.focusIndex(p), max(0, content.items.count - 1))
        let focused = content.items.indices.contains(focus) ? content.items[focus] : nil
        return VStack(alignment: .leading, spacing: 16) {
            if p != .top { header(p, content) }
            pageBody(p, content, width: inner, focus: focus)
                .frame(width: inner, height: QMStyle.contentHeight(p), alignment: .topLeading)
                .id(p)
                .transition(.asymmetric(insertion: .opacity.combined(with: .offset(x: CGFloat(menu.lastMove) * 16)),
                                        removal: .opacity))
            footer(p, content, focused: focused)
        }
        .padding(QMStyle.margin)
        .frame(width: width)
        .background(RoundedRectangle(cornerRadius: QMStyle.radius).fill(QMStyle.panel))
        .overlay(RoundedRectangle(cornerRadius: QMStyle.radius).stroke(Color.white.opacity(0.08), lineWidth: 1))
        .shadow(color: .black.opacity(0.5), radius: 24, y: 8)
        .contentShape(RoundedRectangle(cornerRadius: QMStyle.radius))
        .onTapGesture {}   // a click on the panel itself does not close the menu
        .background(GeometryReader { g in
            Color.clear.onAppear { probe(p, g.size.height, area.height) }
                .onChange(of: g.size.height) { _, h in probe(p, h, area.height) }
                .onChange(of: p) { _, np in probe(np, g.size.height, area.height) }
        })
        .foregroundStyle(.white)
    }

    private func probe(_ p: QMPage, _ h: CGFloat, _ area: CGFloat) {
        let pr = QuickMenuProbe.shared
        pr.page = p; pr.panelHeight = h; pr.areaHeight = area
    }

    // MARK: header (breadcrumb, settings tabs, caption)

    private func header(_ p: QMPage, _ content: QMContent) -> some View {
        HStack(spacing: 8) {
            // "⚙ Settings › Display › CRT Details": the settings tabs read "Settings › <tab>".
            // Every segment but the last is a link back to its level; "Settings" = the Settings top
            // page (the first tab, what its tile opens), like rnf_menu_crumb_select on the desktop.
            let crumbs: [(String, QMPage)] = p.path.dropFirst().flatMap { c -> [(String, QMPage)] in
                QMPage.settingsTabs.contains(c) ? [(String(localized: "Settings"), QMPage.settingsTabs[0]), (c.title, c)] : [(c.title, c)]
            }
            Image(systemName: p.path.dropFirst().first?.icon ?? p.icon).font(.system(size: 15, weight: .semibold)).foregroundStyle(QMStyle.brand)
            ForEach(Array(crumbs.enumerated()), id: \.offset) { i, crumb in
                let last = i == crumbs.count - 1
                if i > 0 { Image(systemName: "chevron.right").font(.system(size: 11, weight: .semibold)).foregroundStyle(.white.opacity(0.4)) }
                Text(crumb.0)
                    .font(last ? QMStyle.title : .system(size: 22, weight: .regular))
                    .foregroundStyle(last ? Color.white : Color.white.opacity(0.55))
                    .lineLimit(1)
                    .contentShape(Rectangle())
                    .onTapGesture { if !last { menu.crumbSelected(crumb.1) } }
            }
            if let c = content.caption {
                Text(c).font(.system(size: 13, weight: .medium)).foregroundStyle(.white.opacity(0.6)).lineLimit(1)
                    .padding(.leading, 6)
            }
            Spacer(minLength: 12)
            if QMPage.settingsTabs.contains(p) { settingsTabs(p) }
            else if p.paged && content.pageCount > 1 { pager(p, content) }
        }
        .frame(height: 30)
    }

    private func settingsTabs(_ p: QMPage) -> some View {
        let g = HintGlyphs.current(monitor.controllers)
        return HStack(spacing: 4) {
            KeyCap(g.l)
            ForEach(QMPage.settingsTabs, id: \.self) { t in
                Text(t.title)
                    .font(.system(size: 14, weight: t == p ? .semibold : .regular))
                    .foregroundStyle(t == p ? Color.white : Color.white.opacity(0.55))
                    .padding(.horizontal, 10).frame(height: 28)
                    .background(RoundedRectangle(cornerRadius: 8).fill(t == p ? Color.white.opacity(0.14) : .clear))
                    .contentShape(Rectangle())
                    .onTapGesture { menu.switchTab(to: t) }
            }
            KeyCap(g.r)
        }
    }

    private func pager(_ p: QMPage, _ content: QMContent) -> some View {
        let g = HintGlyphs.current(monitor.controllers)
        let cur = menu.listPageIndex(p)
        return HStack(spacing: 6) {
            KeyCap(g.l).onTapGesture { menu.handle(.pagePrev) }
            HStack(spacing: 5) {
                ForEach(0..<content.pageCount, id: \.self) { i in
                    Circle().fill(i == cur ? Color.white : Color.white.opacity(0.3)).frame(width: 6, height: 6)
                }
            }
            KeyCap(g.r).onTapGesture { menu.handle(.pageNext) }
        }
    }

    // MARK: page body

    @ViewBuilder
    private func pageBody(_ p: QMPage, _ content: QMContent, width: CGFloat, focus: Int) -> some View {
        switch p.layout {
        case .tiles(let columns):
            let w = (width - CGFloat(columns - 1) * QMStyle.gap) / CGFloat(columns)
            HStack(spacing: QMStyle.gap) {
                ForEach(Array(content.items.enumerated()), id: \.element.id) { i, item in
                    TopTile(item: item, focused: i == focus)
                        .frame(width: w, height: QMStyle.topTileHeight)
                        .modifier(Interactive(menu: menu, page: p, index: i, item: item))
                }
            }
        case .actions:
            grid(content.items, columns: 3, width: width, height: QMStyle.actionHeight, focus: focus, page: p) { item, f in
                ActionTile(item: item, focused: f)
            }
        case .rows:
            VStack(spacing: QMStyle.rowGap) {
                ForEach(Array(content.items.enumerated()), id: \.element.id) { i, item in
                    SettingRow(item: item, focused: i == focus, menu: menu)
                        .frame(height: QMStyle.rowHeight)
                        .modifier(Interactive(menu: menu, page: p, index: i, item: item))
                }
            }
        case .cards(let columns, _):
            let h = p == .practice ? QMStyle.practiceCardHeight : QMStyle.cardHeight
            if content.items.isEmpty {
                Text("Nothing here yet").font(QMStyle.label).foregroundStyle(.white.opacity(0.5))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                grid(content.items, columns: columns, width: width, height: h, focus: focus, page: p) { item, f in
                    CardTile(item: item, focused: f, practice: p == .practice)
                }
            }
        case .custom:
            ControllerPage(menu: menu, monitor: monitor)
        }
    }

    private func grid<V: View>(_ items: [QMItem], columns: Int, width: CGFloat, height: CGFloat, focus: Int, page: QMPage,
                               @ViewBuilder cell: @escaping (QMItem, Bool) -> V) -> some View {
        let w = (width - CGFloat(columns - 1) * QMStyle.gap) / CGFloat(columns)
        let rows = stride(from: 0, to: items.count, by: columns).map { Array($0..<min(items.count, $0 + columns)) }
        return VStack(alignment: .leading, spacing: QMStyle.gap) {
            ForEach(rows, id: \.first) { row in
                HStack(spacing: QMStyle.gap) {
                    ForEach(row, id: \.self) { i in
                        cell(items[i], i == focus)
                            .frame(width: w, height: height)
                            .modifier(Interactive(menu: menu, page: page, index: i, item: items[i]))
                    }
                }
            }
        }
    }

    // MARK: footer (description + hint bar)

    private func footer(_ p: QMPage, _ content: QMContent, focused: QMItem?) -> some View {
        let g = HintGlyphs.current(monitor.controllers)
        var hints: [(String, String, () -> Void)] = []
        if let f = focused, f.enabled, f.page != nil || f.confirm != nil || f.adjust != nil {
            hints.append((g.confirm, f.verb ?? String(localized: "Select"), { menu.handle(.confirm) }))
        } else if p == .controller {
            hints.append((g.confirm, String(localized: "Change"), { menu.handle(.confirm) }))
        }
        let x = focused?.x ?? content.x, y = focused?.y ?? content.y
        if let y { hints.append((g.y, y.label, { menu.handle(.y) })) }
        if let x { hints.append((g.x, x.label, { menu.handle(.x) })) }
        if hints.count < 3 {
            hints.append((g.back == "⌫" && p == .top ? "esc" : g.back, p == .top ? String(localized: "Close") : String(localized: "Back"), { menu.handle(.back) }))
        }
        let detail: String = {
            if p == .controller { return String(localized: "Pressed buttons light up · click a button to change it") }
            if let f = focused { return f.enabled ? f.detail : String(localized: "Not available right now") }
            return ""
        }()
        return HStack(spacing: 16) {
            Text(detail).font(.system(size: 13)).foregroundStyle(.white.opacity(0.7)).lineLimit(1).truncationMode(.tail)
                .id(detail)
                .transition(.opacity)
            Spacer(minLength: 8)
            HStack(spacing: 14) {
                ForEach(Array(hints.prefix(3).enumerated()), id: \.offset) { _, h in
                    HStack(spacing: 5) {
                        KeyCap(h.0)
                        Text(h.1).font(QMStyle.hint).foregroundStyle(.white.opacity(0.8)).fixedSize()
                    }
                    .contentShape(Rectangle())
                    .onTapGesture(perform: h.2)
                }
            }
            .fixedSize()
        }
        .frame(height: 22)
    }
}

/// Hover = focus, click = confirm (mouse users get the same model as the controller).
private struct Interactive: ViewModifier {
    let menu: QuickMenuController
    let page: QMPage
    let index: Int
    let item: QMItem
    func body(content: Content) -> some View {
        content
            .contentShape(Rectangle())
            .onHover { if $0 { menu.setFocus(index, on: page) } }
            .onTapGesture {
                menu.setFocus(index, on: page)
                menu.activate(item, on: page)
            }
    }
}

// MARK: - tiles, rows, cards

private struct FocusFrame: ViewModifier {
    let focused: Bool
    let enabled: Bool
    var radius: CGFloat = QMStyle.tileRadius
    var raise: CGFloat = 1.0
    func body(content: Content) -> some View {
        content
            .background(RoundedRectangle(cornerRadius: radius).fill(focused ? QMStyle.tileFocused : QMStyle.tile))
            .overlay(RoundedRectangle(cornerRadius: radius).strokeBorder(focused ? QMStyle.brand : Color.clear, lineWidth: 2))
            .scaleEffect(focused ? raise : 1)
            .opacity(enabled ? 1 : 0.35)
            .animation(QMStyle.anim, value: focused)
    }
}

private struct TopTile: View {
    let item: QMItem
    let focused: Bool
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: item.icon).font(.system(size: 30, weight: .medium))
                .foregroundStyle(focused ? QMStyle.brand : Color.white)
                .frame(height: 36)
            Text(item.title).font(QMStyle.label).lineLimit(1).minimumScaleFactor(0.8)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .modifier(FocusFrame(focused: focused, enabled: item.enabled, raise: 1.04))
    }
}

private struct ActionTile: View {
    let item: QMItem
    let focused: Bool
    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: item.icon).font(.system(size: 22, weight: .medium))
                .foregroundStyle(focused ? QMStyle.brand : Color.white)
                .frame(width: 30)
            VStack(alignment: .leading, spacing: 3) {
                Text(item.title).font(QMStyle.label).lineLimit(1).minimumScaleFactor(0.85)
                if let s = item.subtitle { Text(s).font(QMStyle.hint).foregroundStyle(.white.opacity(0.55)).lineLimit(1) }
            }
            Spacer(minLength: 0)
            if item.page != nil { Image(systemName: "chevron.right").font(.system(size: 12, weight: .semibold)).foregroundStyle(.white.opacity(0.4)) }
        }
        .padding(.horizontal, 16)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .modifier(FocusFrame(focused: focused, enabled: item.enabled, raise: 1.02))
    }
}

private struct SettingRow: View {
    let item: QMItem
    let focused: Bool
    let menu: QuickMenuController
    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: item.icon).font(.system(size: 14, weight: .medium))
                .foregroundStyle(focused ? QMStyle.brand : Color.white.opacity(0.6))
                .frame(width: 22)
            Text(item.title).font(QMStyle.label).lineLimit(1)
                .frame(minWidth: 120, alignment: .leading)
                .layoutPriority(1)
            Spacer(minLength: 12)
            value
        }
        .padding(.horizontal, 14)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .modifier(FocusFrame(focused: focused, enabled: item.enabled, radius: 10, raise: 1.0))
    }

    @ViewBuilder private var value: some View {
        switch item.value {
        case .toggle(let on):
            Switch(on: on)
        case .choice(let options, let index):
            if options.count <= 5 {
                HStack(spacing: 2) {
                    ForEach(Array(options.enumerated()), id: \.offset) { i, o in
                        Text(o).font(.system(size: 13, weight: i == index ? .semibold : .regular))
                            .foregroundStyle(i == index ? Color.white : Color.white.opacity(0.5))
                            .lineLimit(1)
                            .padding(.horizontal, 9).frame(height: 26)
                            .background(RoundedRectangle(cornerRadius: 7).fill(i == index ? (focused ? QMStyle.brand : Color.white.opacity(0.18)) : .clear))
                            .contentShape(Rectangle())
                            .onTapGesture { item.setChoice?(i); menu.touch() }
                    }
                }
                .padding(2)
                .background(RoundedRectangle(cornerRadius: 9).fill(Color.black.opacity(0.25)))
                .fixedSize()
            } else {
                HStack(spacing: 10) {
                    Image(systemName: "chevron.left").onTapGesture { item.adjust?(-1); menu.touch() }
                    Text(options[min(max(0, index), options.count - 1)]).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                    Image(systemName: "chevron.right").onTapGesture { item.adjust?(1); menu.touch() }
                }
                .font(.system(size: 12, weight: .semibold))
                .fixedSize()
            }
        case .slider(let v, let range, let text):
            HStack(spacing: 10) {
                SliderBar(fraction: (v - range.lowerBound) / max(0.0001, range.upperBound - range.lowerBound), focused: focused) { f in
                    item.setFraction?(f); menu.touch()
                }
                .frame(width: 170, height: 20)
                Text(text).font(.system(size: 13, weight: .medium).monospacedDigit()).foregroundStyle(.white.opacity(0.85))
                    .frame(minWidth: 64, alignment: .trailing).lineLimit(1)
            }
        case .text(let t):
            Text(t).font(.system(size: 13)).foregroundStyle(.white.opacity(0.6)).lineLimit(1).truncationMode(.middle)
        case .buttons(let pairs):
            HStack(spacing: 10) {
                Image(systemName: "chevron.left").onTapGesture { item.adjust?(-1); menu.touch() }
                ForEach(Array(pairs.enumerated()), id: \.offset) { _, p in
                    HStack(spacing: 5) {
                        KeyCap(p.glyph)
                        Text(p.verb).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                    }
                }
                Image(systemName: "chevron.right").onTapGesture { item.adjust?(1); menu.touch() }
            }
            .font(.system(size: 12, weight: .semibold))
            .fixedSize()
        case .none:
            if item.page != nil {
                Image(systemName: "chevron.right").font(.system(size: 12, weight: .semibold)).foregroundStyle(.white.opacity(0.45))
            }
        }
    }
}

private struct Switch: View {
    let on: Bool
    var body: some View {
        ZStack(alignment: on ? .trailing : .leading) {
            Capsule().fill(on ? QMStyle.brand : Color.white.opacity(0.2)).frame(width: 40, height: 22)
            Circle().fill(Color.white).frame(width: 18, height: 18).padding(2)
        }
        .animation(QMStyle.anim, value: on)
    }
}

private struct SliderBar: View {
    let fraction: Double
    let focused: Bool
    let set: (Double) -> Void
    var body: some View {
        GeometryReader { g in
            let f = CGFloat(min(1, max(0, fraction)))
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.18)).frame(height: 4)
                Capsule().fill(focused ? QMStyle.brand : Color.white.opacity(0.8)).frame(width: max(4, g.size.width * f), height: 4)
                Circle().fill(Color.white).frame(width: 14, height: 14).offset(x: (g.size.width - 14) * f)
            }
            .frame(maxHeight: .infinity)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { v in set(Double(min(1, max(0, v.location.x / g.size.width)))) })
        }
    }
}

private struct CardTile: View {
    let item: QMItem
    let focused: Bool
    let practice: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ZStack(alignment: .topLeading) {
                thumb
                if let b = item.badge {
                    Text(b).font(.system(size: 11, weight: .bold)).padding(.horizontal, 7).padding(.vertical, 2)
                        .background(Capsule().fill(QMStyle.brand)).padding(6)
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: practice ? 92 : 78)
            .clipped()
            VStack(alignment: .leading, spacing: 2) {
                Text(item.title).font(.system(size: 14, weight: .semibold)).lineLimit(1)
                if let s = item.subtitle { Text(s).font(QMStyle.hint.monospacedDigit()).foregroundStyle(.white.opacity(0.55)).lineLimit(1) }
            }
            .padding(.horizontal, 10).padding(.vertical, 7)
            Spacer(minLength: 0)
        }
        .clipShape(RoundedRectangle(cornerRadius: QMStyle.tileRadius))
        .overlay(alignment: .top) {
            if let a = item.accent { Rectangle().fill(a).frame(height: 3).clipShape(RoundedRectangle(cornerRadius: 1.5)) }
        }
        .modifier(FocusFrame(focused: focused, enabled: item.enabled, raise: 1.03))
    }

    @ViewBuilder private var thumb: some View {
        if let img = item.image {
            Image(decorative: img, scale: 1).interpolation(.none).resizable().scaledToFill()
        } else {
            ZStack {
                LinearGradient(colors: [(item.accent ?? .gray).opacity(0.35), Color.black.opacity(0.3)], startPoint: .topLeading, endPoint: .bottomTrailing)
                Image(systemName: item.icon).font(.system(size: 26, weight: .medium)).foregroundStyle(.white.opacity(0.8))
            }
        }
    }
}

/// Settings › Controls › Controller: the diagram of pad 1 / 2 (L / R), held buttons light up, a click
/// assigns.
private struct ControllerPage: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var menu: QuickMenuController
    @ObservedObject var monitor: ControllerMonitor
    var body: some View {
        let slot = menu.listPageIndex(.controller)
        let info = monitor.controllers.first { $0.slot == slot }
        let family = QuickMenuPages.diagramFamily(model, slot: slot)
        let canvas = ControllerDiagramLayout.canvas
        let els = menu.diagramElements(model)
        let f = min(max(0, menu.focusIndex(.controller)), max(0, els.count - 1))
        GeometryReader { g in
            let s = min(1.15, g.size.width / canvas.width, g.size.height / canvas.height)
            ControllerDiagramView(family: family, slot: slot, config: model.inputConfig, labels: info?.labels ?? [:],
                                  pressed: monitor.pressed, focused: els.indices.contains(f) ? els[f].element : nil) { element in
                menu.openAssign("gc\(slot):\(element)")
            }
                .scaleEffect(s)
                .frame(width: canvas.width * s, height: canvas.height * s)
                .frame(width: g.size.width, height: g.size.height)
        }
        .onAppear { monitor.setLive(true) }
        .onDisappear { monitor.setLive(false) }
    }
}

// MARK: - button glyphs

/// Hint-bar glyphs for the connected controller family (keyboard when none is connected). Confirm /
/// back are the UI's confirm / cancel buttons (east / south by default; Settings › Controls ›
/// Confirm Button swaps them: rnf_ui_confirm_element).
struct HintGlyphs {
    let confirm: String, back: String, x: String, y: String, l: String, r: String
    var controller = true

    static var southConfirm: Bool { UserDefaults.standard.bool(forKey: "southConfirm") }
    static var confirmElement: String { String(cString: rnf_ui_confirm_element(southConfirm ? 1 : 0)) }
    static var cancelElement: String { String(cString: rnf_ui_cancel_element(southConfirm ? 1 : 0)) }

    static func current(_ controllers: [ControllerInfo]) -> HintGlyphs {
        guard let c = controllers.min(by: { $0.slot < $1.slot }) else {
            return HintGlyphs(confirm: "⏎", back: "⌫", x: "X", y: "Y", l: "⇧⇥", r: "⇥", controller: false)
        }
        let ps = c.family == .playStation
        return HintGlyphs(confirm: c.label(confirmElement), back: c.label(cancelElement), x: c.label("face.west"), y: c.label("face.north"),
                          l: ps ? "L1" : "L", r: ps ? "R1" : "R")
    }
}

struct KeyCap: View {
    let text: String
    init(_ t: String) { text = t }
    var body: some View {
        Text(text).font(.system(size: 11, weight: .bold, design: .rounded))
            .foregroundStyle(.white.opacity(0.9))
            .padding(.horizontal, 5)
            .frame(minWidth: 20, minHeight: 20)
            .background(RoundedRectangle(cornerRadius: 5).fill(Color.white.opacity(0.16)))
            .overlay(RoundedRectangle(cornerRadius: 5).stroke(Color.white.opacity(0.22), lineWidth: 1))
            .fixedSize()
    }
}
