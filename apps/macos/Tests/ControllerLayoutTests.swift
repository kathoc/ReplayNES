// Positional face buttons, layout migration and the controller diagram (geometry + offscreen render).
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI
import XCTest

final class ControllerLayoutTests: XCTestCase {
    private func set(_ pairs: [(String, String)]) -> Set<String> { Set(pairs.map { "\($0.0)=\($0.1)" }) }
    private func set(_ c: InputCatalog.Config) -> Set<String> { Set(c.bindings.map { "\($0.input)=\($0.action)" }) }
    private var defaults: InputCatalog.Config { InputCatalog.parse(InputCatalog.defaultConfigJSON())! }

    // MARK: defaults

    func testDefaultFaceButtonsArePositional() {
        let b = set(InputCatalog.defaultBindings)
        for (slot, p) in [(0, "p1"), (1, "p2")] {
            XCTAssertTrue(b.contains("gc\(slot):face.east=\(p).a"), "NES A = east (Nintendo A / Xbox B / PS ○)")
            XCTAssertTrue(b.contains("gc\(slot):face.south=\(p).b"), "NES B = south (Nintendo B / Xbox A / PS ✕)")
            XCTAssertTrue(b.contains("gc\(slot):face.north=\(p).turbo_a"))
            XCTAssertTrue(b.contains("gc\(slot):face.west=\(p).turbo_b"))
        }
        XCTAssertFalse(InputCatalog.defaultBindings.contains { b in InputCatalog.legacyFaceNames.contains { b.0.hasSuffix(":" + $0) } },
                       "no GameController (label-based) names in the defaults")
    }

    // MARK: GameController -> position

    func testFamilyFromProductCategory() {
        XCTAssertEqual(ControllerFamily.from(productCategory: "Switch Pro Controller"), .nintendo)
        XCTAssertEqual(ControllerFamily.from(productCategory: "Nintendo Switch Joy-Con (L/R)"), .nintendo)
        XCTAssertEqual(ControllerFamily.from(productCategory: "HID", vendorName: "Pro Controller"), .nintendo)
        XCTAssertEqual(ControllerFamily.from(productCategory: "Xbox One"), .xbox)
        XCTAssertEqual(ControllerFamily.from(productCategory: "DualSense"), .playStation)
        XCTAssertEqual(ControllerFamily.from(productCategory: "DualShock 4"), .playStation)
        XCTAssertEqual(ControllerFamily.from(productCategory: "MFi"), .generic)
    }

    func testGameControllerFaceNamesToPositions() {
        // Nintendo: GameController names by label (buttonA = the "A" button, on the right).
        for cat in ["Switch Pro Controller", "Nintendo Switch Joy-Con (L/R)"] {
            let m = GCFaceMapping.positions(productCategory: cat)
            XCTAssertEqual(m["buttonA"], .east, cat)
            XCTAssertEqual(m["buttonB"], .south, cat)
            XCTAssertEqual(m["buttonX"], .north, cat)
            XCTAssertEqual(m["buttonY"], .west, cat)
        }
        // Everything else: Apple's documented diamond (A bottom, B right, X left, Y top).
        for cat in ["Xbox One", "DualSense", "DualShock 4", "MFi", "HID"] {
            let m = GCFaceMapping.positions(productCategory: cat)
            XCTAssertEqual(m["buttonA"], .south, cat)
            XCTAssertEqual(m["buttonB"], .east, cat)
            XCTAssertEqual(m["buttonX"], .west, cat)
            XCTAssertEqual(m["buttonY"], .north, cat)
        }
        // A single sideways Joy-Con.
        let single = GCFaceMapping.positions(productCategory: "Nintendo Switch Joy-Con (R)")
        XCTAssertEqual(Set(single.values), Set(FacePosition.allCases))
        XCTAssertEqual(single["buttonA"], .south)
        XCTAssertEqual(GCFaceMapping.label(fromSymbol: "a.circle"), "A")
        XCTAssertEqual(GCFaceMapping.label(fromSymbol: "xmark.circle.fill"), "✕")
        XCTAssertNil(GCFaceMapping.label(fromSymbol: nil))
    }

    /// The reported bug: on a Pro Controller the button printed "A" must be NES A (and "B" NES B),
    /// while on Xbox the right button (printed "B") is NES A. Through the engine pipeline.
    func testPrintedButtonsReachTheRightNESButtons() {
        func nesBits(category: String, gcButton: String) -> UInt8 {
            let h = rn_input_new()!
            defer { rn_input_free(h) }
            XCTAssertEqual(rn_input_load_json(h, InputCatalog.defaultConfigJSON()), RN_OK)
            let pos = GCFaceMapping.positions(productCategory: category)[gcButton]!
            rn_input_set_pressed(h, "gc0:" + pos.element, 1)
            var p1: UInt8 = 0, p2: UInt8 = 0
            rn_input_sample_game(h, 0, &p1, &p2)
            return p1
        }
        XCTAssertEqual(nesBits(category: "Switch Pro Controller", gcButton: "buttonA"), UInt8(RN_BTN_A), "Pro Controller A = NES A")
        XCTAssertEqual(nesBits(category: "Switch Pro Controller", gcButton: "buttonB"), UInt8(RN_BTN_B), "Pro Controller B = NES B")
        XCTAssertEqual(nesBits(category: "Xbox One", gcButton: "buttonB"), UInt8(RN_BTN_A), "Xbox B (right) = NES A")
        XCTAssertEqual(nesBits(category: "Xbox One", gcButton: "buttonA"), UInt8(RN_BTN_B), "Xbox A (bottom) = NES B")
        XCTAssertEqual(nesBits(category: "DualSense", gcButton: "buttonB"), UInt8(RN_BTN_A), "PS ○ = NES A")
    }

    // MARK: migration (layout 2 -> 3)

    /// The defaults as saved by 0.2.0 (GameController face names).
    private var layout2Defaults: InputCatalog.Config {
        var c = defaults
        c.bindings.removeAll { $0.input.contains(":face.") }
        c.bindings += (InputCatalog.legacyFaceDefaults(slot: 0) + InputCatalog.legacyFaceDefaults(slot: 1)).map { (input: $0.0, action: $0.1) }
        return c
    }

    private func applying(_ plan: (unbind: [(String, String)], bind: [(String, String)]), to c: InputCatalog.Config) -> InputCatalog.Config {
        var c = c
        for u in plan.unbind { c.bindings.removeAll { $0.input == u.0 && $0.action == u.1 } }
        c.bindings += plan.bind.map { (input: $0.0, action: $0.1) }
        return c
    }

    func testUntouchedLayout2DefaultsBecomePositionalDefaults() {
        let m = InputCatalog.faceLayoutMigration(layout2Defaults)
        XCTAssertEqual(m.unbind.count, 8)
        XCTAssertEqual(set(applying(m, to: layout2Defaults)), set(defaults))
        // Already migrated: nothing to do.
        XCTAssertTrue(InputCatalog.faceLayoutMigration(defaults).bind.isEmpty)
    }

    func testCustomisedFaceButtonsAreKeptPerSlot() {
        // Slot 0 swapped by hand (a Pro Controller owner working around the bug); slot 1 untouched.
        var c = layout2Defaults
        c.bindings.removeAll { $0.input == "gc0:buttonA" || $0.input == "gc0:buttonB" }
        c.bindings += [(input: "gc0:buttonA", action: "p1.a"), (input: "gc0:buttonB", action: "p1.b")]
        let m = InputCatalog.faceLayoutMigration(c)
        XCTAssertTrue(m.unbind.allSatisfy { $0.0.hasPrefix("gc1:") }, "customised slot 0 untouched")
        XCTAssertEqual(m.bind.count, 4)
        let migrated = applying(m, to: c)
        XCTAssertTrue(migrated.bindings.contains { $0.input == "gc0:buttonA" && $0.action == "p1.a" })

        // When the Pro Controller attaches, its names are resolved to ITS positions: the button
        // printed "A" keeps doing NES A, exactly as before the update.
        let pro = InputCatalog.legacyFaceTranslation(migrated, slot: 0, positions: GCFaceMapping.positions(productCategory: "Switch Pro Controller"))
        let afterPro = set(applying(pro, to: migrated))
        XCTAssertTrue(afterPro.contains("gc0:face.east=p1.a"))
        XCTAssertTrue(afterPro.contains("gc0:face.south=p1.b"))
        XCTAssertTrue(afterPro.contains("gc0:face.west=p1.turbo_a"), "the button printed Y keeps turbo A")
        XCTAssertTrue(afterPro.contains("gc0:face.north=p1.turbo_b"), "the button printed X keeps turbo B")
        XCTAssertFalse(afterPro.contains { $0.hasPrefix("gc0:button") })
        // The same file used with an Xbox pad: Xbox A (bottom) keeps NES A.
        let xbox = InputCatalog.legacyFaceTranslation(migrated, slot: 0, positions: GCFaceMapping.positions(productCategory: "Xbox One"))
        XCTAssertTrue(set(applying(xbox, to: migrated)).contains("gc0:face.south=p1.a"))
        // Nothing left to translate afterwards.
        XCTAssertTrue(InputCatalog.legacyFaceTranslation(applying(pro, to: migrated), slot: 0, positions: [:]).bind.isEmpty)
    }

    func testFullChainFrom01x() {
        // 0.1.x file: old hotkeys + GameController face names.
        var c = layout2Defaults
        c.bindings.removeAll { b in InputCatalog.controllerHotkeys.contains { $0.0 == b.input && $0.1 == b.action } }
        c.bindings += InputCatalog.legacyControllerHotkeys.map { (input: $0.0, action: $0.1) }
        c = applying(InputCatalog.controllerLayoutMigration(c), to: c)
        c = applying(InputCatalog.faceLayoutMigration(c), to: c)
        XCTAssertEqual(set(c), set(defaults))
    }

    func testPerControllerReset() {
        var c = defaults
        c.bindings.removeAll { $0.input == "gc0:face.east" }
        c.bindings += [(input: "gc0:face.east", action: "hk.bookmark"), (input: "gc1:face.east", action: "hk.save"), (input: "kb:1", action: "hk.save")]
        let reset = applying(InputCatalog.controllerResetPlan(c, slot: 0), to: c)
        let s = set(reset)
        XCTAssertTrue(s.contains("gc0:face.east=p1.a"))
        XCTAssertFalse(s.contains("gc0:face.east=hk.bookmark"))
        XCTAssertTrue(s.contains("gc1:face.east=hk.save"), "other controllers untouched")
        XCTAssertTrue(s.contains("kb:1=hk.save"), "keyboard untouched")
        XCTAssertEqual(s.filter { $0.hasPrefix("gc0:") }, set(InputCatalog.defaultBindings.filter { $0.0.hasPrefix("gc0:") }))
    }

    // MARK: diagram

    /// Every element InputManager reports (besides face positions) has a place on every diagram,
    /// with the family's printed label.
    func testDiagramElementsPerFamily() {
        var expected: Set<String> = ["leftShoulder", "rightShoulder", "leftTrigger", "rightTrigger", "menu", "options", "home",
                                     "leftThumb", "rightThumb"]
        for p in FacePosition.allCases { expected.insert(p.element) }
        for g in ["dpad", "lstick", "rstick"] { for d in ["up", "down", "left", "right"] { expected.insert("\(g).\(d)") } }
        let defaultElements = Set(InputCatalog.defaultBindings.filter { $0.0.hasPrefix("gc") }.map { String($0.0.split(separator: ":")[1]) })
        XCTAssertTrue(defaultElements.isSubset(of: expected))

        let canvas = CGRect(origin: .zero, size: ControllerDiagramLayout.canvas)
        for f in ControllerFamily.allCases {
            let l = ControllerDiagramLayout(family: f)
            XCTAssertEqual(Set(l.bindableElements), expected, f.rawValue)
            XCTAssertEqual(l.bindableElements.count, expected.count, "no duplicates")
            for e in l.elements { XCTAssertTrue(canvas.contains(e.frame), "\(f) \(e.element) inside canvas") }
            // Clickable areas never overlap (stick click vs. its direction arrows included).
            for (i, a) in l.elements.enumerated() {
                for b in l.elements[(i + 1)...] {
                    XCTAssertFalse(a.frame.intersects(b.frame), "\(f): \(a.element) overlaps \(b.element)")
                }
            }
            // Face diamond really is a diamond: east right of west, north above south.
            let pos = { (p: FacePosition) in l.element(p.element)!.center }
            XCTAssertGreaterThan(pos(.east).x, pos(.west).x)
            XCTAssertLessThan(pos(.north).y, pos(.south).y)
        }
        XCTAssertEqual(ControllerFamily.nintendo.label("face.east"), "A")
        XCTAssertEqual(ControllerFamily.nintendo.label("face.south"), "B")
        XCTAssertEqual(ControllerFamily.xbox.label("face.east"), "B")
        XCTAssertEqual(ControllerFamily.playStation.label("face.east"), "○")
        XCTAssertEqual(ControllerFamily.nintendo.label("leftTrigger"), "ZL")
        XCTAssertEqual(ControllerFamily.xbox.label("rightTrigger"), "RT")
        XCTAssertEqual(ControllerAssignments.title(element: "face.east", family: .nintendo), "右ボタン（A）")
    }

    func testDiagramBadgesShowGameButtonsAndHotkeys() {
        let c = defaults
        XCTAssertEqual(ControllerAssignments.badge(element: "face.east", slot: 0, config: c), "A")
        XCTAssertEqual(ControllerAssignments.badge(element: "face.north", slot: 0, config: c), "連射A")
        XCTAssertEqual(ControllerAssignments.badge(element: "rightTrigger", slot: 0, config: c), "巻き戻し")
        XCTAssertEqual(ControllerAssignments.badge(element: "leftTrigger", slot: 0, config: c), "早送り")
        XCTAssertEqual(ControllerAssignments.badge(element: "leftShoulder", slot: 0, config: c), "スロー")
        XCTAssertEqual(ControllerAssignments.badge(element: "rightShoulder", slot: 0, config: c), "一時停止")
        XCTAssertEqual(ControllerAssignments.badge(element: "menu", slot: 0, config: c), "START")
        XCTAssertNil(ControllerAssignments.badge(element: "home", slot: 0, config: c))
        XCTAssertEqual(ControllerAssignments.group("dpad", slot: 0, config: c), .movement("移動"))
        XCTAssertEqual(ControllerAssignments.group("lstick", slot: 0, config: c), .movement("移動"))
        XCTAssertEqual(ControllerAssignments.group("rstick", slot: 0, config: c), ControllerAssignments.GroupSummary.none)
        // Pad 2 drives player 2: no "2P" prefix there, hotkeys only on pad 1.
        XCTAssertEqual(ControllerAssignments.badge(element: "face.east", slot: 1, config: c), "A")
        XCTAssertNil(ControllerAssignments.badge(element: "rightTrigger", slot: 1, config: c))
        XCTAssertEqual(ControllerAssignments.shortLabel("p2.a", slot: 0), "2P A")
        var custom = c
        custom.bindings.removeAll { $0.input == "gc0:dpad.up" }
        custom.bindings.append((input: "gc0:dpad.up", action: "hk.bookmark"))
        XCTAssertEqual(ControllerAssignments.group("dpad", slot: 0, config: custom), .custom)
    }

    /// Renders every family to PNG (RN_DIAGRAM_DIR or the temp dir) so the layout can be looked at.
    @MainActor
    func testRenderDiagrams() throws {
        let dir = ProcessInfo.processInfo.environment["RN_DIAGRAM_DIR"].map { URL(fileURLWithPath: $0) }
            ?? FileManager.default.temporaryDirectory.appendingPathComponent("replaynes-diagrams")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var custom = defaults
        custom.bindings.removeAll { $0.input.hasPrefix("gc0:dpad.") || $0.input == "gc0:leftThumb" }
        custom.bindings += [(input: "gc0:dpad.up", action: "hk.bookmark"), (input: "gc0:dpad.left", action: "hk.step_back"),
                            (input: "gc0:dpad.right", action: "hk.frame_advance"), (input: "gc0:dpad.down", action: "p1.select"),
                            (input: "gc0:leftThumb", action: "hk.save"), (input: "gc0:rightThumb", action: "hk.toggle_mode"),
                            (input: "gc0:rstick.up", action: "p2.up"), (input: "gc0:home", action: "hk.undo_take")]
        let cases: [(String, ControllerFamily, InputCatalog.Config, Set<String>)] = [
            ("nintendo", .nintendo, defaults, ["gc0:face.east", "gc0:lstick.left"]),
            ("xbox", .xbox, defaults, []),
            ("playstation", .playStation, defaults, ["gc0:rightTrigger"]),
            ("generic", .generic, defaults, []),
            ("nintendo-custom", .nintendo, custom, ["gc0:dpad.left"]),
            ("playstation-custom", .playStation, custom, []),
        ]
        for (name, family, config, pressed) in cases {
            let view = ControllerDiagramView(family: family, slot: 0, config: config, pressed: pressed)
                .padding(10).background(Color.white).environment(\.colorScheme, .light)
            let r = ImageRenderer(content: view)
            r.scale = 2
            let image = try XCTUnwrap(r.cgImage, name)
            XCTAssertEqual(image.width, Int((ControllerDiagramLayout.canvas.width + 20) * 2))
            let png = try XCTUnwrap(NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:]))
            try png.write(to: dir.appendingPathComponent("controller-\(name).png"))
        }
    }
}
