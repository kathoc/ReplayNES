// Quick Menu rules (docs/design/UI_REDESIGN.md): the L+R chord (shared core), the paused seek bar's
// A tap, the layout-5 binding migration, page structure / limits and the match with the core's
// menu tree, focus movement and the menu pill's placement and fading.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

/// The L+R chord comes from the shared core (rnf_chord_*, tested in tests/test_frontend_menu.cpp);
/// this checks the Swift wrapper and the default "hk.menu" binding it is configured from.
final class ChordDetectorTests: XCTestCase {
    private let w = ChordDetector.window
    private let l = "gc0:leftShoulder", r = "gc0:rightShoulder", combo = "gc0:leftShoulder+gc0:rightShoulder"

    private func detector() -> ChordDetector {
        let c = ChordDetector()
        let config = InputCatalog.parse(InputCatalog.defaultConfigJSON())!
        XCTAssertEqual(c.configure(config.bindings), 1, "the defaults bind pad 1's L+R to hk.menu")
        return c
    }

    func testBothWithinWindowIsChordEitherOrder() {
        for (first, second) in [(l, r), (r, l)] {
            let c = detector()
            XCTAssertEqual(c.feed(first, down: true, now: 0).events, [])
            XCTAssertEqual(c.feed(second, down: true, now: w * 0.6).events, [.comboDown(combo)])
            XCTAssertEqual(c.feed(first, down: false, now: 0.5).events, [])
            XCTAssertEqual(c.feed(second, down: false, now: 0.6).events, [.comboUp(combo)])
            XCTAssertNil(c.nextDeadline)
        }
    }

    func testTapAloneFiresOnRelease() {
        let c = detector()
        XCTAssertTrue(c.feed(r, down: true, now: 1).taken)
        XCTAssertEqual(c.nextDeadline ?? 0, 1 + w, accuracy: 1e-9)
        XCTAssertEqual(c.feed(r, down: false, now: 1.05).events, [.alone(r, down: true), .alone(r, down: false)])
        XCTAssertNil(c.nextDeadline)
    }

    func testHoldAloneFiresAtTimeout() {
        let c = detector()
        _ = c.feed(l, down: true, now: 0)
        XCTAssertEqual(c.tick(now: w * 0.5), [])
        XCTAssertEqual(c.tick(now: w), [.alone(l, down: true)])
        XCTAssertEqual(c.tick(now: w * 3), [])
        XCTAssertEqual(c.feed(l, down: false, now: 1).events, [.alone(l, down: false)])
    }

    func testOtherInputsPassAndRepeatOrResetFireNothing() {
        let c = detector()
        let other = c.feed("gc0:face.south", down: true, now: 0)
        XCTAssertFalse(other.taken)
        XCTAssertFalse(c.isMember("gc1:leftShoulder"), "only pad 1 has the chord by default")
        _ = c.feed(l, down: true, now: 0)
        XCTAssertEqual(c.feed(l, down: true, now: 0.01).events, [])
        c.reset()
        XCTAssertNil(c.nextDeadline)
        XCTAssertEqual(c.feed(l, down: false, now: 0.02).events, [])
    }
}

/// The paused seek bar's "A = resume" is a tap: holding A / B for a frame advance never resumes.
final class ConfirmTapTests: XCTestCase {
    func testTapResumesButHoldingForAStepDoesNot() {
        var t = ConfirmTap()
        t.cancel(); t.press("gc0:face.south")
        XCTAssertTrue(t.release("gc0:face.south"))
        t.cancel(); t.press("gc0:face.south")
        t.cancel()   // D-pad → steps a frame while it is held
        XCTAssertFalse(t.release("gc0:face.south"))
        XCTAssertFalse(t.release("gc0:face.south"), "pressed before pausing: its release does nothing")
        t.press("gc1:face.south"); t.clear()
        XCTAssertFalse(t.release("gc1:face.south"))
    }
}

/// bindings.json from 0.4.0 (layout 4, no "hk.menu") migrates to layout 5 on load.
final class MenuMigrationTests: XCTestCase {
    func testLayout4FileGetsTheQuickMenuBindings() {
        let defaults = InputCatalog.parse(InputCatalog.defaultConfigJSON())!
        let old = defaults.bindings.filter { $0.action != "hk.menu" }
        let plan = InputCatalog.menuMigration(InputCatalog.Config(bindings: old, turboPeriod: defaults.turboPeriod,
                                                                  turboDuty: defaults.turboDuty, socd: defaults.socd,
                                                                  analogThreshold: defaults.analogThreshold))
        XCTAssertTrue(plan.unbind.isEmpty)
        XCTAssertEqual(Set(plan.bind.map { "\($0.0)=\($0.1)" }),
                       ["gc0:leftShoulder+gc0:rightShoulder=hk.menu", "kb:53=hk.menu"])
        XCTAssertEqual(InputCatalog.controllerLayoutVersion, 5)
        XCTAssertTrue(InputCatalog.isCombo("gc0:leftShoulder+gc0:rightShoulder"))
        XCTAssertFalse(InputCatalog.isCombo("kb:53"))
        // Esc already bound to something else: kept, only the pad chord is added.
        let esc = InputCatalog.menuMigration(InputCatalog.Config(bindings: old + [(input: "kb:53", action: "hk.pause")],
                                                                 turboPeriod: 4, turboDuty: 2, socd: "neutral", analogThreshold: 0.5))
        XCTAssertEqual(esc.bind.map(\.0), ["gc0:leftShoulder+gc0:rightShoulder"])
    }
}

/// The SwiftUI Quick Menu keeps its own page structure (QMPage), checked here against the shared
/// core's menu tree (frontend/src/menu.cpp, drawn by the Linux / Windows frontends).
final class CoreMenuTreeTests: XCTestCase {
    private var menu: OpaquePointer!

    override func setUp() {
        let f = RNF_MENU_FEATURE_EXPORT.rawValue | RNF_MENU_FEATURE_STREAM.rawValue | RNF_MENU_FEATURE_CRT.rawValue
            | RNF_MENU_FEATURE_UPDATES.rawValue
        menu = rnf_menu_new(UInt32(f))
    }

    override func tearDown() { rnf_menu_free(menu) }

    private func page(_ i: Int) -> rnf_menu_page_info {
        var info = rnf_menu_page_info()
        XCTAssertNotEqual(rnf_menu_page_get(menu, i, &info), 0)
        return info
    }

    private func items(_ i: Int) -> [rnf_menu_item_info] {
        (0..<page(i).count).compactMap { j in
            var it = rnf_menu_item_info()
            return rnf_menu_item_get(menu, i, j, &it) != 0 ? it : nil
        }
    }

    /// Core pages macOS deliberately has no page for (see QMPage.coreID).
    private let notOnMac: Set<String> = ["system.detail", "library.projects"]

    func testTopLevelTilesMatch() {
        let quick = Int(rnf_menu_page_find(menu, "quick"))
        XCTAssertGreaterThanOrEqual(quick, 0)
        XCTAssertEqual(items(quick).map { String(cString: $0.id) }, QMPage.topTileIDs)
        XCTAssertEqual(QMPage.top.capacity, QMPage.topTileIDs.count)
    }

    func testEveryCorePageHasASwiftPageAndBack() {
        let swiftIDs = Set(QMPage.allCases.compactMap(\.coreID))
        for i in 0..<rnf_menu_page_count(menu) {
            let id = String(cString: page(i).id)
            if notOnMac.contains(id) { continue }
            XCTAssertTrue(swiftIDs.contains(id), "core page \(id) has no QMPage")
        }
        for p in QMPage.allCases {
            guard let id = p.coreID else { continue }
            XCTAssertGreaterThanOrEqual(rnf_menu_page_find(menu, id), 0, "\(p): core page \(id) missing")
        }
    }

    func testParentsAndSettingsPagesMatch() {
        // Core parents: the page whose PAGE item opens it; the settings group's pages are all
        // reached from the Quick Menu (its Settings tile opens the first, L / R the others).
        var parent: [String: String] = [:]
        var settings: [String] = []
        for i in 0..<rnf_menu_page_count(menu) {
            let pid = String(cString: page(i).id)
            if String(cString: page(i).group) == "settings" { settings.append(pid) }
            for it in items(i) where it.kind == RNF_MENU_ITEM_PAGE { parent[String(cString: it.target)] = pid }
        }
        for s in settings { parent[s] = "quick" }
        XCTAssertEqual(settings, QMPage.settingsTabs.compactMap(\.coreID), "settings pages and their L / R order")
        for p in QMPage.allCases {
            guard let id = p.coreID, let up = p.parent, let upID = up.coreID else { continue }
            XCTAssertEqual(parent[id], upID, "\(p) is under \(up) on macOS")
        }
    }

    func testLayoutsMatchPageKinds() {
        for p in QMPage.allCases {
            guard let id = p.coreID else { continue }
            let i = Int(rnf_menu_page_find(menu, id))
            guard i >= 0 else { continue }
            let kind = page(i).kind
            let ok: Bool
            switch (kind, p.layout) {
            case (RNF_MENU_PAGE_TILES, .tiles), (RNF_MENU_PAGE_TILES, .actions): ok = true
            case (RNF_MENU_PAGE_SETTINGS, .rows): ok = true
            case (RNF_MENU_PAGE_CARDS, .cards): ok = true
            case (RNF_MENU_PAGE_LIST, .cards), (RNF_MENU_PAGE_LIST, .rows): ok = true   // takes as thumbnails
            case (RNF_MENU_PAGE_CUSTOM, .custom): ok = true
            default: ok = false
            }
            XCTAssertTrue(ok, "\(p): \(p.layout) for core kind \(kind.rawValue)")
            if kind == RNF_MENU_PAGE_TILES || kind == RNF_MENU_PAGE_SETTINGS {
                XCTAssertLessThanOrEqual(page(i).count, p.capacity, "\(p): the core's items fit")
            }
        }
        XCTAssertEqual(QMPage.practice.capacity, Int(RNF_MENU_MAX_CARDS))
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
