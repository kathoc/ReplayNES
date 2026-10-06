// Geometry of the Syphon streaming output (canvas size, picture placement).
// The live server/client path is checked end to end by scripts/check-syphon-macos.sh.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class StreamOutputTests: XCTestCase {
    private func check(_ s: StreamOutputSize, _ par: Bool, canvas: (Int, Int), picture: CGRect,
                       file: StaticString = #filePath, line: UInt = #line) {
        let l = StreamOutputLayout(size: s, par87: par)
        XCTAssertEqual(l.canvas, .init(width: canvas.0, height: canvas.1), file: file, line: line)
        XCTAssertEqual(l.picture, picture, file: file, line: line)
    }

    func testIntegerMultiplesFillTheCanvas() {
        check(.x1, false, canvas: (256, 240), picture: CGRect(x: 0, y: 0, width: 256, height: 240))
        check(.x2, false, canvas: (512, 480), picture: CGRect(x: 0, y: 0, width: 512, height: 480))
        check(.x3, false, canvas: (768, 720), picture: CGRect(x: 0, y: 0, width: 768, height: 720))
        check(.x4, false, canvas: (1024, 960), picture: CGRect(x: 0, y: 0, width: 1024, height: 960))
        // 8:7 pixel aspect widens the canvas, the height stays an exact multiple.
        check(.x1, true, canvas: (293, 240), picture: CGRect(x: 0, y: 0, width: 293, height: 240))
        check(.x4, true, canvas: (1170, 960), picture: CGRect(x: 0, y: 0, width: 1170, height: 960))
    }

    func testFixedCanvasesAreCentredWithBars() {
        check(.w1280, false, canvas: (1280, 960), picture: CGRect(x: 128, y: 0, width: 1024, height: 960))
        check(.w1280, true, canvas: (1280, 960), picture: CGRect(x: 55, y: 0, width: 1170, height: 960))
        check(.w1920, false, canvas: (1920, 1440), picture: CGRect(x: 192, y: 0, width: 1536, height: 1440))
        check(.w1920, true, canvas: (1920, 1440), picture: CGRect(x: 82, y: 0, width: 1755, height: 1440))
    }

    func testDefaultAndLabels() {
        XCTAssertEqual(StreamOutputSize.default, .x4)
        XCTAssertEqual(StreamOutputSize.x1.label(par87: false), "Native 256×240")
        XCTAssertEqual(StreamOutputSize.x4.label(par87: false), "4× 1024×960")
        XCTAssertTrue(StreamOutputSize.w1280.label(par87: true).hasPrefix("1280×960"))
        // Raw values are persisted in user defaults: keep them stable.
        XCTAssertEqual(StreamOutputSize.allCases.map(\.rawValue), ["x1", "x2", "x3", "x4", "w1280", "w1920"])
    }
}
