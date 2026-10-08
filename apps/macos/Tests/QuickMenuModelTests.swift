// Quick Menu rules (docs/design/UI_REDESIGN.md): the L+R chord, page structure / limits, focus
// movement and the menu pill's placement and fading.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class ShoulderChordTests: XCTestCase {
    private let w = SwiftShoulderChord.window

    func testBothWithinWindowIsChordEitherOrder() {
        for (first, second) in [(Shoulder.left, Shoulder.right), (.right, .left)] {
            let c = SwiftShoulderChord()
            XCTAssertEqual(c.update(first, down: true, now: 0), [])
            XCTAssertEqual(c.update(second, down: true, now: w * 0.6), [.chord])
            // Releases of a chord are swallowed; nothing is pending afterwards.
            XCTAssertEqual(c.update(first, down: false, now: 0.5), [])
            XCTAssertEqual(c.update(second, down: false, now: 0.6), [])
            XCTAssertNil(c.nextDeadline)
        }
    }

    func testTapAloneFiresOnRelease() {
        let c = SwiftShoulderChord()
        XCTAssertEqual(c.update(.right, down: true, now: 1), [])
        XCTAssertEqual(c.nextDeadline, 1 + w)
        XCTAssertEqual(c.update(.right, down: false, now: 1.05), [.alone(.right, down: true), .alone(.right, down: false)])
        XCTAssertNil(c.nextDeadline)
    }

    func testHoldAloneFiresAtTimeout() {
        let c = SwiftShoulderChord()
        _ = c.update(.left, down: true, now: 0)
        XCTAssertEqual(c.tick(now: w * 0.5), [])
        XCTAssertEqual(c.tick(now: w), [.alone(.left, down: true)])
        XCTAssertEqual(c.tick(now: w * 3), [])
        XCTAssertEqual(c.update(.left, down: false, now: 1), [.alone(.left, down: false)])
    }

    func testOtherShoulderAfterTimeoutIsNotAChord() {
        let c = SwiftShoulderChord()
        _ = c.update(.left, down: true, now: 0)
        // The late R first lets the expired L fire, then waits on its own.
        XCTAssertEqual(c.update(.right, down: true, now: w + 0.05), [.alone(.left, down: true)])
        XCTAssertEqual(c.update(.right, down: false, now: w + 0.08), [.alone(.right, down: true), .alone(.right, down: false)])
    }

    func testRepeatAndResetFireNothing() {
        let c = SwiftShoulderChord()
        _ = c.update(.left, down: true, now: 0)
        XCTAssertEqual(c.update(.left, down: true, now: 0.01), [])
        c.reset()
        XCTAssertNil(c.nextDeadline)
        XCTAssertEqual(c.update(.left, down: false, now: 0.02), [])
    }
}

final class QuickMenuModelTests: XCTestCase {
    func testTopLevelHasSixTilesInOneRow() {
        XCTAssertEqual(QMPage.top.layout, .tiles(columns: 6))
        XCTAssertEqual(QMPage.top.capacity, 6)
        XCTAssertEqual(QMPage.allCases.filter { $0.parent == .top }.count, 4 + QMPage.settingsTabs.count)
    }

    func testPagesHoldAtMostSixItemsExceptPracticeSlots() {
        for p in QMPage.allCases {
            if p == .practice {
                XCTAssertEqual(p.layout, .cards(columns: 4, rows: 2), "8 A/B slots as 4x2 cards")
            } else {
                XCTAssertLessThanOrEqual(p.capacity, 6, "\(p)")
            }
        }
    }

    func testDepthIsAtMostThreeLevels() {
        for p in QMPage.allCases { XCTAssertLessThanOrEqual(p.depth, 2, "\(p): Quick Menu › … at most 3 levels") }
        XCTAssertEqual(QMPage.crtDetail.path, [.top, .display, .crtDetail])
        XCTAssertEqual(QMPage.controller.path, [.top, .controls, .controller])
        XCTAssertTrue(QMPage.crtDetail.isSettings)
        XCTAssertFalse(QMPage.takes.isSettings)
    }

    func testGridMovement() {
        // 6 items, 3 columns (two rows).
        XCTAssertNil(QuickMenuNav.move(0, count: 6, columns: 3, .left))
        XCTAssertEqual(QuickMenuNav.move(0, count: 6, columns: 3, .right), 1)
        XCTAssertNil(QuickMenuNav.move(2, count: 6, columns: 3, .right))
        XCTAssertEqual(QuickMenuNav.move(1, count: 6, columns: 3, .down), 4)
        XCTAssertNil(QuickMenuNav.move(4, count: 6, columns: 3, .down))
        XCTAssertEqual(QuickMenuNav.move(4, count: 6, columns: 3, .up), 1)
        // Shorter last row: down lands on its last item.
        XCTAssertEqual(QuickMenuNav.move(2, count: 5, columns: 3, .down), 4)
        XCTAssertNil(QuickMenuNav.move(4, count: 5, columns: 3, .right))
        // Rows (one column).
        XCTAssertEqual(QuickMenuNav.move(0, count: 6, columns: 1, .down), 1)
        XCTAssertNil(QuickMenuNav.move(5, count: 6, columns: 1, .down))
        XCTAssertNil(QuickMenuNav.move(0, count: 0, columns: 1, .down))
    }

    func testPaging() {
        XCTAssertEqual(QuickMenuNav.pageCount(items: 0, perPage: 6), 1)
        XCTAssertEqual(QuickMenuNav.pageCount(items: 13, perPage: 6), 3)
        XCTAssertEqual(QuickMenuNav.pageRange(items: 13, perPage: 6, page: 2), 12..<13)
        XCTAssertEqual(QuickMenuNav.pageRange(items: 13, perPage: 6, page: 9), 12..<13)
    }
}

final class MenuPillLayoutTests: XCTestCase {
    let pill = CGSize(width: 200, height: 54)

    func testTopRightCorner() {
        let r = MenuPillLayout.rect(areaSize: CGSize(width: 2000, height: 1200), pillSize: pill, scale: 2)
        XCTAssertEqual(r, CGRect(x: 2000 - 24 - 200, y: 24, width: 200, height: 54))
    }

    func testFadesOnlyOverThePicture() {
        XCTAssertEqual(MenuPillLayout.alpha(elapsed: 10, overPicture: false), 1)
        XCTAssertEqual(MenuPillLayout.alpha(elapsed: 2.9, overPicture: true), 1)
        XCTAssertEqual(MenuPillLayout.alpha(elapsed: 10, overPicture: true), MenuPillLayout.dimmedAlpha, accuracy: 1e-9)
        let mid = MenuPillLayout.alpha(elapsed: MenuPillLayout.fadeDelay + MenuPillLayout.fadeDuration / 2, overPicture: true)
        XCTAssertTrue(mid < 1 && mid > MenuPillLayout.dimmedAlpha)
    }

    func testLetterboxedPictureLeavesTheCornerFree() {
        let area = CGSize(width: 2000, height: 1200)
        let r = MenuPillLayout.rect(areaSize: area, pillSize: pill, scale: 2)
        // Integer scale 4x: 1024 wide, centred.
        XCTAssertFalse(MenuPillLayout.overPicture(r, picture: CGRect(x: 488, y: 120, width: 1024, height: 960)))
        // FILL: the picture reaches the top edge across the corner.
        XCTAssertTrue(MenuPillLayout.overPicture(r, picture: CGRect(x: 0, y: 0, width: 2000, height: 1200)))
    }

    func testGlyphFollowsTheControllerFamily() {
        XCTAssertEqual(MenuPillLayout.glyph(hasController: false, playStation: false), "Esc")
        XCTAssertEqual(MenuPillLayout.glyph(hasController: true, playStation: false), "L+R")
        XCTAssertEqual(MenuPillLayout.glyph(hasController: true, playStation: true), "L1+R1")
    }
}
