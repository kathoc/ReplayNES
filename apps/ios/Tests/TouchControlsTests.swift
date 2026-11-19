// The ported NESTERM touch controller: layout against NESTERM's CSS (numbers measured from
// web/styles.css in a browser at 390 x 844 and 844 x 390, see apps/ios/docs/TOUCH_PORT.md), the
// pure input.mjs helpers, NESTERM's own controller acceptance checks (scripts/controller-check.mjs,
// same positions and expectations) on the iPhone layout, and the touch bindings.
// Exact step-by-step equality with the JS implementation: BrickboyGoldenTests.
// SPDX-License-Identifier: Apache-2.0 AND GPL-2.0-or-later
import CoreGraphics
import UIKit
import XCTest

private extension TouchGamepad {
    func with(_ g: Geometry) -> TouchGamepad { geometry = g; return self }
}

final class TouchLayoutTests: XCTestCase {
    /// NESTERM's CSS as measured (no AA / BB: those are ReplayNES's).
    static let css = TouchLayout.Options(showAABB: false)
    let portrait = TouchLayout.make(size: CGSize(width: 390, height: 844), safe: TouchInsets(), options: css)
    let landscape = TouchLayout.make(size: CGSize(width: 844, height: 390), safe: TouchInsets(), options: css)
    // iPhone 16 Pro / iPhone 17 (points) with their safe areas.
    let phonePortrait = TouchLayout.make(size: CGSize(width: 402, height: 874), safe: TouchInsets(top: 62, left: 0, bottom: 34, right: 0))
    let phoneLandscape = TouchLayout.make(size: CGSize(width: 874, height: 402), safe: TouchInsets(top: 0, left: 62, bottom: 21, right: 62))

    /// Screens the layout must fit (points, safe areas): iPhone 17 / 16 Pro, 17 Pro Max / 16 Pro
    /// Max, SE (2nd / 3rd generation), and the browser sizes of the CSS measurements.
    static let devices: [(String, CGSize, TouchInsets)] = [
        ("iPhone 17", CGSize(width: 402, height: 874), TouchInsets(top: 62, left: 0, bottom: 34, right: 0)),
        ("iPhone 17 landscape", CGSize(width: 874, height: 402), TouchInsets(top: 0, left: 62, bottom: 21, right: 62)),
        ("iPhone 17 Pro Max", CGSize(width: 440, height: 956), TouchInsets(top: 62, left: 0, bottom: 34, right: 0)),
        ("iPhone 17 Pro Max landscape", CGSize(width: 956, height: 440), TouchInsets(top: 0, left: 62, bottom: 21, right: 62)),
        ("iPhone SE", CGSize(width: 375, height: 667), TouchInsets(top: 20, left: 0, bottom: 0, right: 0)),
        ("iPhone SE landscape", CGSize(width: 667, height: 375), TouchInsets()),
        ("390 x 844", CGSize(width: 390, height: 844), TouchInsets()),
        ("844 x 390", CGSize(width: 844, height: 390), TouchInsets()),
    ]

    static func layouts(_ o: TouchLayout.Options = TouchLayout.Options()) -> [(String, TouchLayout)] {
        devices.map { ($0.0, TouchLayout.make(size: $0.1, safe: $0.2, options: o)) }
    }

    static func layout(_ name: String, _ o: TouchLayout.Options) -> TouchLayout {
        let d = devices.first { $0.0 == name }!
        return TouchLayout.make(size: d.1, safe: d.2, options: o)
    }

    private static func disc(_ c: CGPoint, _ d: CGFloat) -> CGRect { CGRect(x: c.x - d / 2, y: c.y - d / 2, width: d, height: d) }

    /// Browser measurements of NESTERM's controller (getBoundingClientRect, 390 x 844). Blink rounds
    /// layout boxes to 1/64 px (LayoutUnit): ±0.05 for derived sizes. The D-pad is 10 % larger
    /// (ReplayNES), at the same left edge and centre.
    func testPortraitMatchesNESTERMCSS() {
        let g = portrait.pad
        XCTAssertEqual(g.dpad.width, (144 * 1.1).rounded())     // min(--bb-dpad 144, max(112, 844 * .24)) × 1.1
        XCTAssertEqual(g.dpad.minX, 19.546875, accuracy: 0.01)  // padding-inline 4.5vw + margin-left 2px
        XCTAssertEqual(g.face.minX, 214.265625, accuracy: 0.05)
        XCTAssertEqual(g.face.width, 158.1875, accuracy: 0.05)
        XCTAssertEqual(g.face.height, 150.59375, accuracy: 0.05)
        XCTAssertEqual(portrait.faceButton, 74.1, accuracy: 0.05) // clamp(48, min(19vw, el-h * .13), 78)
        // B sits bTowardA x the button further right than the CSS (A and B closer, B off the D-pad).
        let abX = 331.40625 - 255.3125 - TouchLayout.bTowardA * portrait.faceButton
        XCTAssertEqual(g.a.x - g.b.x, abX, accuracy: 0.05)
        XCTAssertEqual(g.a.y - g.b.y, 580.5522766113281 - 649.07275390625, accuracy: 0.05)
        XCTAssertEqual(hypot(g.a.x - g.b.x, g.a.y - g.b.y), hypot(abX, 580.5522766113281 - 649.07275390625), accuracy: 0.05)
        XCTAssertLessThan(hypot(g.a.x - g.b.x, g.a.y - g.b.y), 102.3978251614086, "A and B closer than the CSS")
        XCTAssertEqual(g.face.midY, g.dpad.midY, accuracy: 0.01, "both centred in the first grid row")
        XCTAssertEqual(g.system?.minX ?? 0, 17.546875, accuracy: 0.01)
        XCTAssertEqual(g.system?.width ?? 0, 354.90625, accuracy: 0.01)
        XCTAssertEqual(g.system?.height ?? 0, 48)
        XCTAssertEqual(g.select.height, 40)
        XCTAssertEqual(g.select.width, 92.65625, accuracy: 1.5, "SELECT: label (system font) + 44")
        XCTAssertEqual(g.start.width, 85.421875, accuracy: 1.5)
        XCTAssertEqual(g.start.minX - g.select.maxX, 22, accuracy: 0.01)
        XCTAssertEqual((g.select.minX + g.start.maxX) / 2, 195, accuracy: 0.01, "pills centred")
        // Rows: 6 px padding, row gap 4, system row 48, bottom padding 6.
        let clip = g.dpadClip!
        XCTAssertEqual(g.system!.maxY, clip.maxY - 6, accuracy: 0.01)
        XCTAssertEqual(g.dpad.midY, (clip.minY + 6 + g.system!.minY - 4) / 2, accuracy: 0.01)
        XCTAssertEqual(g.frame, CGRect(x: 0, y: 0, width: 390, height: 844))
        XCTAssertEqual(portrait.gameArea.width, 390)
        // No transport row (2026-10-30): the gamepad starts right under the picture's row.
        XCTAssertLessThanOrEqual(portrait.gameArea.maxY, clip.minY + 0.01)
    }

    /// Browser measurements at 844 x 390 (.bb-side); D-pad × 1.1.
    func testLandscapeMatchesNESTERMCSS() {
        let g = landscape.pad
        XCTAssertEqual(g.dpad, CGRect(x: 16, y: g.dpad.minY, width: 158, height: 158))
        // Each value to 0.01 (its height is clipped to the screen in floating point: 174 ± 1e-13).
        XCTAssertEqual(g.face.minX, 654, accuracy: 0.01)
        XCTAssertEqual(g.face.width, 190, accuracy: 0.01)
        XCTAssertEqual(g.face.height, 174, accuracy: 0.01)
        XCTAssertEqual(g.a.x, 789, accuracy: 0.01)
        XCTAssertEqual(g.b.x, 709 + TouchLayout.bTowardA * landscape.faceButton, accuracy: 0.01)
        XCTAssertEqual(g.b.y - g.a.y, 241.5159912109375 - 169.4840087890625, accuracy: 0.01)
        XCTAssertEqual(g.face.midY, g.dpad.midY, accuracy: 0.01)
        XCTAssertNil(g.system, "display: contents: only the pills take touches")
        XCTAssertNil(g.dpadClip)
        XCTAssertEqual(g.select.maxY, 390 - 8, accuracy: 0.01)
        XCTAssertEqual(g.select.height, 31.390625, accuracy: 0.01)
        XCTAssertEqual(g.select.midX, 25.671875 + 92.65625 / 2, accuracy: 0.01, "centred in the 144 px column")
        XCTAssertEqual(g.start.midX, 729.28125 + 85.421875 / 2, accuracy: 0.01)
        // ReplayNES: the picture sits between the columns, centred.
        XCTAssertEqual(landscape.gameArea.midX, 422, accuracy: 0.01)
        // The full height (no transport row since 2026-10-30).
        XCTAssertEqual(landscape.gameArea.minY, 0)
        XCTAssertEqual(landscape.gameArea.maxY, 390, accuracy: 0.01)
    }

    func testSafeAreasMoveTheControlsIn() {
        let l = phoneLandscape.pad
        XCTAssertEqual(l.dpad.minX, 62 + 16)
        // AA / BB up-left: the face moved right into its 16 px padding, A's disc ends at the safe area.
        XCTAssertEqual(l.face.maxX, 874 - 62 + 16, accuracy: 0.01)
        XCTAssertEqual(l.a.x + phoneLandscape.faceButton / 2, 874 - 62, accuracy: 0.01)
        let css = TouchLayout.make(size: CGSize(width: 874, height: 402), safe: TouchInsets(top: 0, left: 62, bottom: 21, right: 62), options: Self.css)
        XCTAssertEqual(css.pad.face.maxX, 874 - 62, accuracy: 0.01)
        XCTAssertEqual(l.select.maxY, 402 - 8 - 21, accuracy: 0.01)
        XCTAssertEqual(l.frame.minX, 62)
        XCTAssertEqual(l.frame.maxX, phoneLandscape.gameArea.minX + EDGE_MARGIN, accuracy: 0.01, "the pad's frame ends at the picture")
        let p = phonePortrait.pad
        XCTAssertEqual(phonePortrait.gameArea.minY, 62)
        XCTAssertEqual(p.system!.maxY, 874 - 34 - 6, accuracy: 0.01)
    }

    func testEverythingOnScreenAndApart() {
        for o in [TouchLayout.Options(), Self.css] {
            for (name, l) in Self.layouts(o) {
                let screen = CGRect(origin: .zero, size: l.size)
                let rounds = l.extras.filter { $0.style == .round }
                let pills = l.extras.filter { $0.style != .round }
                var boxes: [(String, CGRect)] = [("dpad", l.pad.dpad), ("face", l.pad.face), ("select", l.select), ("start", l.start)]
                boxes += pills.map { ("\($0.kind)", $0.frame) }
                // Every pill's 44 pt touch area stays on screen and off the D-pad, A / B and SELECT / START.
                for p in pills {
                    XCTAssertGreaterThanOrEqual(p.hitFrame.width, 44 - 0.01, "\(p.kind) (\(name))")
                    XCTAssertGreaterThanOrEqual(p.hitFrame.height, 44 - 0.01, "\(p.kind) (\(name))")
                    XCTAssertTrue(screen.contains(p.hitFrame), "\(p.kind) touch area off screen (\(name))")
                    for (n, r) in [("dpad", l.pad.dpad), ("select", l.select), ("start", l.start), ("a", Self.disc(l.pad.a, l.faceButton)), ("b", Self.disc(l.pad.b, l.faceButton))] {
                        XCTAssertFalse(p.hitFrame.intersects(r), "\(p.kind) touch area overlaps \(n) (\(name))")
                    }
                    for q in pills where q.kind != p.kind { XCTAssertFalse(p.hitFrame.intersects(q.hitFrame), "\(p.kind) / \(q.kind) touch areas (\(name))") }
                }
                for (n, r) in boxes + rounds.map({ ("\($0.kind)", $0.frame) }) {
                    XCTAssertTrue(screen.contains(r), "\(n) \(r) off \(screen) (\(name))")
                }
                for i in boxes.indices { for j in boxes.indices where j > i {
                    XCTAssertFalse(boxes[i].1.intersects(boxes[j].1), "\(boxes[i].0) overlaps \(boxes[j].0) (\(name))")
                } }
                // AA / BB sit over .gp-face's padding (by design) but clear of everything else.
                for r in rounds {
                    for (n, b) in boxes where n != "face" {
                        XCTAssertFalse(r.frame.intersects(b), "\(r.kind) overlaps \(n) (\(name))")
                    }
                }
                let kinds = l.extras.map(\.kind)
                for b in [TouchButton.slow, .pause, .menu] {
                    XCTAssertTrue(kinds.contains(.button(b)), "\(b) (\(name))")
                }
                XCTAssertTrue(l.extras.filter(\.isTransport).isEmpty, "no transport row (\(name))")
                XCTAssertTrue(kinds.contains(.ways))
                XCTAssertEqual(kinds.contains(.button(.turboA)), o.showAABB)
                XCTAssertEqual(kinds.contains(.button(.turboB)), o.showAABB)
            }
        }
    }

    /// The D-pad is 10 % larger and centred on the A/B midpoint, in both orientations.
    func testDpadLargerAndOnTheABMidpoint() {
        for o in [TouchLayout.Options(), Self.css] {
            for (name, l) in Self.layouts(o) {
                let g = l.pad
                XCTAssertEqual(g.dpad.midY, (g.a.y + g.b.y) / 2, accuracy: 0.01, name)
                let css = l.landscape ? o.dpadSize : min(o.dpadSize, max(112, l.size.height * 0.24))
                XCTAssertEqual(g.dpad.width, (css * 1.1).rounded(), name)
                XCTAssertEqual(g.dpad.width, g.dpad.height)
                if let clip = g.dpadClip { XCTAssertTrue(clip.contains(g.dpad), name) }
            }
        }
    }

    /// AA / BB: up-left of A / B (BB from B, AA from A, the same offset perpendicular to the A/B
    /// axis: a row parallel to A / B, like a Pro Controller's Y / X over B / A), the same .gp-round
    /// look but smaller, and hit zones that never reach the A / B discs (the ported roll logic keeps
    /// its buttons).
    func testAABBZonesUpLeftOfAB() {
        for (name, l) in Self.layouts() {
            let g = l.pad
            let aa = l.extra(.turboA)!, bb = l.extra(.turboB)!
            XCTAssertEqual(aa.style, .round)
            let da = CGVector(dx: aa.frame.midX - g.a.x, dy: aa.frame.midY - g.a.y)
            let db = CGVector(dx: bb.frame.midX - g.b.x, dy: bb.frame.midY - g.b.y)
            XCTAssertEqual(da.dx, db.dx, accuracy: 0.01, name)
            XCTAssertEqual(da.dy, db.dy, accuracy: 0.01, name)
            XCTAssertLessThan(da.dx, -15, "left of A (\(name))")
            XCTAssertLessThan(da.dy, -(l.faceButton / 2), "above A (\(name))")
            let step = TouchLayout.turboMetrics(btn: l.faceButton).step
            XCTAssertEqual(hypot(da.dx, da.dy), step, accuracy: 0.01, name)
            let u = TouchLayout.turboDirection(btn: l.faceButton)
            if abs(da.dx - u.dx * step) < 0.01 {
                // 11 o'clock: turboAngle left of straight up.
                XCTAssertEqual(atan2(-da.dx, -da.dy) * 180 / .pi, TouchLayout.turboAngle, accuracy: 0.01, "11 o'clock (\(name))")
            } else {
                // Turned toward straight up only as far as the D-pad (portrait) / picture (landscape) needs.
                XCTAssertGreaterThan(da.dx, u.dx * step, name)
                if l.landscape {
                    XCTAssertEqual(bb.frame.minX, Self.layout(name, TouchLayout.Options(showAABB: false)).pad.b.x - l.faceButton / 2, accuracy: 0.01, name)
                } else {
                    XCTAssertEqual(bb.frame.minX, g.dpad.maxX + TouchLayout.turboGap, accuracy: 0.01, name)
                }
            }
            XCTAssertLessThan(bb.frame.maxX, aa.frame.minX, name)
            XCTAssertLessThan(aa.frame.width, l.faceButton)
            XCTAssertEqual(aa.frame.width, bb.frame.width)
            let zoneR = aa.frame.width / 2 + TouchLayout.turboSlop
            for z in [aa, bb] {
                for c in [g.a, g.b] {
                    XCTAssertGreaterThanOrEqual(hypot(z.frame.midX - c.x, z.frame.midY - c.y), zoneR + l.faceButton / 2, "\(z.kind) zone reaches a face disc (\(name))")
                }
                XCTAssertTrue(z.hit(CGPoint(x: z.frame.midX, y: z.frame.midY)))
                XCTAssertTrue(z.hit(CGPoint(x: z.frame.midX, y: z.frame.minY - TouchLayout.turboSlop + 0.5)))
                XCTAssertFalse(z.hit(CGPoint(x: z.frame.midX, y: z.frame.maxY + TouchLayout.turboSlop + 0.5)))
            }
            // A touch on A or B never lands on an extra (TouchExtrasView is on top): the face gets it.
            for c in [g.a, g.b] {
                XCTAssertNil(l.extras.first { $0.hit(c) }, name)
                XCTAssertEqual(TouchGamepad(press: { _, _ in }).with(g).regionAt(c), .face, name)
            }
        }
    }

    /// 2026-10-30: no transport row (rewind / fast-forward are the picture's side quarters, frame
    /// steps the paused seek bar's). Slow motion: portrait in the top band left of the Dynamic
    /// Island, landscape above 8-WAY. Pause right above MENU. Both are app buttons.
    func testSlowAndPauseInsteadOfTheTransportRow() {
        for o in [TouchLayout.Options(), Self.css, TouchLayout.Options(dpadSize: 210)] {
            for (name, l) in Self.layouts(o) {
                XCTAssertTrue(l.extras.filter(\.isTransport).isEmpty, name)
                guard let slow = l.extra(.slow), let pause = l.extra(.pause), let menu = l.extra(.menu) else {
                    XCTFail("slow / pause / menu (\(name))"); continue
                }
                XCTAssertEqual(slow.style, .icon("tortoise.fill"))
                XCTAssertEqual(pause.style, .icon("pause.fill"))
                XCTAssertEqual(pause.frame.midX, menu.frame.midX, accuracy: 0.01, "pause centred over MENU (\(name))")
                XCTAssertLessThanOrEqual(pause.hitFrame.maxY, menu.hitFrame.minY - TouchLayout.pauseGap + 0.01, "pause above MENU (\(name))")
                if l.landscape {
                    let ways = l.extras.first { $0.kind == .ways }!
                    XCTAssertEqual(slow.frame.midX, ways.frame.midX, accuracy: 0.01, name)
                    XCTAssertLessThanOrEqual(slow.hitFrame.maxY, ways.hitFrame.minY + 0.01, "slow above 8-WAY (\(name))")
                    XCTAssertLessThan(slow.frame.midX, l.size.width / 2, name)
                } else {
                    XCTAssertLessThanOrEqual(slow.frame.maxX, l.size.width / 2 - TouchLayout.islandHalfWidth - TouchLayout.islandGap + 0.01,
                                             "left of the Dynamic Island (\(name))")
                    // With a top safe area (Dynamic Island / notch) it stays in that band, above the picture.
                    if l.gameArea.minY >= 40 { XCTAssertLessThanOrEqual(slow.frame.maxY, l.gameArea.minY + 0.01, name) }
                }
            }
        }
        XCTAssertNil(TouchButton.slow.inputID, "handled by the app, never game input")
        XCTAssertNil(TouchButton.pause.inputID, "handled by the app, never game input")
        XCTAssertNil(TouchButton.stepBack.inputID, "handled by the app (the seek bar's stepFrame)")
        XCTAssertNil(TouchButton(rawValue: "l"), "no touch L")
        XCTAssertNil(TouchButton(rawValue: "r"), "no touch R")
    }

    /// The round discs follow the game's buttons: 2 = turbo BB / AA (with the setting), 3 = C in AA's
    /// place (BB still turbo), 4 = D in BB's place too; C / D shown whatever the turbo setting.
    func testRoundsFollowTheGameButtons() {
        for (buttons, aabb, want) in [(2, true, [TouchButton.turboB, .turboA]), (2, false, []), (3, true, [.turboB, .c]),
                                      (3, false, [.c]), (4, true, [.d, .c]), (4, false, [.d, .c])] {
            var o = TouchLayout.Options(); o.buttons = buttons; o.showAABB = aabb
            for (name, l) in Self.layouts(o) {
                XCTAssertEqual(l.extras.filter { $0.style == .round }.map(\.kind), want.map { .button($0) }, "\(buttons) buttons, AABB \(aabb) (\(name))")
            }
        }
        XCTAssertEqual(TouchLayout.label(.button(.c), ways: 8), "C")
        XCTAssertEqual(TouchButton.c.inputID, "touch:c")
    }

    /// Another core's touch bindings: B / A = buttons 1 / 2, BB / AA = their turbo, C / D = 3 / 4,
    /// SELECT = coin (only actions the game has).
    func testTouchBindingsForAnArcadeLayout() {
        let actions: Set<String> = ["p1.up", "p1.down", "p1.left", "p1.right", "p1.b1", "p1.b1.turbo", "p1.b2", "p1.b2.turbo",
                                    "p1.b3", "p1.b3.turbo", "p1.coin", "p1.start"]
        let m = Dictionary(TouchBindings.forLayout(actions: actions).map { ($0.input, $0.action) }, uniquingKeysWith: { a, _ in a })
        XCTAssertEqual(m["touch:b"], "p1.b1"); XCTAssertEqual(m["touch:a"], "p1.b2"); XCTAssertEqual(m["touch:c"], "p1.b3")
        XCTAssertEqual(m["touch:turboB"], "p1.b1.turbo", "BB = button 1's turbo")
        XCTAssertEqual(m["touch:turboA"], "p1.b2.turbo", "AA = button 2's turbo")
        XCTAssertNil(m["touch:d"], "no button 4")
        XCTAssertEqual(m["touch:select"], "p1.coin")
    }

    /// Portrait: the NES picture as the Display options show it spans the screen's width in its row
    /// (FILL), the viewport drawn by MetalView. With a fixed 256 x 224 row the 240 x 224 picture (overscan
    /// hidden on all four sides, the default) left a band on each side (2026-11-19).
    func testPortraitNESPictureSpansTheWidth() {
        for (overscan, par87) in [(true, false), (false, false), (true, true), (false, true)] {
            var d = DisplayOptions(); d.integerScale = false; d.hideOverscan = overscan; d.pixelAspect87 = par87
            var o = TouchLayout.Options(); o.fill = true
            o.pictureAspect = d.displayedAspect; o.rowAspect = d.displayedAspect
            let size = CGSize(width: 402, height: 874)
            let l = TouchLayout.make(size: size, safe: TouchInsets(top: 62, left: 0, bottom: 34, right: 0), options: o)
            let px = CGSize(width: l.gameArea.width * 3, height: l.gameArea.height * 3)
            let (r, _) = d.viewport(drawableSize: px)
            XCTAssertEqual(r.width, px.width, accuracy: 3, "overscan \(overscan) 8:7 \(par87): picture \(r) in \(px)")
            XCTAssertEqual(r.height, px.height, accuracy: 3, "overscan \(overscan) 8:7 \(par87): row as tall as the picture")
            XCTAssertLessThan(l.gameArea.maxY, l.pad.dpad.minY, "the controls stay under the row")
        }
        XCTAssertEqual(DisplayOptions().displayedAspect, 240.0 / 224.0, accuracy: 1e-9)
    }

    /// Portrait FILL with the CRT Display: the CRT's shown part (32/35 of the 4:3 face) spans the screen's
    /// width and fills its row. Before, the whole face was fitted to the width: the shown part inside it
    /// was smaller, black around it (2026-11-19).
    func testPortraitCRTPictureSpansTheWidth() {
        let active = 32.0 / 35.0
        for overscan in [true, false] {
            var d = DisplayOptions(); d.integerScale = false; d.hideOverscan = overscan
            var o = TouchLayout.Options(); o.fill = true
            o.pictureAspect = d.crtShownAspect(activeWidth: active); o.rowAspect = o.pictureAspect!
            let l = TouchLayout.make(size: CGSize(width: 402, height: 874), safe: TouchInsets(top: 62, left: 0, bottom: 34, right: 0), options: o)
            let px = CGSize(width: l.gameArea.width * 3, height: l.gameArea.height * 3)
            let (face, _) = d.crtViewport(drawableSize: px, activeWidth: active)
            let shown = CGRect(x: face.minX + face.width * (1 - active) / 2, y: face.minY, width: face.width * active, height: face.height)
            XCTAssertEqual(shown.minX, 0, accuracy: 2, "overscan \(overscan): \(shown) in \(px)")
            XCTAssertEqual(shown.width, px.width, accuracy: 3, "overscan \(overscan): \(shown) in \(px)")
            XCTAssertEqual(shown.height, px.height, accuracy: 3, "overscan \(overscan): the row as tall as the shown part")
            XCTAssertGreaterThan(face.width, px.width, "the face beside the shown part lies outside")
        }
        // A landscape screen (height-bound): the shown part as tall as the plain picture, as before.
        var fill = DisplayOptions(); fill.integerScale = false
        XCTAssertEqual(fill.crtViewport(drawableSize: CGSize(width: 2622, height: 1206), activeWidth: active).0.height, 1206, accuracy: 1)
        XCTAssertEqual(DisplayOptions().crtViewport(drawableSize: CGSize(width: 2622, height: 1206), activeWidth: active).0.height,
                       224 * 5, accuracy: 1, "integer scale: 5 x 224 rows")
    }

    /// The bottom overlays (status label, Undo chip, seek bar) stay clear of the controls, also when
    /// portrait FILL puts a tall picture under them.
    func testOverlayAreaClearOfTheControls() {
        for aspect in [nil, 3.0 / 4.0, 4.0 / 3.0] as [CGFloat?] {
            var o = TouchLayout.Options(); o.fill = true; o.pictureAspect = aspect; o.buttons = 3
            for (name, l) in Self.layouts(o) {
                let a = l.overlayArea
                XCTAssertGreaterThan(a.height, 40, name)
                XCTAssertTrue(l.gameArea.contains(a) || l.landscape, name)
                let d = l.pad.dpad
                XCTAssertFalse(a.intersects(d), "D-pad (\(name), \(String(describing: aspect)))")
                for c in [l.pad.a, l.pad.b] {
                    let disc = CGRect(x: c.x - l.faceButton / 2, y: c.y - l.faceButton / 2, width: l.faceButton, height: l.faceButton)
                    XCTAssertFalse(a.intersects(disc), "A / B (\(name), \(String(describing: aspect)))")
                }
                for r in l.extras where r.style == .round { XCTAssertFalse(a.intersects(r.frame), "\(r.kind) (\(name))") }
            }
        }
    }

    /// 8-WAY and MENU: portrait at the ends of the SELECT / START row, landscape in the side columns.
    func testWaysAndMenuPlaces() {
        for (name, l) in Self.layouts() {
            let ways = l.extras.first { $0.kind == .ways }!.frame, menu = l.extra(.menu)!.frame
            if l.landscape {
                XCTAssertLessThan(ways.maxX, l.gameArea.minX, name)
                XCTAssertGreaterThan(menu.minX, l.gameArea.maxX, name)
            } else {
                XCTAssertEqual(ways.midY, l.select.midY, accuracy: 0.01, name)
                XCTAssertEqual(menu.midY, l.start.midY, accuracy: 0.01, name)
                XCTAssertLessThan(ways.maxX, l.select.minX, name)
                XCTAssertGreaterThan(menu.minX, l.start.maxX, name)
            }
        }
    }

    /// Landscape: the picture never runs under a control, on any of the screens, and the D-pad's
    /// home cannot follow the thumb over it.
    func testLandscapePictureBetweenTheControls() {
        for o in [TouchLayout.Options(), Self.css, TouchLayout.Options(dpadSize: 210)] {
            for (name, l) in Self.layouts(o) where l.landscape {
                let game = l.gameArea
                // The picture yields to the controls: at the largest D-pad (210 × 1.1) it gets narrow.
                XCTAssertGreaterThan(game.width, l.size.width * (o.dpadSize > TouchLayout.dpadDefault ? 0.2 : 0.4), name)
                XCTAssertEqual(game.midX, l.size.width / 2, accuracy: 0.01, name)
                var visible: [CGRect] = [l.pad.dpad, l.select, l.start, Self.disc(l.pad.a, l.faceButton), Self.disc(l.pad.b, l.faceButton)]
                visible += l.extras.map(\.frame)
                for r in visible { XCTAssertFalse(r.intersects(game), "\(r) under the picture \(game) (\(name))") }
                // AA / BB never cost picture width.
                XCTAssertEqual(game.width, Self.layout(name, TouchLayout.Options(showAABB: false, dpadSize: o.dpadSize)).gameArea.width, accuracy: 0.01, name)
                let pad = TouchGamepad(press: { _, _ in }).with(l.pad)
                pad.setHome(1000, 0)
                XCTAssertLessThanOrEqual(l.pad.dpad.maxX + pad.ox, game.minX + 0.01, name)
            }
        }
    }

    func testDpadSizeSetting() {
        let big = TouchLayout.make(size: CGSize(width: 844, height: 390), safe: TouchInsets(), options: TouchLayout.Options(dpadSize: 210))
        XCTAssertEqual(big.pad.dpad.width, (210 * 1.1).rounded())
        // Portrait caps it: min(--bb-dpad, max(112px, el-h * .24)), then × 1.1.
        let small = TouchLayout.make(size: CGSize(width: 320, height: 568), safe: TouchInsets(), options: TouchLayout.Options(dpadSize: 210))
        XCTAssertEqual(small.pad.dpad.width, (568 * 0.24 * 1.1).rounded(), accuracy: 0.01)
    }

    func testTouchBindingsCoverEveryGameButtonOnce() {
        // C / D (buttons 3 / 4) are another core's only (TouchBindings.forLayout), never the NES pad's.
        let ids = TouchButton.allCases.filter { $0 != .c && $0 != .d }.compactMap(\.inputID)
        XCTAssertEqual(Set(TouchBindings.defaults.map(\.input)), Set(ids))
        XCTAssertEqual(TouchBindings.defaults.count, ids.count)
        XCTAssertNil(TouchButton.menu.inputID)
        XCTAssertNil(TouchButton.stepForward.inputID)
        XCTAssertEqual(TouchButton.turboA.inputID, "touch:turboA", "AA is bound like the old turbo pill")
        XCTAssertEqual(TouchBindings.missing(existing: [("kb:1", "p1.a")]).count, TouchBindings.defaults.count)
        XCTAssertTrue(TouchBindings.missing(existing: [("touch:a", "p1.b")]).isEmpty, "a customised touch table is kept")
        // input-router.mjs BUTTON_NAME: every Brickboy id reaches the NES button of the same name.
        XCTAssertEqual(BBButton.allCases.map { TouchButton($0).inputID! },
                       ["touch:dpad.right", "touch:dpad.left", "touch:dpad.up", "touch:dpad.down", "touch:a", "touch:b", "touch:select", "touch:start"])
    }

    func testOrientationLockMasks() {
        XCTAssertEqual(OrientationLock.auto.mask, .allButUpsideDown)
        XCTAssertEqual(OrientationLock.portrait.mask, .portrait)
        XCTAssertEqual(OrientationLock.landscape.mask, .landscape)
        XCTAssertEqual(OrientationLock.allCases.count, 3)
    }

    func testTouchBindingsAreAcceptedByTheEngine() {
        let input = rn_input_new()!
        defer { rn_input_free(input) }
        for b in TouchBindings.defaults { XCTAssertEqual(rn_input_bind(input, b.input, b.action), RN_OK, "\(b)") }
        rn_input_set_pressed(input, "touch:a", 1)
        rn_input_set_pressed(input, "touch:dpad.left", 1)
        var p1: UInt8 = 0, p2: UInt8 = 0
        rn_input_sample_game(input, 0, &p1, &p2)
        XCTAssertNotEqual(p1, 0)
        XCTAssertEqual(p2, 0)
        rn_input_release_prefix(input, "touch:")
        rn_input_sample_game(input, 1, &p1, &p2)
        XCTAssertEqual(p1, 0)
    }
}

/// input.mjs's pure helpers.
final class BrickboyHelperTests: XCTestCase {
    func testSectors() {
        XCTAssertEqual(norm360(-90), 270)
        XCTAssertEqual(norm360(725), 5)
        XCTAssertEqual(angularDist(350, 10), 20)
        XCTAssertEqual(angularDist(-170, 170), 20)
        // First read: cardinals own 65% of the circle (half-width 29.25°).
        XCTAssertEqual(sectorRaw(29.25, CARD_HALF_ENGAGE), .R)
        XCTAssertEqual(sectorRaw(29.3, CARD_HALF_ENGAGE), .DR)
        XCTAssertEqual(sectorRaw(270 + 29.2, CARD_HALF_ENGAGE), .U)
        XCTAssertEqual(sectorRaw(315, CARD_HALF_ENGAGE), .UR)
        // Held: an even split.
        XCTAssertEqual(sectorRaw(22.5, CARD_HALF_HELD), .R)
        XCTAssertEqual(sectorRaw(22.6, CARD_HALF_HELD), .DR)
        // 4-way: no diagonal can be produced (the first band in DIR8_ORDER takes the tie).
        for a in stride(from: CGFloat(0), to: 360, by: 0.5) { XCTAssertEqual(sectorRaw(a, CARD_HALF_4WAY).btns.count, 1, "\(a)") }
        XCTAssertEqual(sectorRaw(45, CARD_HALF_4WAY), .R)
        XCTAssertEqual(sectorRaw(45.01, CARD_HALF_4WAY), .D)
        XCTAssertEqual(DIR8_SETS[.UL], [.up, .left])
    }

    func testRadiiFromReach() {
        let pad = TouchGamepad(press: { _, _ in })
        // No taps yet: REACH_DEFAULT (0.45 r) of a 72 px radius.
        XCTAssertEqual(pad.engageDeadzone(72), 8.1, accuracy: 1e-9)   // clamp(32.4 * .25, 5.76, 14.4)
        XCTAssertEqual(pad.commitRadius(72), 19.44, accuracy: 1e-9)   // clamp(32.4 * .6, 8.64, 21.6)
        pad.setWays(4)
        XCTAssertEqual(pad.commitRadius(72), 31.104, accuracy: 1e-9)  // x R_COMMIT_4WAY
        XCTAssertEqual(pad.engageDeadzone(72), 12.96, accuracy: 1e-9)
        XCTAssertNil(pad.restEstimate())
    }

    func testCSSEase() {
        XCTAssertEqual(TouchGamepad.cssEase(0), 0, accuracy: 1e-6)
        XCTAssertEqual(TouchGamepad.cssEase(1), 1, accuracy: 1e-6)
        XCTAssertEqual(TouchGamepad.cssEase(0.5), 0.8024, accuracy: 1e-3) // cubic-bezier(.25,.1,.25,1) at 0.5
    }
}

/// NESTERM scripts/controller-check.mjs, the portrait group: the same touches (positions relative
/// to the controls) and the same expectations, on the iPhone layout.
final class NESTERMControllerCheckTests: XCTestCase {
    private var pad: TouchGamepad!
    private var t: Double = 1000
    private var held: Set<BBButton> { Set(BBButton.allCases.filter { pad.isActive($0) }) }
    private var g: TouchGamepad.Geometry { pad.geometry }

    override func setUp() {
        let l = TouchLayout.make(size: CGSize(width: 390, height: 844), safe: TouchInsets(), options: TouchLayoutTests.css)
        pad = TouchGamepad(press: { _, _ in })
        pad.geometry = l.pad
        pad.setHomeGain(0.3, 1)
    }

    private func at(_ dt: Double = 40) -> Double { t += dt; return t }
    private func down(_ id: Int, _ p: CGPoint, _ dt: Double = 40) { pad.pointerDown(PointerEvent(id: id, x: p.x, y: p.y, t: at(dt))) }
    private func move(_ id: Int, _ p: CGPoint, _ dt: Double = 40) { pad.pointerMove(PointerEvent(id: id, x: p.x, y: p.y, t: at(dt))) }
    private func up(_ id: Int, _ p: CGPoint = .zero, _ dt: Double = 40) { pad.pointerUp(PointerEvent(id: id, x: p.x, y: p.y, t: at(dt))) }
    private func frames(_ ms: Double) {
        let end = t + ms
        while t < end { t += 1000 / 60; pad.animationFrame(now: t) }
    }
    private func along(_ f: CGFloat) -> CGPoint { CGPoint(x: g.b.x + (g.a.x - g.b.x) * f, y: g.b.y + (g.a.y - g.b.y) * f) }
    private func fromA(_ f: CGFloat) -> CGPoint { CGPoint(x: g.a.x + (g.b.x - g.a.x) * f, y: g.a.y + (g.b.y - g.a.y) * f) }

    func testAdaptiveDpadAndCancel() {
        let d = g.dpad, c = CGPoint(x: d.midX, y: d.midY)
        down(1, c)
        move(1, CGPoint(x: c.x + d.width * 0.40, y: c.y))
        XCTAssertEqual(held, [.right], "adaptive D-pad right slide")
        move(1, CGPoint(x: c.x + d.width * 0.34, y: c.y - d.height * 0.34))
        frames(50)
        XCTAssertEqual(held, [.right, .up], "8-way diagonal")
        up(1) // pointercancel
        XCTAssertEqual(held, [], "pointercancel must release D-pad")
    }

    func testFourAndEightWay() {
        let d = g.dpad, c = CGPoint(x: d.midX, y: d.midY)
        pad.setWays(4)
        down(2, c)
        move(2, CGPoint(x: c.x + d.width * 0.34, y: c.y - d.height * 0.34))
        XCTAssertEqual(held.count, 1, "4-way: exactly one direction")
        up(2)
        XCTAssertEqual(held, [])
        pad.setWays(8)
        XCTAssertEqual(pad.wayCount, 8)
    }

    func testFaceSlideOverlapAndTwoFinger() {
        down(3, along(0)); XCTAssertEqual(held, [.b], "face B")
        move(3, along(0.5)); XCTAssertEqual(held, [.a, .b], "face overlap A+B")
        move(3, along(1)); XCTAssertEqual(held, [.a], "face slide B to A")
        up(3); XCTAssertEqual(held, [], "face slide release")
        down(4, along(1)); XCTAssertEqual(held, [.a], "first face finger")
        down(5, along(0)); XCTAssertEqual(held, [.a, .b], "second face finger must latch other button")
        up(5); XCTAssertEqual(held, [.a], "first face finger must remain")
        up(4); XCTAssertEqual(held, [], "two-finger face release")
    }

    func testRollTap() {
        down(6, along(0)); XCTAssertEqual(held, [.b], "roll: B held")
        move(6, along(0.55)); XCTAssertEqual(held, [.a, .b], "roll: into overlap presses A")
        move(6, along(0.40)); XCTAssertEqual(held, [.b], "roll: partial return releases A while B stays")
        move(6, along(0.56)); XCTAssertEqual(held, [.a, .b], "roll: second roll re-taps A")
        move(6, along(0.42)); XCTAssertEqual(held, [.b], "roll: second partial return releases A")
        up(6); XCTAssertEqual(held, [], "roll: release")
    }

    func testMotionRoll() {
        down(7, along(0)); XCTAssertEqual(held, [.b], "motion: B held")
        move(7, along(0.18), 60); XCTAssertEqual(held, [.a, .b], "motion: quick shallow roll presses A")
        move(7, along(0.04)); XCTAssertEqual(held, [.b], "motion: retreat releases A")
        up(7); XCTAssertEqual(held, [], "motion: release")
        down(8, along(0), 300); XCTAssertEqual(held, [.b], "drift: B held")
        for k in 1...6 { move(8, along(0.03 * CGFloat(k)), 150) }
        XCTAssertEqual(held, [.b], "drift: slow thumb drift must not press A")
        up(8); XCTAssertEqual(held, [], "drift: release")
    }

    func testRememberedRoll() {
        down(9, fromA(0)); XCTAssertEqual(held, [.a], "mirror: A held")
        move(9, fromA(0.55)); XCTAssertEqual(held, [.a, .b], "mirror: roll presses B")
        move(9, fromA(0.40)); XCTAssertEqual(held, [.a], "mirror: retreat releases B")
        up(9); XCTAssertEqual(held, [], "mirror: release")
        down(10, along(0), 300); XCTAssertEqual(held, [.b], "creep: B held")
        move(10, along(0.55)); XCTAssertEqual(held, [.a, .b], "creep: roll presses A")
        move(10, along(0.42)); XCTAssertEqual(held, [.b], "creep: retreat releases A")
        for f in [0.45, 0.48, 0.51] as [CGFloat] { move(10, along(f), 150) }
        XCTAssertEqual(held, [.b], "creep: slow drift after a release must not re-press A")
        move(10, along(0.65), 120); XCTAssertEqual(held, [.a, .b], "creep: a quick advance re-presses A")
        up(10); XCTAssertEqual(held, [], "creep: release")
        down(11, along(0.45), 300); XCTAssertEqual(held, [.a, .b], "middle: touchdown in the overlap presses both")
        move(11, along(0.35)); XCTAssertEqual(held, [.b], "middle: retreat toward B releases A")
        up(11); XCTAssertEqual(held, [], "middle: release")
    }

    func testSystemOverlap() {
        let s = CGPoint(x: g.select.midX, y: g.select.midY), st = CGPoint(x: g.start.midX, y: g.start.midY)
        down(6, CGPoint(x: (s.x + st.x) / 2, y: (s.y + st.y) / 2))
        XCTAssertEqual(held, [.select, .start], "system overlap must press START+SELECT")
        up(6); XCTAssertEqual(held, [], "system overlap release")
    }

    func testRapidDpadRetapPassesThroughZero() {
        // input.mjs onDpadDown: the newest finger owns the pad, so a re-tap before the old finger
        // lifts still produces a release and a press edge.
        var edges: [(BBButton, Bool)] = []
        let p = TouchGamepad(press: { edges.append(($0, $1)) })
        p.geometry = pad.geometry
        let d = g.dpad, right = CGPoint(x: d.midX + d.width * 0.4, y: d.midY)
        p.pointerDown(PointerEvent(id: 1, x: right.x, y: right.y, t: 1000))
        p.pointerDown(PointerEvent(id: 2, x: right.x, y: right.y, t: 1040))
        p.pointerUp(PointerEvent(id: 1, x: right.x, y: right.y, t: 1050))
        XCTAssertEqual(edges.map { "\($0.0)\($0.1 ? "+" : "-")" }, ["right+", "right-", "right+"])
    }

    func testReleaseAllKeepsTheHand() {
        let d = g.dpad
        for i in 0..<4 { // a few taps: the rest estimate learns
            down(20 + i, CGPoint(x: d.midX + 10, y: d.midY - d.width * 0.3))
            frames(40)
            up(20 + i)
            frames(40)
        }
        XCTAssertNotNil(pad.restEstimate())
        down(30, CGPoint(x: d.midX, y: d.midY - d.width * 0.4))
        pad.releaseAll()
        XCTAssertEqual(held, [])
        XCTAssertEqual(pad.ox, 0)
        XCTAssertEqual(pad.oy, 0)
        XCTAssertNotNil(pad.restEstimate(), "the rest estimate describes the hand: kept")
    }
}

/// The touch turbo keys on another core's layout reach the engine as turbo (rn_input with the arcade
/// bindings of TouchBindings.forLayout): BB held = button 1 pulsing, B held = button 1 held.
final class ArcadeTouchTurboTests: XCTestCase {
    private static let layout = """
    {"name":"test-arcade","recordBytes":8,"buttons":[
     {"id":"p1.up","byte":0,"bit":4},{"id":"p1.down","byte":0,"bit":5},{"id":"p1.left","byte":0,"bit":6},{"id":"p1.right","byte":0,"bit":7},
     {"id":"p1.b1","byte":0,"bit":0},{"id":"p1.b2","byte":0,"bit":1},{"id":"p1.b3","byte":0,"bit":2},
     {"id":"p1.coin","byte":1,"bit":0},{"id":"p1.start","byte":1,"bit":1}],"axes":[],"opposite":[]}
    """

    private func bit0(_ input: OpaquePointer, frames: Int) -> [Int] {
        (0..<frames).map { f in
            var r = rn_input_record()
            rn_input_sample_record(input, UInt64(f), &r)
            return Int(withUnsafeBytes(of: r.bytes) { $0[0] } & 1)
        }
    }

    func testTurboKeysPulseTheirButton() {
        let input = rn_input_new()!
        defer { rn_input_free(input) }
        XCTAssertEqual(rn_input_set_layout_json(input, Self.layout), RN_OK)
        let actions: Set<String> = ["p1.up", "p1.down", "p1.left", "p1.right", "p1.b1", "p1.b1.turbo", "p1.b2", "p1.b2.turbo",
                                    "p1.b3", "p1.b3.turbo", "p1.coin", "p1.start"]
        for b in TouchBindings.forLayout(actions: actions) { XCTAssertEqual(rn_input_bind(input, b.input, b.action), RN_OK, b.input) }
        XCTAssertEqual(rn_input_set_turbo(input, 4, 2), RN_OK)
        rn_input_set_pressed(input, "touch:turboB", 1)
        let turbo = bit0(input, frames: 12)
        XCTAssertTrue(turbo.contains(0) && turbo.contains(1), "BB held pulses button 1: \(turbo)")
        XCTAssertEqual(turbo.filter { $0 == 1 }.count, 6, "on 2 of every 4 frames: \(turbo)")
        rn_input_set_pressed(input, "touch:turboB", 0)
        rn_input_set_pressed(input, "touch:b", 1)
        XCTAssertEqual(bit0(input, frames: 12).dropFirst(), Array(repeating: 1, count: 11)[...], "B held holds button 1")
    }
}
