// Quick Menu state: open / close (pauses the game and restores it), the page stack, focus per
// page, list paging, and what controller / keyboard input does on a page
// (docs/design/UI_REDESIGN.md). The pages' items come from QuickMenuPages.swift.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

final class QuickMenuController: ObservableObject {
    @Published private(set) var isOpen = false
    @Published private(set) var stack: [QMPage] = [.top]
    @Published private(set) var focus: [QMPage: Int] = [:]
    /// List page of paged pages (takes, bookmarks, bindings, controller slot).
    @Published private(set) var listPage: [QMPage: Int] = [:]
    /// Bumped after an action so values read from preferences are shown fresh.
    @Published private(set) var revision = 0
    /// The physical id ("gc0:face.east") whose action the .assign page picks.
    @Published private(set) var assignElement: String?
    /// +1 / -1: direction of the last page change (transition), 0 = none.
    private(set) var lastMove = 0

    /// The game was running when the menu opened (closing resumes it).
    private var resumeOnClose = false
    weak var model: AppModel?

    var page: QMPage { stack.last ?? .top }

    func focusIndex(_ p: QMPage) -> Int { focus[p] ?? defaultFocus(p) }
    func listPageIndex(_ p: QMPage) -> Int { listPage[p] ?? 0 }

    private func defaultFocus(_ p: QMPage) -> Int {
        guard p == .top, let m = model else { return 0 }
        // Resume is the default; without a session, Settings (the only useful tile with Game).
        return m.status.hasSession ? 0 : 4
    }

    // MARK: open / close

    /// Opens the menu (pausing the game), optionally straight at `page` (its path is the stack,
    /// so Back walks up the hierarchy).
    func open(at target: QMPage? = nil) {
        // A dialog has the focus: the pill / L+R / Esc / menu commands wait until it is answered.
        guard let m = model, !m.dialogs.isActive else { return }
        if !isOpen {
            resumeOnClose = m.status.hasSession && !m.status.paused
            if m.status.hasSession { m.setPaused(true) }
            focus = [:]
            listPage = [:]
        }
        lastMove = 0
        withAnimation(.easeOut(duration: 0.12)) {
            stack = (target ?? .top).path
            isOpen = true
        }
        m.updateUIMode()
    }

    /// `resume`: true = play, false = stay paused, nil = as before the menu opened.
    func close(resume: Bool? = nil) {
        guard isOpen, let m = model else { return }
        withAnimation(.easeOut(duration: 0.12)) { isOpen = false }
        if m.input.captureHandler != nil { m.input.captureHandler = nil; m.capturingAction = nil }
        if m.status.hasSession && (resume ?? resumeOnClose) { m.setPaused(false) }
        resumeOnClose = false
        m.updateUIMode()
    }

    func toggle() {
        guard model?.dialogs.isActive != true else { return }
        if isOpen { close() } else { open() }
    }

    // MARK: navigation

    func push(_ p: QMPage) {
        lastMove = 1
        withAnimation(.easeOut(duration: 0.12)) { stack.append(p) }
    }

    /// Back one level; on the top level it closes the menu (resume).
    func back() {
        if stack.count <= 1 { close(); return }
        lastMove = -1
        withAnimation(.easeOut(duration: 0.12)) { _ = stack.popLast() }
    }

    /// Breadcrumb click.
    func popTo(_ p: QMPage) {
        guard let i = stack.lastIndex(of: p) else { return }
        lastMove = -1
        withAnimation(.easeOut(duration: 0.12)) { stack = Array(stack.prefix(i + 1)) }
    }

    /// A breadcrumb segment clicked / tapped: back to that level; a settings tab that is not on the
    /// stack (the "Settings" segment = the first tab) switches to it.
    func crumbSelected(_ p: QMPage) {
        if stack.contains(p) { popTo(p) } else { switchTab(to: p) }
    }

    /// Settings tab strip (L / R, or a click).
    func switchTab(to p: QMPage) {
        guard QMPage.settingsTabs.contains(p), let cur = stack.first(where: { QMPage.settingsTabs.contains($0) }) else { return }
        let a = QMPage.settingsTabs.firstIndex(of: cur) ?? 0, b = QMPage.settingsTabs.firstIndex(of: p) ?? 0
        lastMove = b >= a ? 1 : -1
        withAnimation(.easeOut(duration: 0.12)) { stack = p.path }
    }

    func setFocus(_ i: Int, on p: QMPage) {
        guard focus[p] != i else { return }
        withAnimation(.easeOut(duration: 0.12)) { focus[p] = i }
    }

    func setListPage(_ i: Int, on p: QMPage) {
        withAnimation(.easeOut(duration: 0.12)) {
            listPage[p] = i
            focus[p] = 0
        }
    }

    func touch() { revision &+= 1 }

    // MARK: input

    func handle(_ n: NavInput) {
        guard isOpen, let m = model else { return }
        let p = page
        if p == .controller { handleController(n, m); return }
        let content = QuickMenuPages.content(p, model: m, menu: self)
        let items = content.items
        let i = min(focusIndex(p), max(0, items.count - 1))
        switch n {
        case .options, .search:
            break
        case .menu, .escape:   // L+R / Esc close the menu from any level
            close()
        case .back:            // B / Delete: one level up (closes on the top level)
            back()
        case .up, .down, .left, .right:
            let d: NavDirection = n == .up ? .up : n == .down ? .down : n == .left ? .left : .right
            // Rows: left / right change the focused value.
            if p.layout == .rows, d == .left || d == .right, items.indices.contains(i), let adjust = items[i].adjust, items[i].enabled {
                adjust(d == .left ? -1 : 1)
                touch()
                return
            }
            if let j = QuickMenuNav.move(i, count: items.count, columns: p.columns, d) {
                setFocus(j, on: p)
            } else if p.paged, d == .left || d == .right {
                flipPage(p, content: content, by: d == .left ? -1 : 1)
            }
        case .confirm:
            guard items.indices.contains(i) else { return }
            activate(items[i], on: p)
        case .x:
            if items.indices.contains(i), let a = items[i].x, items[i].enabled { a.run(); touch() }
            else if let a = content.x { a.run(); touch() }
        case .y:
            if items.indices.contains(i), let a = items[i].y, items[i].enabled { a.run(); touch() }
            else if let a = content.y { a.run(); touch() }
        case .pagePrev, .pageNext:
            let by = n == .pagePrev ? -1 : 1
            if p.paged {
                flipPage(p, content: content, by: by)
            } else if let k = QMPage.settingsTabs.firstIndex(of: p) {
                let tabs = QMPage.settingsTabs
                switchTab(to: tabs[(k + by + tabs.count) % tabs.count])
            }
        }
    }

    // MARK: controller diagram

    /// The diagram's elements for the pad shown (focus = an index into them).
    func diagramElements(_ m: AppModel) -> [DiagramElement] {
        ControllerDiagramLayout(family: QuickMenuPages.diagramFamily(m, slot: listPageIndex(.controller))).elements
    }

    /// Settings › Controls › Controller: the D-pad / left stick move between the buttons on the
    /// picture (the nearest one in that direction), confirm opens the action picker, X = next pad,
    /// Y = defaults, cancel = back, L / R = the other pad.
    private func handleController(_ n: NavInput, _ m: AppModel) {
        let p = QMPage.controller
        let content = QuickMenuPages.content(p, model: m, menu: self)
        let els = diagramElements(m)
        let i = min(max(0, focusIndex(p)), max(0, els.count - 1))
        switch n {
        case .menu, .escape: close()
        case .back: back()
        case .up, .down, .left, .right:
            guard els.indices.contains(i) else { return }
            let dx: CGFloat = n == .left ? -1 : n == .right ? 1 : 0, dy: CGFloat = n == .up ? -1 : n == .down ? 1 : 0
            let c = els[i].center
            var best: Int?, bestScore = CGFloat.greatestFiniteMagnitude
            for (j, e) in els.enumerated() where j != i {
                let vx = e.center.x - c.x, vy = e.center.y - c.y
                let along = vx * dx + vy * dy, across = abs(vx * dy) + abs(vy * dx)
                if along <= 1 { continue }
                let score = along + across * 2.2
                if score < bestScore { bestScore = score; best = j }
            }
            if let b = best { setFocus(b, on: p) }
        case .confirm:
            if els.indices.contains(i) { openAssign("gc\(listPageIndex(p)):\(els[i].element)") }
        case .x: content.x?.run(); touch()
        case .y: content.y?.run(); touch()
        case .pagePrev, .pageNext: flipPage(p, content: content, by: n == .pagePrev ? -1 : 1)
        case .options, .search: break
        }
    }

    /// The action picker of a controller button (page .assign): focus on the action assigned now.
    func openAssign(_ physicalID: String) {
        guard let m = model, let colon = physicalID.firstIndex(of: ":") else { return }
        let element = String(physicalID[physicalID.index(after: colon)...])
        let slot = Int(physicalID.dropFirst(2).prefix { $0.isNumber }) ?? 0
        if let i = diagramElements(m).firstIndex(where: { $0.element == element }), slot == listPageIndex(.controller) {
            setFocus(i, on: .controller)
        }
        assignElement = physicalID
        let current = ControllerAssignments.actions(element: element, slot: slot, config: m.inputConfig).first ?? ""
        let k = QuickMenuPages.assignChoices(slot: slot).firstIndex(of: current) ?? 0
        let per = QMPage.assign.capacity
        listPage[.assign] = k / per
        focus[.assign] = k % per
        if page != .assign { push(.assign) }
    }

    private func flipPage(_ p: QMPage, content: QMContent, by: Int) {
        let pages = content.pageCount
        guard pages > 1 else { return }
        let next = (listPageIndex(p) + by + pages) % pages
        lastMove = by
        setListPage(next, on: p)
    }

    /// Confirm on an item (also a click).
    func activate(_ item: QMItem, on p: QMPage) {
        guard item.enabled else { NSSound.beep(); return }
        if let target = item.page { push(target); return }
        if let run = item.confirm { run(); touch(); return }
        if let adjust = item.adjust { adjust(1); touch() }
    }
}
