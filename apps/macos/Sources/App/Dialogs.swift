// In-window dialogs instead of system alerts (docs/design/UI_REDESIGN.md, "Dialogs and notices"):
// drawn over the game / menu / library in the Quick Menu's look (Views/DialogOverlay.swift), so the
// controller, the keyboard and the mouse all work and the window stays the only surface. Requests
// are queued and answered through a callback; nothing waits on the main thread. While one is up it
// owns navigation input: the Menu pill and the L+R chord do nothing, the game is paused (and
// resumed afterwards if the dialog paused it).
// Purely informational messages are toasts instead (AppModel.flash).
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI

struct AppDialog {
    enum Kind { case info, warning, question }
    var kind: Kind = .warning
    var title: String
    var message: String = ""
    /// Left to right. The first one is focused unless `defaultIndex` says otherwise.
    var buttons: [String] = [String(localized: "OK")]
    var defaultIndex = 0
    /// Cancel / Esc / the controller's cancel button press this one (nil: the last button).
    var cancelIndex: Int?
    /// Drawn in the brand red.
    var destructiveIndex: Int?
    /// One on/off row above the buttons (e.g. "Keep A/B repeat sections").
    var toggle: String?
    var toggleOn = false
    /// A text field (rename) with this initial text.
    var text: String?
    var onResult: ((DialogAnswer) -> Void)?
}

struct DialogAnswer {
    let button: Int
    let toggleOn: Bool
    let text: String
}

final class DialogCenter: ObservableObject {
    @Published private(set) var current: AppDialog?
    @Published private(set) var nav = DialogNav(buttons: 1)
    @Published var text = ""
    /// Bumped for every dialog shown (the view resets its text focus).
    @Published private(set) var serial = 0
    private var queue: [AppDialog] = []
    /// Main thread: a dialog appeared (true) / the last one went away (false).
    var onActiveChange: ((Bool) -> Void)?
    /// The window the dialogs are drawn in (nil before it exists: they wait for it).
    var window: () -> NSWindow? = { nil }

    var isActive: Bool { current != nil }

    func present(_ d: AppDialog) {
        // The window was closed (quitting): a system alert is the only place left.
        if let w = window(), !w.isVisible, !w.isMiniaturized {
            let a = Self.runAlert(d)
            d.onResult?(a)
            return
        }
        // The same message again (a failing autosave reports every tick): shown once.
        let same = { (o: AppDialog) in o.title == d.title && o.message == d.message && o.buttons == d.buttons && d.buttons.count == 1 }
        if let c = current, same(c) || queue.contains(where: same) { d.onResult?(DialogAnswer(button: 0, toggleOn: d.toggleOn, text: d.text ?? "")); return }
        if current == nil { show(d) } else { queue.append(d) }
    }

    private func show(_ d: AppDialog) {
        let wasActive = current != nil
        withAnimation(QMStyle.anim) {
            current = d
            nav = DialogNav(buttons: d.buttons.count, defaultIndex: d.defaultIndex, cancelIndex: d.cancelIndex,
                            toggle: d.toggle == nil ? nil : d.toggleOn)
            text = d.text ?? ""
            serial &+= 1
        }
        if !wasActive { onActiveChange?(true) }
    }

    /// Controller / keyboard navigation (AppModel.handleNav routes everything here first).
    func handle(_ n: NavInput) {
        guard current != nil else { return }
        let input: DialogNav.Input
        switch n {
        case .up: input = .up
        case .down: input = .down
        case .left: input = .left
        case .right: input = .right
        case .confirm: input = .confirm
        case .back, .escape: input = .cancel
        // The L+R chord, the pill's Esc, pages, X / Y: nothing while a dialog has the focus.
        case .menu, .pagePrev, .pageNext, .x, .y, .options, .search: return
        }
        var nv = nav
        let outcome = nv.handle(input)
        withAnimation(QMStyle.anim) { nav = nv }
        if case .answer(let i) = outcome { answer(i) }
    }

    func hover(_ i: Int) {
        guard nav.focus != i else { return }
        var nv = nav
        nv.setFocus(i)
        withAnimation(QMStyle.anim) { nav = nv }
    }

    func flipToggle() {
        var nv = nav
        nv.flipToggle()
        withAnimation(QMStyle.anim) { nav = nv }
    }

    /// The text field's Return.
    func submit() { answer(nav.focus == DialogNav.toggleRow ? (current?.defaultIndex ?? 0) : nav.focus) }
    func cancel() { answer(nav.cancelIndex) }

    func answer(_ i: Int) {
        guard let d = current else { return }
        let result = DialogAnswer(button: i, toggleOn: nav.toggleOn, text: text)
        if queue.isEmpty {
            withAnimation(QMStyle.anim) { current = nil }
        } else {
            show(queue.removeFirst())
        }
        d.onResult?(result)
        if current == nil { onActiveChange?(false) }
    }

    /// No window to draw in (quitting after the window was closed): the same dialog as an NSAlert.
    static func runAlert(_ d: AppDialog) -> DialogAnswer {
        let a = NSAlert()
        a.alertStyle = d.kind == .info ? .informational : .warning
        a.messageText = d.title
        a.informativeText = d.message
        // NSAlert puts the first button on the right and makes it the default (Return).
        let order = [d.defaultIndex] + d.buttons.indices.filter { $0 != d.defaultIndex }
        for i in order where d.buttons.indices.contains(i) {
            let b = a.addButton(withTitle: d.buttons[i])
            if i == d.destructiveIndex { b.hasDestructiveAction = true }
            if i == (d.cancelIndex ?? d.buttons.count - 1) && i != d.defaultIndex { b.keyEquivalent = "\u{1b}" }
        }
        var toggle: NSButton?
        var field: NSTextField?
        if let t = d.toggle {
            let b = NSButton(checkboxWithTitle: t, target: nil, action: nil)
            b.state = d.toggleOn ? .on : .off
            a.accessoryView = b
            toggle = b
        } else if let t = d.text {
            let f = NSTextField(string: t)
            f.frame = NSRect(x: 0, y: 0, width: 260, height: 24)
            a.accessoryView = f
            a.window.initialFirstResponder = f
            field = f
        }
        let r = a.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
        let button = order.indices.contains(r) ? order[r] : (d.cancelIndex ?? d.buttons.count - 1)
        return DialogAnswer(button: button, toggleOn: toggle.map { $0.state == .on } ?? d.toggleOn, text: field?.stringValue ?? d.text ?? "")
    }
}
