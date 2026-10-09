// In-window dialogs (docs/design/UI_REDESIGN.md, "Dialogs and notices"): controller / keyboard
// navigation inside a dialog (DialogNav, Sources/Core/DialogNav.swift).
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class DialogNavTests: XCTestCase {
    func testButtonsMoveWithoutWrapping() {
        var n = DialogNav(buttons: 3, cancelIndex: 2)
        XCTAssertEqual(n.focus, 0)
        XCTAssertEqual(n.handle(.left), .none)
        XCTAssertEqual(n.handle(.right), .moved)
        XCTAssertEqual(n.handle(.right), .moved)
        XCTAssertEqual(n.focus, 2)
        XCTAssertEqual(n.handle(.right), .none)
        XCTAssertEqual(n.handle(.up), .none, "no on/off row: up does nothing")
        XCTAssertEqual(n.handle(.down), .none)
        XCTAssertEqual(n.focus, 2)
    }

    func testConfirmPressesTheFocusedButton() {
        var n = DialogNav(buttons: 3, cancelIndex: 2)
        _ = n.handle(.right)
        XCTAssertEqual(n.handle(.confirm), .answer(1))
    }

    func testCancelPressesTheCancelButtonWhereverTheFocusIs() {
        var n = DialogNav(buttons: 3, cancelIndex: 2)
        XCTAssertEqual(n.handle(.cancel), .answer(2))
        var quit = DialogNav(buttons: 2, cancelIndex: 0)   // "Don't Quit" / "Quit Anyway"
        _ = quit.handle(.right)
        XCTAssertEqual(quit.handle(.cancel), .answer(0))
        var ok = DialogNav(buttons: 1)
        XCTAssertEqual(ok.handle(.cancel), .answer(0), "a single OK: cancel closes it too")
        XCTAssertEqual(ok.handle(.confirm), .answer(0))
    }

    func testDefaultsAndClamping() {
        let n = DialogNav(buttons: 2)
        XCTAssertEqual(n.cancelIndex, 1, "nil: the last button cancels")
        let reset = DialogNav(buttons: 2, defaultIndex: 1, cancelIndex: 1, toggle: true)
        XCTAssertEqual(reset.focus, 1, "the reset confirmation focuses Cancel")
        XCTAssertTrue(reset.toggleOn)
        let odd = DialogNav(buttons: 0, defaultIndex: 5, cancelIndex: 9)
        XCTAssertEqual(odd.buttonCount, 1)
        XCTAssertEqual(odd.focus, 0)
        XCTAssertEqual(odd.cancelIndex, 0)
    }

    func testToggleRowAboveTheButtons() {
        var n = DialogNav(buttons: 2, defaultIndex: 1, cancelIndex: 1, toggle: true)
        XCTAssertEqual(n.handle(.up), .moved)
        XCTAssertEqual(n.focus, DialogNav.toggleRow)
        XCTAssertEqual(n.handle(.up), .none)
        XCTAssertEqual(n.handle(.confirm), .toggled, "confirm on the row flips it, never answers")
        XCTAssertFalse(n.toggleOn)
        XCTAssertEqual(n.handle(.right), .toggled)
        XCTAssertTrue(n.toggleOn)
        XCTAssertEqual(n.handle(.right), .none)
        XCTAssertEqual(n.handle(.left), .toggled)
        XCTAssertFalse(n.toggleOn)
        XCTAssertEqual(n.handle(.down), .moved)
        XCTAssertEqual(n.focus, 1, "down returns to the button focused before")
        _ = n.handle(.left)
        XCTAssertEqual(n.handle(.confirm), .answer(0))
        XCTAssertFalse(n.toggleOn, "the answer carries the row's state")
    }

    func testCancelFromTheToggleRow() {
        var n = DialogNav(buttons: 2, cancelIndex: 1, toggle: false)
        _ = n.handle(.up)
        XCTAssertEqual(n.handle(.cancel), .answer(1))
    }

    func testMouseFocus() {
        var n = DialogNav(buttons: 3)
        n.setFocus(2)
        XCTAssertEqual(n.focus, 2)
        n.setFocus(7)
        XCTAssertEqual(n.focus, 2, "out of range is ignored")
        n.setFocus(DialogNav.toggleRow)
        XCTAssertEqual(n.focus, 2, "no row to focus")
        var t = DialogNav(buttons: 2, toggle: false)
        t.setFocus(1)
        t.setFocus(DialogNav.toggleRow)
        XCTAssertEqual(t.focus, DialogNav.toggleRow)
        t.flipToggle()
        XCTAssertTrue(t.toggleOn)
        XCTAssertEqual(t.handle(.down), .moved)
        XCTAssertEqual(t.focus, 1)
    }
}
