// The Display options and where the picture goes on the screen (MetalView; the iPhone's portrait row).
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics

struct DisplayOptions: Equatable {
    var integerScale = true
    var pixelAspect87 = false
    var hideOverscan = true
    /// The picture's size, rotation and pixel aspect (EmuStatus.geometry). The 8:7 option and the
    /// overscan crop are for the NES picture; another core's picture uses its own pixel aspect.
    var geometry = PictureGeometry.nes
    /// The empty sides show the picture enlarged, blurred and dimmed (docs/design/BACKDROP.md).
    var backdrop = 2  // 0 none, 1 sharp, 2 low, 3 medium, 4 high (render/backdrop.h backdropTexels)

    /// The shown part of the picture (texels, upright) and its pixel aspect: the NES picture with the 8:7
    /// option and the overscan crop (8 texels hidden on every side), another core's picture with its own
    /// pixel aspect after its rotation, never cropped. MetalView.viewport draws it; the iPhone's portrait
    /// layout sizes the picture's row with it (displayedAspect).
    var shownSource: (width: Double, height: Double, pixelAspect: Double, crop: Int) {
        let g = geometry
        let nes = g.isNESPicture
        let crop = nes && hideOverscan ? 8 : 0
        var w = Double(g.width - 2 * crop), h = Double(g.height - 2 * crop)
        var par = nes ? (pixelAspect87 ? 8.0 / 7.0 : 1.0) : g.pixelAspect
        // A quarter turn: the source's rows become columns (a pixel par wide is then par high).
        if g.rotation % 2 == 1 { (w, h) = (h, w); par = 1 / par }
        return (w, h, par, crop)
    }

    /// The shown picture's width / height on the screen (shownSource).
    var displayedAspect: CGFloat {
        let s = shownSource
        return s.height > 0 ? CGFloat(s.width * s.pixelAspect / s.height) : 256.0 / 224.0
    }

    /// CRT Display: the tube face's width / height (the 4:3 raster minus the hidden overscan rows) and the
    /// fraction of its height hidden at the top and at the bottom.
    var crtFace: (aspect: Double, cropFraction: Double) {
        let cropFraction = (hideOverscan ? 8.0 : 0.0) / 240
        return ((4.0 / 3.0) / (1 - 2 * cropFraction), cropFraction)
    }

    /// CRT Display: the shown part's width / height (activeWidth of the face width, the face's height:
    /// CRTRenderer.shownRect), 256 x 224 at 8:7 with the defaults.
    func crtShownAspect(activeWidth: Double) -> CGFloat {
        CGFloat(crtFace.aspect * Self.shownFraction(activeWidth))
    }

    static func shownFraction(_ activeWidth: Double) -> Double { activeWidth > 0 && activeWidth < 1 ? activeWidth : 1 }

    /// CRT destination (pixels, origin top-left): the tube face, its shown part (activeWidth of the face
    /// width, centred) as tall as the plain viewport would be (integer scale or FILL; no 8:7, the tube is
    /// 4:3 by construction) and no wider than the drawable. The face beside the shown part may lie outside
    /// the drawable (the show pass is clipped to the shown part). Sizing the whole face to the drawable's
    /// width left the shown part 32/35 of it, inside black, in portrait FILL (2026-11-19).
    func crtViewport(drawableSize size: CGSize, activeWidth: Double) -> (CGRect, Double) {
        let (aspect, cropFraction) = crtFace
        let a = Self.shownFraction(activeWidth)
        var plain = self
        plain.pixelAspect87 = false
        var h = plain.viewport(drawableSize: size).0.height
        if (h * aspect * a).rounded() > size.width { h = (size.width / (aspect * a)).rounded() }
        let wd = (h * aspect).rounded()
        let x0 = ((size.width - wd) / 2).rounded(), y0 = ((size.height - h) / 2).rounded()
        return (CGRect(x: x0, y: y0, width: wd, height: h), cropFraction)
    }

    /// Destination rectangle (pixels, origin bottom-left) and overscan crop (pixels hidden on every side)
    /// for a drawable size: the shown source as large as the drawable allows (whole multiples with
    /// integerScale), centred. MetalView draws there.
    func viewport(drawableSize size: CGSize) -> (CGRect, Int) {
        let (srcW, srcH, par, crop) = shownSource
        var scale = min(size.width / (srcW * par), size.height / srcH)
        if integerScale && scale >= 1 { scale = floor(scale) }
        let dw = (srcW * par * scale).rounded(), dh = (srcH * scale).rounded()
        let x0 = ((size.width - dw) / 2).rounded(), y0 = ((size.height - dh) / 2).rounded()
        return (CGRect(x: x0, y: y0, width: dw, height: dh), crop)
    }
}
