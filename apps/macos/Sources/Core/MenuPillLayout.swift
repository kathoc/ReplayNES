// Geometry and fading of the always-visible menu pill ("☰ Menu  L+R", docs/design/UI_REDESIGN.md):
// top-right of the game area, over the picture only when the picture reaches that corner (FILL);
// there it fades to 30 % after 3 s of play. Shared by the Metal pass (GameRenderer draws the pill
// into the game's own drawable while playing) and the SwiftUI pill (paused, menus, library).
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics

enum MenuPillLayout {
    /// Distance from the game area's top and right edges (points).
    static let margin: CGFloat = 12
    static let fadeDelay = 3.0
    static let fadeDuration = 0.3
    static let dimmedAlpha = 0.3

    /// The pill's rectangle in pixels, origin top-left, for a game area of `areaSize` pixels.
    static func rect(areaSize: CGSize, pillSize: CGSize, scale: CGFloat) -> CGRect {
        let m = (margin * scale).rounded()
        return CGRect(x: (areaSize.width - m - pillSize.width).rounded(), y: m, width: pillSize.width, height: pillSize.height)
    }

    /// Whether the pill lies over the picture (`picture`: pixels, origin top-left).
    static func overPicture(_ pill: CGRect, picture: CGRect) -> Bool { pill.intersects(picture) }

    /// Opacity `elapsed` seconds after the last pointer activity / resume.
    static func alpha(elapsed: Double, overPicture: Bool) -> Double {
        guard overPicture, elapsed > fadeDelay else { return 1 }
        let t = min(1, (elapsed - fadeDelay) / fadeDuration)
        return 1 - (1 - dimmedAlpha) * t
    }

    /// The label after the menu word: the chord on the connected controller family, Esc without one.
    static func glyph(hasController: Bool, playStation: Bool) -> String {
        guard hasController else { return "Esc" }
        return playStation ? "L1+R1" : "L+R"
    }
}
