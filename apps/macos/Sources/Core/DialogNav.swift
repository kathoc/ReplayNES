// In-window dialogs (docs/design/UI_REDESIGN.md, "Dialogs and notices"): what controller /
// keyboard input does inside one. A row of buttons (left / right move, confirm presses the focused
// one, cancel presses the cancel button wherever the focus is) and optionally one on/off row
// above them (up / down move between the row and the buttons; confirm or left / right flip it).
// Pure and tested; the queue and the view are App/Dialogs.swift. The desktop frontends' dialogs
// (apps/desktop/src/ui_dialogs.cpp, ImGui navigation) behave the same.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct DialogNav: Equatable {
    enum Input { case up, down, left, right, confirm, cancel }
    enum Outcome: Equatable { case none, moved, toggled, answer(Int) }

    /// `focus` of the on/off row.
    static let toggleRow = -1

    let buttonCount: Int
    let cancelIndex: Int
    let hasToggle: Bool
    /// A button index, or `toggleRow`.
    private(set) var focus: Int
    private(set) var toggleOn: Bool
    /// The button focused before moving up to the on/off row (down returns to it).
    private var lastButton: Int

    /// `cancelIndex` nil: the last button. `toggle` nil: no on/off row.
    init(buttons: Int, defaultIndex: Int = 0, cancelIndex: Int? = nil, toggle: Bool? = nil) {
        buttonCount = max(1, buttons)
        let clamp = { (i: Int) in min(max(0, i), max(1, buttons) - 1) }
        self.cancelIndex = clamp(cancelIndex ?? buttonCount - 1)
        hasToggle = toggle != nil
        toggleOn = toggle ?? false
        focus = clamp(defaultIndex)
        lastButton = focus
    }

    /// Mouse hover / a click: focus that button (or the on/off row).
    mutating func setFocus(_ i: Int) {
        if i == Self.toggleRow { if hasToggle { focus = i }; return }
        guard (0..<buttonCount).contains(i) else { return }
        focus = i
        lastButton = i
    }

    mutating func flipToggle() {
        guard hasToggle else { return }
        toggleOn.toggle()
    }

    mutating func handle(_ input: Input) -> Outcome {
        switch input {
        case .cancel:
            return .answer(cancelIndex)
        case .confirm:
            if focus == Self.toggleRow { toggleOn.toggle(); return .toggled }
            return .answer(focus)
        case .left, .right:
            if focus == Self.toggleRow {
                let on = input == .right
                guard on != toggleOn else { return .none }
                toggleOn = on
                return .toggled
            }
            let next = focus + (input == .left ? -1 : 1)
            guard (0..<buttonCount).contains(next) else { return .none }
            focus = next
            lastButton = next
            return .moved
        case .up:
            guard hasToggle, focus != Self.toggleRow else { return .none }
            focus = Self.toggleRow
            return .moved
        case .down:
            guard focus == Self.toggleRow else { return .none }
            focus = lastButton
            return .moved
        }
    }
}
