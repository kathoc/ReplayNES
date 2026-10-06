// Geometry of the streaming (Syphon) output picture: canvas size and where the 256x240 game
// frame is drawn into it (nearest neighbour). Pure value code, shared with the tests.
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics

enum StreamOutputSize: String, CaseIterable, Identifiable {
    case x1, x2, x3, x4, w1280, w1920

    static let `default` = StreamOutputSize.x4
    var id: String { rawValue }

    /// Integer multiple of the frame, or nil for a fixed 4:3 canvas.
    var multiple: Int? {
        switch self {
        case .x1: return 1
        case .x2: return 2
        case .x3: return 3
        case .x4: return 4
        case .w1280, .w1920: return nil
        }
    }

    func label(par87: Bool) -> String {
        let c = StreamOutputLayout(size: self, par87: par87).canvas
        switch multiple {
        case 1: return "原寸 \(c.width)×\(c.height)"
        case let n?: return "\(n)倍 \(c.width)×\(c.height)"
        case nil: return "\(c.width)×\(c.height)（4:3・黒帯あり）"
        }
    }
}

struct StreamOutputLayout: Equatable {
    struct Size: Equatable { var width: Int; var height: Int }
    /// Output texture size.
    let canvas: Size
    /// Destination of the full 256x240 frame inside the canvas (pixels, origin top-left).
    let picture: CGRect

    static let frameWidth = 256, frameHeight = 240

    init(size: StreamOutputSize, par87: Bool) {
        let fw = Double(Self.frameWidth), fh = Double(Self.frameHeight)
        let par = par87 ? 8.0 / 7.0 : 1.0
        if let n = size.multiple {
            // The canvas is exactly the scaled frame: no borders.
            let w = Int((fw * par * Double(n)).rounded()), h = Self.frameHeight * n
            canvas = Size(width: w, height: h)
            picture = CGRect(x: 0, y: 0, width: w, height: h)
        } else {
            let (cw, ch) = size == .w1280 ? (1280, 960) : (1920, 1440)
            // Largest scale that fits; integer when the aspect allows it (it does for both canvases).
            let s = min(Double(cw) / (fw * par), Double(ch) / fh)
            let pw = (fw * par * s).rounded(), ph = (fh * s).rounded()
            canvas = Size(width: cw, height: ch)
            picture = CGRect(x: ((Double(cw) - pw) / 2).rounded(.down), y: ((Double(ch) - ph) / 2).rounded(.down), width: pw, height: ph)
        }
    }
}
