// Geometry of the streaming (Syphon) output picture: canvas size and where the 256x240 game
// frame is drawn into it (nearest neighbour). Computed by the shared frontend core
// (rnf_stream_output_layout); the raw values below are persisted in user defaults.
// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics

enum StreamOutputSize: String, CaseIterable, Identifiable {
    case x1, x2, x3, x4, w1280, w1920

    static let `default` = StreamOutputSize.x4
    var id: String { rawValue }

    var cValue: rnf_stream_size {
        switch self {
        case .x1: return RNF_STREAM_X1
        case .x2: return RNF_STREAM_X2
        case .x3: return RNF_STREAM_X3
        case .x4: return RNF_STREAM_X4
        case .w1280: return RNF_STREAM_W1280
        case .w1920: return RNF_STREAM_W1920
        }
    }

    /// Integer multiple of the frame, or nil for a fixed 4:3 canvas.
    var multiple: Int? {
        let n = rnf_stream_output_multiple(cValue)
        return n > 0 ? Int(n) : nil
    }

    func label(par87: Bool) -> String { rnfString(rnf_stream_output_label(cValue, par87 ? 1 : 0)) }
}

struct StreamOutputLayout: Equatable {
    struct Size: Equatable { var width: Int; var height: Int }
    /// Output texture size.
    let canvas: Size
    /// Destination of the full 256x240 frame inside the canvas (pixels, origin top-left).
    let picture: CGRect

    static let frameWidth = Int(RN_VIDEO_WIDTH), frameHeight = Int(RN_VIDEO_HEIGHT)

    init(size: StreamOutputSize, par87: Bool) {
        let l = rnf_stream_output_layout(size.cValue, par87 ? 1 : 0)
        canvas = Size(width: Int(l.canvas_width), height: Int(l.canvas_height))
        picture = CGRect(x: l.x, y: l.y, width: l.width, height: l.height)
    }
}
