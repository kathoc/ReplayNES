// The practice countdown's picture (3, 2, 1 after the run returned to A): a large numeral on a soft
// dark disc with a thin ring running down the second. The animation (fade / scale) is the shared
// core's (rnf_practice_countdown_visual), the same look as the desktop frontends (ui_play.cpp);
// MetalView draws it inside the game's own pass.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

enum CountdownOverlay {
    /// One line of text on a dark capsule, `height` px tall: (premultiplied BGRA, width, height).
    static func renderNote(_ text: String, height: Int) -> ([UInt8], Int, Int)? {
        let h = max(8, height)
        let font = NSFont.systemFont(ofSize: CGFloat(h) * 0.55, weight: .semibold)
        let attr = NSAttributedString(string: text, attributes: [.font: font, .foregroundColor: NSColor.white])
        let w = Int((attr.size().width + CGFloat(h) * 1.1).rounded(.up))
        guard w > 0 else { return nil }
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        let ok = bytes.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
            else { return false }
            let r = CGFloat(h) / 2
            ctx.setFillColor(CGColor(srgbRed: 20 / 255, green: 20 / 255, blue: 22 / 255, alpha: 0.7))
            ctx.addPath(CGPath(roundedRect: CGRect(x: 0, y: 0, width: w, height: h), cornerWidth: r, cornerHeight: r, transform: nil))
            ctx.fillPath()
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
            let ts = attr.size()
            attr.draw(at: CGPoint(x: (CGFloat(w) - ts.width) / 2, y: (CGFloat(h) - ts.height) / 2))
            NSGraphicsContext.restoreGraphicsState()
            return true
        }
        return ok ? (bytes, w, h) : nil
    }

    /// side x side premultiplied BGRA (top row first), or nil.
    static func render(_ v: rnf_countdown_visual, side: Int) -> [UInt8]? {
        var bytes = [UInt8](repeating: 0, count: side * side * 4)
        let ok = bytes.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: side, height: side, bitsPerComponent: 8, bytesPerRow: side * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
            else { return false }
            let a = CGFloat(v.alpha)
            let s = CGFloat(side), r = s / 2, c = CGPoint(x: r, y: r)
            ctx.setFillColor(CGColor(srgbRed: 20 / 255, green: 20 / 255, blue: 22 / 255, alpha: 0.62 * a))
            ctx.fillEllipse(in: CGRect(x: 0, y: 0, width: s, height: s))
            let ringW = max(2, r * 0.045), rr = r - ringW * 1.6
            ctx.setLineWidth(ringW)
            ctx.setStrokeColor(CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.14 * a))
            ctx.strokeEllipse(in: CGRect(x: c.x - rr, y: c.y - rr, width: rr * 2, height: rr * 2))
            if v.ring > 0.002 {
                // From 12 o'clock, clockwise (CoreGraphics is y-up here).
                ctx.setStrokeColor(CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.85 * a))
                ctx.setLineCap(.butt)
                ctx.addArc(center: c, radius: rr, startAngle: .pi / 2, endAngle: .pi / 2 - 2 * .pi * CGFloat(v.ring), clockwise: true)
                ctx.strokePath()
            }
            let ns = NSGraphicsContext(cgContext: ctx, flipped: false)
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = ns
            let font = NSFont.monospacedDigitSystemFont(ofSize: r * 1.25 * CGFloat(v.scale), weight: .semibold)
            let text = NSAttributedString(string: "\(v.number)", attributes: [
                .font: font, .foregroundColor: NSColor(srgbRed: 1, green: 1, blue: 1, alpha: a),
            ])
            let ts = text.size()
            // Optical centre: the numeral's cap height on the centre, not the line box (draw(at:) is
            // the line's lower left; the baseline is |descender| above it).
            let baseline = c.y - font.capHeight / 2
            text.draw(at: CGPoint(x: c.x - ts.width / 2, y: baseline + font.descender))
            NSGraphicsContext.restoreGraphicsState()
            return true
        }
        return ok ? bytes : nil
    }
}
