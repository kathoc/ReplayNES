// Where the picture and the on-screen controls go: the geometry of NESTERM's controller CSS
// (web/styles.css, derived from Brickboy; portrait = `#app:not(.bb-side)` rules, landscape =
// `.bb-side` grid) computed for a screen size and its safe area. Every number below is the CSS
// value it comes from; apps/ios/docs/TOUCH_PORT.md maps the rules. Pure geometry (points, origin
// top-left; UIFont only measures the pill labels), unit-tested in BrickboyTouchTests.
//
// Portions derived from Brickboy / NESTERM web/styles.css, Copyright 2026 Katsuya Hokamura,
// Apache-2.0 (apps/ios/ThirdParty/brickboy/LICENSE). The ReplayNES extras (the transport row under
// the picture, MENU, turbo) and TouchButton / TouchBindings are ReplayNES's own.
// SPDX-License-Identifier: Apache-2.0 AND GPL-2.0-or-later
import CoreGraphics
import Foundation
import UIKit

/// One on-screen button as the app sees it. Game buttons reach the engine as the physical ids
/// "touch:<name>" (bound like a controller's: TouchBindings); `menu` opens the Quick Menu.
enum TouchButton: String, CaseIterable, Hashable {
    case up, down, left, right
    case a, b, turboA, turboB
    case select, start
    /// Rewind / fast-forward (the transport row's large buttons): bound like a controller's L2 / R2
    /// (hold to wind; on release the resume countdown, docs/design/VTR_UX.md). The ids keep their
    /// old names ("touch:l2" / "touch:r2") so saved bindings stay valid.
    case l2, r2
    case menu
    /// Frame step back / forward (ReplayNES): handled by the app like the desktop's Step Back /
    /// Frame Advance keys (AppModel.touchButtons → stepFrame: pauses, then steps one frame; held:
    /// repeats after 400 ms, every 50 ms, as a controller's paused L / R).
    case stepBack, stepForward
    /// Slow motion (ReplayNES; portrait: left of the Dynamic Island, landscape: above 8-WAY): handled
    /// by the app, toggles normal ⇔ 1/2 at the press (the desktop's / controller's hk.slow, R held + L).
    case slow
    /// Pause (ReplayNES, above MENU): handled by the app, pauses the game (the seek bar shows). The
    /// picture is no longer tapped to pause (docs/TOUCH_PORT.md, 2026-10-30).
    case pause
    /// Buttons 3 / 4 of another core's game (ReplayArcade: three- and four-button boards), in AA's / BB's
    /// place (TouchLayout.Options.buttons); never on the NES.
    case c, d

    /// The engine input id ("touch:dpad.up"), nil for buttons handled by the app.
    var inputID: String? {
        switch self {
        case .up, .down, .left, .right: return "touch:dpad.\(rawValue)"
        case .menu, .stepBack, .stepForward, .slow, .pause: return nil
        default: return "touch:\(rawValue)"
        }
    }

    var isDirection: Bool { self == .up || self == .down || self == .left || self == .right }

    /// The Brickboy gamepad's buttons (input-router.mjs BUTTON_NAME: right, left, up, down, a, b,
    /// select, start).
    init(_ b: BBButton) {
        switch b {
        case .right: self = .right
        case .left: self = .left
        case .up: self = .up
        case .down: self = .down
        case .a: self = .a
        case .b: self = .b
        case .select: self = .select
        case .start: self = .start
        }
    }
}

/// Default bindings of the touch ids: the same actions as a controller's (frontend core
/// defaults: L2 rewind, R2 fast-forward; turbo A / B). There is no touch L / R: pause is a tap on
/// the picture, slow motion is on the seek bar, the frame steps are app buttons.
enum TouchBindings {
    static let defaults: [(input: String, action: String)] = [
        ("touch:dpad.up", "p1.up"), ("touch:dpad.down", "p1.down"),
        ("touch:dpad.left", "p1.left"), ("touch:dpad.right", "p1.right"),
        ("touch:a", "p1.a"), ("touch:b", "p1.b"),
        ("touch:turboA", "p1.turbo_a"), ("touch:turboB", "p1.turbo_b"),
        ("touch:select", "p1.select"), ("touch:start", "p1.start"),
        ("touch:l2", "hk.rewind"), ("touch:r2", "hk.fast_forward"),
    ]

    /// Another core's layout (`actions`: its game action ids, "p1.b1", "p1.b1.turbo" ...): the D-pad,
    /// B = button 1, A = button 2, BB / AA = their turbo (as the NES pad), C / D = buttons 3 / 4 when the
    /// game has them (TouchLayout.Options.buttons), SELECT = coin, START = start; L2 / R2 wind the tape.
    static func forLayout(actions: Set<String>) -> [(input: String, action: String)] {
        var out: [(input: String, action: String)] = [
            ("touch:dpad.up", "p1.up"), ("touch:dpad.down", "p1.down"),
            ("touch:dpad.left", "p1.left"), ("touch:dpad.right", "p1.right"),
            ("touch:b", "p1.b1"), ("touch:a", "p1.b2"), ("touch:c", "p1.b3"), ("touch:d", "p1.b4"),
            // Turbo: BB = button 1's, AA = button 2's (InputLayout::turboOf: "<id>.turbo").
            ("touch:turboB", "p1.b1.turbo"), ("touch:turboA", "p1.b2.turbo"),
            ("touch:select", "p1.coin"), ("touch:start", "p1.start"),
        ]
        out = out.filter { actions.contains($0.action) }
        return out + [("touch:l2", "hk.rewind"), ("touch:r2", "hk.fast_forward")]
    }

    /// Bindings to add to a saved configuration that has none for the touch ids (first run or a
    /// configuration from before touch controls). An existing touch binding means the user's
    /// table is kept as it is.
    static func missing(existing: [(input: String, action: String)]) -> [(input: String, action: String)] {
        if existing.contains(where: { $0.input.hasPrefix("touch:") }) { return [] }
        return defaults
    }
}

struct TouchInsets: Equatable {
    var top: CGFloat = 0, left: CGFloat = 0, bottom: CGFloat = 0, right: CGFloat = 0
}

/// A ReplayNES control drawn by TouchExtrasView: a pill of the extras row (a ReplayNES button, or
/// NESTERM's 4/8-way switch) or one of the AA / BB rounds up-left of A / B.
struct TouchExtra: Equatable {
    enum Kind: Equatable {
        case button(TouchButton)
        case ways       // #ways-switch (controller.mjs syncWays): toggles 8-way / 4-way
    }
    /// .bb-mode-switch pill with a text label, the same pill with an SF Symbol, or a .gp-round
    /// (AA / BB: `frame` is the disc's square).
    enum Style: Equatable {
        case pill
        case icon(String)
        case round
    }
    let kind: Kind
    let frame: CGRect
    var style: Style = .pill

    /// Touchdown test: a pill's box grown to the 44 pt minimum (`hitFrame`); a round's disc grown
    /// by TouchLayout.turboSlop.
    func hit(_ p: CGPoint) -> Bool {
        if style == .round {
            return hypot(p.x - frame.midX, p.y - frame.midY) <= frame.width / 2 + TouchLayout.turboSlop
        }
        return hitFrame.contains(p)
    }

    /// A pill's touch area: its box, grown about its centre to at least TouchLayout.minTarget in
    /// each direction (Apple's 44 x 44 pt). A round's: its square plus the slop.
    var hitFrame: CGRect {
        if style == .round { return frame.insetBy(dx: -TouchLayout.turboSlop, dy: -TouchLayout.turboSlop) }
        let m = TouchLayout.minTarget
        return frame.insetBy(dx: -max(0, (m - frame.width) / 2), dy: -max(0, (m - frame.height) / 2))
    }

    /// One of the transport row's buttons (under the picture).
    var isTransport: Bool {
        guard case .button(let b) = kind else { return false }
        return TouchLayout.transportOrder.contains(b)
    }
}

struct TouchLayout: Equatable {
    struct Options: Equatable {
        /// AA / BB (turbo A / turbo B) up-left of A / B.
        var showAABB = true
        /// --bb-dpad (controller.mjs applySize: 130...210 px, CSS default 144px).
        var dpadSize: CGFloat = TouchLayout.dpadDefault
        /// 8 or 4: the label of the ways switch ("8-WAY").
        var ways = 8
        /// Display › FILL (not integer scale): in landscape the game area is the whole screen, the
        /// picture as large as its aspect allows (the screen's height for a 4:3 picture), the controls
        /// over it - the Linux app's FILL (computeGameRect over the whole window, the UI on top).
        var fill = false
        /// The picture's displayed width / height (DisplayOptions.displayedAspect: overscan crop, 8:7,
        /// another core's pixel aspect and rotation); nil = the row's. Portrait FILL makes the game area as
        /// tall as a full-width picture of this aspect.
        var pictureAspect: CGFloat?
        /// The portrait picture row's width / height: the NES picture as the Display options show it
        /// (240 x 224 with the overscan hidden on all four sides). The controls start under that row. A
        /// fixed 256 x 224 row left the shown 240 x 224 picture 3 % short of each side in FILL (2026-11-19).
        var rowAspect: CGFloat = TouchLayout.pictureAspect
        /// The game's fire buttons (the NES: A and B). 3: AA's disc is button 3 ("C"); 4: BB's is
        /// button 4 ("D") too - shown whatever the turbo setting, so every button of the game is there.
        var buttons = 2
        /// The round discs up-left of A / B are shown (turbo keys, or buttons 3 / 4).
        var showRounds: Bool { showAABB || buttons >= 3 }
    }

    var size: CGSize
    var landscape: Bool
    /// The area the game view covers (the renderer centres and scales the picture inside it).
    var gameArea: CGRect
    /// TouchGamepad's geometry (the D-pad at its natural place, A/B, SELECT/START, hit boxes).
    var pad: TouchGamepad.Geometry
    /// The A/B button diameter (--gp-btn).
    var faceButton: CGFloat
    /// SELECT / START pills (drawn; their hit areas are in `pad`).
    var select: CGRect
    var start: CGRect
    /// The ReplayNES extras: the extras row's pills, then AA / BB (style .round) when shown.
    var extras: [TouchExtra]
    /// Where the overlays at the bottom of the game (the tape's status label, the Undo chip, the paused
    /// seek bar) go: the game area clear of the controls. Portrait: down to `overlayGap` above the
    /// highest of the D-pad, A / B and the round discs (FILL can put the picture under them); landscape:
    /// between the control columns.
    var overlayArea: CGRect = .zero

    func extra(_ b: TouchButton) -> TouchExtra? { extras.first { $0.kind == .button(b) } }

    static let dpadDefault: CGFloat = 144
    static let dpadRange: ClosedRange<CGFloat> = 130...210

    /// NES picture aspect with only the overscan rows hidden (256 x 224): Options.rowAspect's default.
    static let pictureAspect: CGFloat = 256.0 / 224.0

    // :root { font: 15px/1.4 system-ui }: buttons inherit the line height (1.4).
    static let lineHeight: CGFloat = 1.4
    // .gp-pill { padding: 8px 22px; font-size: 11px; font-weight: 600; letter-spacing: 1px }
    static let pillFont: CGFloat = 11
    static let pillTracking: CGFloat = 1
    static let pillPadX: CGFloat = 22
    static let pillPadY: CGFloat = 8
    // .bb-mode-switch, .bb-turbo-switch { padding: 5px 10px; border: 1px; font-size: 11px;
    //   font-weight: 600; letter-spacing: .4px } - the style of the extras row.
    static let switchTracking: CGFloat = 0.4
    static let switchPadX: CGFloat = 10
    static let switchPadY: CGFloat = 5
    static let switchBorder: CGFloat = 1
    // .bb-subbar { min-height: 34px; padding: 0 8px } (its row holds the taller transport row).
    static let subbarHeight: CGFloat = 34
    static let subbarPadX: CGFloat = 8
    // .gp-face { --gp-gap: 2px; --gp-tilt: .4502 }
    static let faceGap: CGFloat = 2
    static let faceTilt: CGFloat = 0.4502
    /// B sits this fraction of the button diameter further right than the CSS puts it (ReplayNES,
    /// 2026-10-23): A and B a little closer, B (and BB) further from the D-pad, which a thumb on B
    /// used to brush. TOUCH_PORT.md, Deviations.
    static let bTowardA: CGFloat = 0.12
    /// AA / BB at 11 o'clock of A / B (30° left of straight up; ReplayNES, 2026-10-23). Was
    /// perpendicular to the B → A axis (about 10:30), which put BB too close to the D-pad.
    static let turboAngle: CGFloat = 30
    /// The slow-motion button (portrait): its box under the middle of the transport row.
    static let slowSize = CGSize(width: 44, height: 32)
    static let slowGap: CGFloat = 4
    /// The Dynamic Island (portrait): half its width and the gap kept to it; its middle is about half
    /// the top safe area down. Phones without one (a notch, or none) get the same place.
    static let islandHalfWidth: CGFloat = 63
    static let islandGap: CGFloat = 12
    static func islandMidY(_ safeTop: CGFloat) -> CGFloat { safeTop > 20 ? (safeTop / 2).rounded() : 22 }
    /// The bottom overlays' clearance above the controls (portrait).
    static let overlayGap: CGFloat = 8
    /// Pause above MENU: the gap between their touch areas.
    static let pauseGap: CGFloat = 8

    // ReplayNES deviations from the CSS (apps/ios/docs/TOUCH_PORT.md, Deviations).
    /// The D-pad is drawn 10 % larger than --bb-dpad / the portrait clamp give.
    static let dpadScale: CGFloat = 1.1
    /// AA / BB: diameter relative to --gp-btn, clear gap to the A / B disc below, hit slop.
    static let turboScale: CGFloat = 0.72
    static let turboGap: CGFloat = 8
    static let turboSlop: CGFloat = 4
    static let turboFont: CGFloat = 15
    /// Apple's minimum touch target (points): every pill's touch area is at least this.
    static let minTarget: CGFloat = 44
    /// The transport row under the picture, left to right: Rewind, Step Back, Step Forward,
    /// Fast-forward. Rewind / Fast-forward are `transportWide` wide (narrower only when the row
    /// does not fit), the steps half that, never under `minTarget`; all `transportHeight` tall.
    static let transportOrder: [TouchButton] = [.l2, .stepBack, .stepForward, .r2]
    static let transportHeight: CGFloat = 44
    static let transportWide: CGFloat = 112
    static let transportGap: CGFloat = 8
    /// Space between the transport row and the picture / the gamepad (portrait).
    static let transportMargin: CGFloat = 4
    /// Landscape: clear space between the picture and the control columns.
    static let pictureMargin: CGFloat = 8

    static func make(size: CGSize, safe: TouchInsets, options: Options = Options()) -> TouchLayout {
        // controller.mjs syncViewport: root.classList.toggle('bb-side', width > height).
        size.width > size.height ? landscape(size, safe, options) : portrait(size, safe, options)
    }

    // MARK: text metrics

    /// Width of a label in the system font (system-ui), CSS letter-spacing added after every
    /// character.
    static func textWidth(_ s: String, size: CGFloat, weight: UIFont.Weight, tracking: CGFloat) -> CGFloat {
        let f = UIFont.systemFont(ofSize: size, weight: weight)
        return ceil((s as NSString).size(withAttributes: [.font: f]).width * 100) / 100 + tracking * CGFloat(s.count)
    }

    /// A .gp-pill's size (SELECT / START); `minHeight`: portrait's `.gp-pill { min-height: 40px }`.
    static func pillSize(_ label: String, minHeight: CGFloat) -> CGSize {
        let w = textWidth(label, size: pillFont, weight: .semibold, tracking: pillTracking) + 2 * pillPadX
        return CGSize(width: w, height: max(minHeight, pillFont * lineHeight + 2 * pillPadY))
    }

    /// A .bb-mode-switch-styled pill (the extras row).
    static func switchSize(_ label: String) -> CGSize {
        let w = textWidth(label, size: pillFont, weight: .semibold, tracking: switchTracking) + 2 * (switchPadX + switchBorder)
        return CGSize(width: w, height: pillFont * lineHeight + 2 * (switchPadY + switchBorder))
    }

    static func label(_ k: TouchExtra.Kind, ways: Int) -> String {
        switch k {
        case .ways: return ways == 4 ? "4-WAY" : "8-WAY"
        case .button(let b):
            switch b {
            case .l2: return "◀◀"
            case .r2: return "▶▶"
            case .menu: return "MENU"
            case .turboA: return "AA"
            case .turboB: return "BB"
            case .c: return "C"
            case .d: return "D"
            case .stepBack: return "◀︎|"
            case .stepForward: return "|▶︎"
            default: return b.rawValue.uppercased()
            }
        }
    }

    /// How an extra is drawn: the transport buttons are SF Symbol pills, AA / BB rounds.
    static func style(_ k: TouchExtra.Kind) -> TouchExtra.Style {
        guard case .button(let b) = k else { return .pill }
        switch b {
        case .l2: return .icon("backward.fill")
        case .r2: return .icon("forward.fill")
        case .stepBack: return .icon("backward.frame.fill")
        case .stepForward: return .icon("forward.frame.fill")
        case .slow: return .icon("tortoise.fill")
        case .pause: return .icon("pause.fill")
        case .turboA, .turboB, .c, .d: return .round
        default: return .pill
        }
    }

    /// The transport row (Rewind, Step Back, Step Forward, Fast-forward) centred on `midX` with its
    /// bottom at `bottom`, at most `width` wide: Rewind / Fast-forward `transportWide` (less when
    /// that does not fit, down to `2 × minTarget`), the steps half as wide.
    static func transport(midX: CGFloat, bottom: CGFloat, width: CGFloat) -> [TouchExtra] {
        let g = transportGap
        // Whole points, even (the steps are exactly half).
        let wide = (min(transportWide, max(2 * minTarget, (width - 3 * g) / 3)) / 2).rounded(.down) * 2
        let widths = transportOrder.map { $0 == .l2 || $0 == .r2 ? wide : wide / 2 }
        var x = midX - (widths.reduce(0, +) + 3 * g) / 2
        var out: [TouchExtra] = []
        for (b, w) in zip(transportOrder, widths) {
            let k = TouchExtra.Kind.button(b)
            out.append(TouchExtra(kind: k, frame: CGRect(x: x, y: bottom - transportHeight, width: w, height: transportHeight), style: style(k)))
            x += w + g
        }
        return out
    }

    /// AA / BB up-left of A / B: diameter and the distance from the A (B) centre to the AA (BB)
    /// centre (the discs stay `turboGap` apart).
    static func turboMetrics(btn: CGFloat) -> (size: CGFloat, step: CGFloat) {
        let s = (btn * turboScale).rounded()
        return (s, (btn + s) / 2 + turboGap)
    }

    /// The direction from A (B) to AA (BB): 11 o'clock, `turboAngle` left of straight up. Unit
    /// vector, screen coordinates (y down). (`btn` kept for the callers; the angle does not depend
    /// on it.)
    static func turboDirection(btn: CGFloat) -> CGVector {
        let r = turboAngle * .pi / 180
        return CGVector(dx: -sin(r), dy: -cos(r))
    }

    /// The offset from A (B) to AA (BB): `turboDirection` × `step`, turned toward straight up (same
    /// distance, so the discs stay as far from A / B) only as far as needed to keep BB's disc right
    /// of `minLeft` (portrait: clear of the D-pad; landscape: the picture's edge).
    static func turboOffset(btn: CGFloat, b: CGPoint, minLeft: CGFloat) -> CGVector {
        let t = turboMetrics(btn: btn), u = turboDirection(btn: btn)
        var v = CGVector(dx: u.dx * t.step, dy: u.dy * t.step)
        if b.x + v.dx - t.size / 2 < minLeft {
            v.dx = min(0, minLeft + t.size / 2 - b.x)
            v.dy = -(max(0, t.step * t.step - v.dx * v.dx)).squareRoot()
        }
        return v
    }

    /// How far AA / BB (top of the disc) reach above the A/B midpoint.
    static func turboAbove(btn: CGFloat, offset v: CGVector) -> CGFloat {
        (btn + faceGap) * faceTilt - v.dy + turboMetrics(btn: btn).size / 2
    }

    private static func turboExtras(a: CGPoint, b: CGPoint, btn: CGFloat, offset v: CGVector, _ o: Options) -> [TouchExtra] {
        let t = turboMetrics(btn: btn)
        func disc(_ c: CGPoint) -> CGRect {
            CGRect(x: c.x + v.dx - t.size / 2, y: c.y + v.dy - t.size / 2, width: t.size, height: t.size)
        }
        // B first (the left one), as .gp-face lists its rounds. Buttons 3 / 4 take AA's / BB's place;
        // a turbo disc only with the turbo setting on.
        let left: TouchButton? = o.buttons >= 4 ? .d : o.showAABB ? .turboB : nil
        let right: TouchButton? = o.buttons >= 3 ? .c : o.showAABB ? .turboA : nil
        return [left.map { TouchExtra(kind: .button($0), frame: disc(b), style: .round) },
                right.map { TouchExtra(kind: .button($0), frame: disc(a), style: .round) }].compactMap { $0 }
    }

    /// The A/B midpoint for a face cluster whose AA / BB (if shown) reach `above` over it and whose
    /// .gp-face box reaches `below` under it: the row's middle (the CSS's align-self: center),
    /// moved down only as far as needed to keep AA / BB inside the row.
    private static func faceMid(row: CGRect, top: CGFloat, above: CGFloat, below: CGFloat) -> CGFloat {
        min(max(row.midY, top + above), row.maxY - below)
    }

    /// A text pill (8-WAY, MENU) of the .bb-mode-switch style at x, vertically centred on `midY`.
    private static func pill(_ k: TouchExtra.Kind, x: CGFloat, midY: CGFloat, ways: Int) -> TouchExtra {
        let s = switchSize(label(k, ways: ways))
        return TouchExtra(kind: k, frame: CGRect(x: x, y: midY - s.height / 2, width: s.width, height: s.height), style: style(k))
    }

    /// The A/B pad: `.gp-face` box (padding included) at (x, y) and the centres of the rounds.
    /// display: flex; align-items: center; B first; .gp-b translateY(+off), .gp-a translateY(-off).
    private static func face(x: CGFloat, midY: CGFloat, btn: CGFloat, padX: CGFloat, padY: CGFloat) -> (box: CGRect, a: CGPoint, b: CGPoint) {
        let off = (btn + faceGap) * faceTilt
        let w = 2 * padX + 2 * btn + faceGap
        let h = btn + 2 * padY
        let box = CGRect(x: x, y: midY - h / 2, width: w, height: h)
        let b = CGPoint(x: x + padX + btn / 2 + bTowardA * btn, y: midY + off)
        let a = CGPoint(x: x + padX + btn + faceGap + btn / 2, y: midY - off)
        return (box, a, b)
    }

    // MARK: portrait (#app:not(.bb-side))

    private static func portrait(_ size: CGSize, _ safe: TouchInsets, _ o: Options) -> TouchLayout {
        let W = size.width, H = size.height
        // The picture on top, full width (ReplayNES; NESTERM's top bar is not used, see
        // TOUCH_PORT.md). app.mjs resizeScreen shrinks the screen row to the picture so the
        // controls get the rest.
        let gameH = (W / max(o.rowAspect, 0.01)).rounded()
        // The controls stay where the NES picture's row puts them. Portrait FILL: a picture taller than
        // that row (a vertical arcade game) still spans the screen's width, its lower part under the
        // controls (as landscape FILL, decided 2026-10-30); a wider picture fits the row as before.
        let fullH = o.fill ? (o.pictureAspect.map { (W / max($0, 0.01)).rounded() } ?? gameH) : gameH
        let game = CGRect(x: 0, y: safe.top, width: W, height: max(gameH, fullH))

        // NESTERM's sub bar (.bb-subbar) is not used: rewind / fast-forward are the picture's side
        // quarters, frame steps the paused seek bar's (2026-10-30). The gamepad starts right under the
        // picture's row.
        let subbar = CGRect(x: 0, y: safe.top + gameH, width: W, height: 0)
        var extras: [TouchExtra] = []

        // #app:not(.bb-side) > .bb-root > .bb-gamepad { flex: 1 1 auto;
        //   grid-template-rows: minmax(0, 1fr) auto; padding: 6px 12px calc(6px + safe-bottom);
        //   padding-inline: clamp(12px, 4.5vw, 24px); gap: 4px 8px; clip-path: inset(0) }
        let gamepad = CGRect(x: 0, y: subbar.maxY, width: W, height: max(0, H - subbar.maxY))
        let pi = min(max(12, 0.045 * W), 24)
        let contentTop = gamepad.minY + 6
        let contentBottom = gamepad.maxY - (6 + safe.bottom)
        // Row 2: .gp-system { padding: 4px 0 } around the pills (min-height: 40px).
        let sel = pillSize("SELECT", minHeight: 40), sta = pillSize("START", minHeight: 40)
        let systemH = max(sel.height, sta.height) + 8
        let row1 = CGRect(x: pi, y: contentTop, width: W - 2 * pi, height: max(0, contentBottom - contentTop - 4 - systemH))
        let row2 = CGRect(x: pi, y: row1.maxY + 4, width: W - 2 * pi, height: systemH)

        // #app:not(.bb-side) .gp-face { justify-self: end;
        //   --gp-btn: clamp(48px, min(19vw, el-h * .13), 78px); padding: calc(var(--gp-off) + 4px) 4px }
        let btn = min(max(48, min(0.19 * W, 0.13 * H)), 78)
        let off = (btn + faceGap) * faceTilt
        let faceW = 2 * 4 + 2 * btn + faceGap
        let faceX = W - pi - faceW
        // #app:not(.bb-side) .gp-dpad { width: min(var(--bb-dpad, 144px), max(112px, el-h * .24)) };
        // .gp-dpad { justify-self: start; margin-left: 2px }; align-items: center - here × 1.1 and
        // centred on the A/B midpoint (ReplayNES).
        let d = (min(o.dpadSize, max(112, H * 0.24)) * dpadScale).rounded()
        // AA / BB up-left of A / B, BB kept `turboGap` clear of the D-pad (narrow screens).
        let tv = turboOffset(btn: btn, b: CGPoint(x: faceX + 4 + btn / 2 + bTowardA * btn, y: 0), minLeft: pi + 2 + d + turboGap)
        let below = btn / 2 + off + 4
        let above = o.showRounds ? turboAbove(btn: btn, offset: tv) : below
        let m = faceMid(row: row1, top: row1.minY, above: above, below: below)
        let f = face(x: faceX, midY: m, btn: btn, padX: 4, padY: off + 4)
        let dpad = CGRect(x: pi + 2, y: m - d / 2, width: d, height: d)
        if o.showRounds { extras += turboExtras(a: f.a, b: f.b, btn: btn, offset: tv, o) }
        // Slow motion: in the top band, left of the Dynamic Island (on the left like a controller's L
        // side), centred on the island's height.
        let slowX = W / 2 - islandHalfWidth - islandGap - slowSize.width
        extras.append(TouchExtra(kind: .button(.slow), frame: CGRect(x: max(pi, slowX), y: max(0, islandMidY(safe.top) - slowSize.height / 2),
                                                                     width: slowSize.width, height: slowSize.height), style: style(.button(.slow))))

        // .gp-system { display: flex; justify-content: center; gap: 22px }.
        let total = sel.width + 22 + sta.width
        let px = row2.minX + (row2.width - total) / 2
        let select = CGRect(x: px, y: row2.minY + 4, width: sel.width, height: sel.height)
        let start = CGRect(x: select.maxX + 22, y: row2.minY + 4, width: sta.width, height: sta.height)
        // 8-WAY and MENU at the ends of the SELECT / START row (ReplayNES: the sub bar holds the
        // transport row).
        extras.append(pill(.ways, x: row2.minX, midY: select.midY, ways: o.ways))
        let menuW = switchSize(label(.button(.menu), ways: o.ways)).width
        let menu = pill(.button(.menu), x: row2.maxX - menuW, midY: select.midY, ways: o.ways)
        extras.append(menu)
        // Pause right above MENU, centred on it.
        let pauseSlop = max(0, (minTarget - slowSize.height) / 2)   // its touch area below the pill
        extras.append(TouchExtra(kind: .button(.pause), frame: CGRect(x: menu.frame.midX - slowSize.width / 2,
                                                                      y: menu.hitFrame.minY - pauseGap - pauseSlop - slowSize.height,
                                                                      width: slowSize.width, height: slowSize.height), style: style(.button(.pause))))

        var g = TouchGamepad.Geometry()
        g.viewport = size
        g.frame = CGRect(origin: .zero, size: size) // .bb-root has no padding in portrait
        g.dpad = dpad
        g.dpadClip = gamepad
        g.face = f.box
        g.a = f.a
        g.b = f.b
        g.system = row2
        g.select = select
        g.start = start
        var l = TouchLayout(size: size, landscape: false, gameArea: game, pad: g, faceButton: btn,
                            select: select, start: start, extras: extras)
        let controlsTop = ([dpad.minY, f.a.y - btn / 2, f.b.y - btn / 2] + extras.filter { $0.style == .round }.map(\.frame.minY)).min()!
        l.overlayArea = CGRect(x: 0, y: game.minY, width: W, height: max(0, min(game.maxY, controlsTop - overlayGap) - game.minY))
        return l
    }

    // MARK: landscape (.bb-side)

    private static func landscape(_ size: CGSize, _ safe: TouchInsets, _ o: Options) -> TouchLayout {
        let W = size.width, H = size.height
        // .bb-side > .bb-root { padding: 0 calc(safe-right + edge-gap) 0 calc(safe-left + edge-gap);
        //   grid-template-columns: minmax(144px, 16%) 1fr minmax(144px, 16%);
        //   grid-template-rows: auto auto 1fr auto;
        //   grid-template-areas: "info screen icons" "ways screen repeat" "dpad screen face" "select screen start" }
        // (--bb-edge-gap-* resolve to 0 on iOS: min(0px, inset * 1000).)
        let content = CGRect(x: safe.left, y: 0, width: max(0, W - safe.left - safe.right), height: H)
        let col = max(144, 0.16 * content.width)
        let leftCol = CGRect(x: content.minX, y: 0, width: col, height: H)
        let rightCol = CGRect(x: content.maxX - col, y: 0, width: col, height: H)

        // Row 1 (info / icons, margin-top: safe-top): slow motion and pause (2026-10-30), their 44 pt
        // touch areas from the top, `pauseGap` above row 2's.
        let pillH = switchSize("MENU").height
        let row1H = (safe.top + minTarget + pauseGap + max(0, (minTarget - pillH) / 2)).rounded(.up)
        // Row 2 (ways / repeat): the switches.
        let row2 = CGRect(x: 0, y: row1H, width: W, height: pillH)
        // Row 4 (select / start): .gp-pill { align-self: end; margin-bottom: calc(8px + safe-bottom) }.
        let sel = pillSize("SELECT", minHeight: 0), sta = pillSize("START", minHeight: 0)
        let row4H = max(sel.height, sta.height) + 8 + safe.bottom
        let row3 = CGRect(x: 0, y: row2.maxY, width: W, height: max(0, H - row2.maxY - row4H))

        let ways = o.ways
        func centred(_ k: TouchExtra.Kind, in c: CGRect, midY: CGFloat) -> TouchExtra {
            pill(k, x: c.midX - switchSize(label(k, ways: ways)).width / 2, midY: midY, ways: ways)
        }
        // #ways-switch / #turbo-switch { justify-self: center; align-self: center }; MENU in
        // NESTERM's repeat switch place. Row 1 stays empty (the transport is under the picture).
        var extras = [centred(.ways, in: leftCol, midY: row2.midY), centred(.button(.menu), in: rightCol, midY: row2.midY)]
        // Row 1: slow motion above 8-WAY (the left, like a controller's L side), pause above MENU.
        let row1Mid = safe.top + minTarget / 2
        for (b, c) in [(TouchButton.slow, leftCol), (.pause, rightCol)] {
            extras.append(TouchExtra(kind: .button(b), frame: CGRect(x: c.midX - slowSize.width / 2, y: row1Mid - slowSize.height / 2,
                                                                     width: slowSize.width, height: slowSize.height), style: style(.button(b))))
        }

        // .gp-face { --gp-btn: min(19vw, 78px); padding: 48px 16px }; justify-self: end; align-self: center.
        let btn = min(0.19 * W, 78)
        let faceW = 2 * 16 + 2 * btn + faceGap
        var faceX = rightCol.maxX - faceW
        // AA / BB up-left of A / B without narrowing the picture: B's disc edge (CSS place) is the
        // limit; the face moves right into its 16 px padding first (keeping A 8 pt from the screen
        // edge), then AA / BB turn toward straight up as far as needed.
        var tv = CGVector.zero
        if o.showRounds {
            let bLeft = faceX + 16 + bTowardA * btn
            let t = turboMetrics(btn: btn), u = turboDirection(btn: btn)
            let protrude = max(0, -(u.dx * t.step) + t.size / 2 - btn / 2)
            faceX += min(protrude, 16, 8 + safe.right)
            tv = turboOffset(btn: btn, b: CGPoint(x: faceX + 16 + btn / 2 + bTowardA * btn, y: 0), minLeft: bLeft)
        }
        let below = btn / 2 + 48
        let above = o.showRounds ? turboAbove(btn: btn, offset: tv) : below
        let m = faceMid(row: row3, top: row2.maxY + 4, above: above, below: below)
        var f = face(x: faceX, midY: m, btn: btn, padX: 16, padY: 48)
        f.box = f.box.intersection(CGRect(x: 0, y: 0, width: W, height: H))
        if o.showRounds { extras += turboExtras(a: f.a, b: f.b, btn: btn, offset: tv, o) }

        // .bb-side > .bb-root .gp-dpad { justify-self: start; margin-inline-start: var(--bb-ctl-inset)
        //   (16px); align-self: center; width: var(--bb-dpad, 144px) } - × 1.1, on the A/B midpoint.
        let d = (o.dpadSize * dpadScale).rounded()
        let dpad = CGRect(x: leftCol.minX + 16, y: m - d / 2, width: d, height: d)

        let pillBottom = H - 8 - safe.bottom
        let select = CGRect(x: leftCol.midX - sel.width / 2, y: pillBottom - sel.height, width: sel.width, height: sel.height)
        let start = CGRect(x: rightCol.midX - sta.width / 2, y: pillBottom - sta.height, width: sta.width, height: sta.height)

        // The picture fits between the control columns (ReplayNES; NESTERM draws the controls over
        // a full-screen picture): symmetric about the screen's centre, `pictureMargin` clear of the
        // nearest visible control (D-pad, A / B / AA / BB discs, pills), above the transport row.
        let leftEdge = ([dpad.maxX, select.maxX] + extras.filter { $0.frame.midX < W / 2 }.map(\.frame.maxX)).max()!
        let rightEdge = ([f.b.x - btn / 2, f.a.x - btn / 2, start.minX] + extras.filter { $0.frame.midX >= W / 2 }.map(\.frame.minX)).min()!
        let half = max(0, min(W / 2 - leftEdge, rightEdge - W / 2) - pictureMargin)
        // No transport row (2026-10-30): the picture has the full height between the columns.
        let game = o.fill ? CGRect(x: 0, y: 0, width: W, height: H) : CGRect(x: W / 2 - half, y: 0, width: 2 * half, height: H)

        var g = TouchGamepad.Geometry()
        g.viewport = size
        // The .bb-root content box (its padding is the safe area), its right side ending at the
        // picture (ReplayNES): the pad's home may follow the thumb up to the picture, not over it.
        g.frame = CGRect(x: content.minX, y: 0, width: max(dpad.maxX, game.minX) + EDGE_MARGIN - content.minX, height: H)
        g.dpad = dpad
        g.dpadClip = nil // .bb-gamepad is display: contents: no clip
        g.face = f.box
        g.a = f.a
        g.b = f.b
        g.system = nil // .gp-system is display: contents: only the pills take touches
        g.select = select
        g.start = start
        var l = TouchLayout(size: size, landscape: true, gameArea: game, pad: g, faceButton: btn,
                            select: select, start: start, extras: extras)
        l.overlayArea = CGRect(x: W / 2 - half, y: 0, width: 2 * half, height: H)
        return l
    }
}
