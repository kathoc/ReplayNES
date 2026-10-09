// The controller diagram (SwiftUI offscreen render of the core's geometry). The face-button,
// migration and diagram-geometry rules are tested in tests/test_frontend_input.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI
import XCTest

final class ControllerLayoutTests: XCTestCase {
    private var defaults: InputCatalog.Config { InputCatalog.parse(InputCatalog.defaultConfigJSON())! }

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
                            (input: "gc0:rstick.up", action: "p2.up")]
        let cases: [(String, ControllerFamily, InputCatalog.Config, Set<String>)] = [
            ("nintendo", .nintendo, defaults, ["gc0:face.east", "gc0:lstick.left"]),
            ("xbox", .xbox, defaults, []),
            ("playstation", .playStation, defaults, ["gc0:rightTrigger"]),
            ("generic", .generic, defaults, []),
            ("steamdeck", .steamDeck, defaults, ["gc0:face.south"]),
            ("nintendo-custom", .nintendo, custom, ["gc0:dpad.left"]),
            ("playstation-custom", .playStation, custom, []),
        ]
        for (name, family, config, pressed) in cases {
            let view = ControllerDiagramView(family: family, slot: 0, config: config, pressed: pressed, focused: "face.east")
                .padding(10).background(Color(red: 0.08, green: 0.08, blue: 0.09)).environment(\.colorScheme, .dark)
            let r = ImageRenderer(content: view)
            r.scale = 2
            let image = try XCTUnwrap(r.cgImage, name)
            XCTAssertEqual(image.width, Int((ControllerDiagramLayout.canvas.width + 20) * 2))
            let png = try XCTUnwrap(NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:]))
            try png.write(to: dir.appendingPathComponent("controller-\(name).png"))
        }
    }

    /// The outline comes from the core (no grips): one body for every family; the Steam Deck adds
    /// its screen and two trackpads, PlayStation its touchpad. Everything stays on the canvas.
    func testDecor() {
        let canvas = CGRect(origin: .zero, size: ControllerDiagramLayout.canvas)
        for f in ControllerFamily.allCases {
            let d = ControllerDiagramLayout(family: f).decor
            XCTAssertEqual(d.first?.kind, .body, "\(f)")
            XCTAssertEqual(d.filter { $0.kind == .body }.count, 1, "\(f)")
            for x in d { XCTAssertTrue(canvas.contains(x.rect), "\(f): \(x.rect)") }
        }
        let deck = ControllerDiagramLayout(family: .steamDeck).decor
        XCTAssertEqual(deck.filter { $0.kind == .screen }.count, 1)
        XCTAssertEqual(deck.filter { $0.kind == .pad }.count, 2)
        XCTAssertEqual(ControllerDiagramLayout(family: .playStation).decor.filter { $0.kind == .pad }.count, 1)
    }
}
