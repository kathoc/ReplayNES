// Debug/smoke-test helper: `--snapshot <png>` writes a capture of the main window (with the
// current emulated frame composited into the Metal viewport, since layer-backed Metal content
// is not captured by cacheDisplay) plus a JSON with runtime statistics.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import MetalKit

extension AppModel {
    func writeSnapshot(to url: URL) {
        guard let window = mainWindow, let content = window.contentView else { return }
        let scale = window.backingScaleFactor
        guard let rep = content.bitmapImageRepForCachingDisplay(in: content.bounds) else { return }
        content.cacheDisplay(in: content.bounds, to: rep)

        // Current emulated frame as CGImage.
        var frameImage: CGImage?
        var frameIndex: UInt64 = 0
        _ = emu.frames.readIfNewer(than: .max) { px, meta in
            frameIndex = meta.frame
            let data = Data(bytes: px, count: Int(RN_VIDEO_WIDTH * RN_VIDEO_HEIGHT) * 4)
            if let provider = CGDataProvider(data: data as CFData) {
                frameImage = CGImage(width: Int(RN_VIDEO_WIDTH), height: Int(RN_VIDEO_HEIGHT), bitsPerComponent: 8, bitsPerPixel: 32,
                                     bytesPerRow: Int(RN_VIDEO_WIDTH) * 4, space: CGColorSpaceCreateDeviceRGB(),
                                     bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                                     provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
            }
        }
        if let img = frameImage, let mtk = Self.findMTKView(content), let ctx = NSGraphicsContext(bitmapImageRep: rep) {
            var viewRect = mtk.convert(mtk.bounds, to: content)
            if content.isFlipped { viewRect.origin.y = content.bounds.height - viewRect.maxY } // bitmap context is bottom-left
            let px = CGSize(width: viewRect.width * scale, height: viewRect.height * scale)
            let (r, crop) = GameRenderer.viewport(drawableSize: px, options: displayOptions)
            let cropped = img.cropping(to: CGRect(x: 0, y: crop, width: Int(RN_VIDEO_WIDTH), height: Int(RN_VIDEO_HEIGHT) - 2 * crop)) ?? img
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = ctx
            ctx.imageInterpolation = .none
            let dst = CGRect(x: viewRect.minX + r.minX / scale, y: viewRect.minY + r.minY / scale, width: r.width / scale, height: r.height / scale)
            ctx.cgContext.interpolationQuality = .none
            ctx.cgContext.draw(cropped, in: dst)
            NSGraphicsContext.restoreGraphicsState()
        }
        if let png = rep.representation(using: .png, properties: [:]) { try? png.write(to: url) }
        // Second capture through the layer tree of the whole window frame (includes toolbar).
        if let frameView = content.superview, let layer = frameView.layer {
            let size = frameView.bounds.size
            if let ctx = CGContext(data: nil, width: Int(size.width * scale), height: Int(size.height * scale), bitsPerComponent: 8,
                                   bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) {
                ctx.scaleBy(x: scale, y: scale)
                if frameView.isFlipped { ctx.translateBy(x: 0, y: size.height); ctx.scaleBy(x: 1, y: -1) }
                layer.render(in: ctx)
                if let img = frameImage, let mtk = Self.findMTKView(content) {
                    // Layer rendering skips Metal content: draw the current frame where it is shown.
                    var vr = mtk.convert(mtk.bounds, to: frameView)
                    if frameView.isFlipped { vr.origin.y = size.height - vr.maxY }
                    let (r, crop) = GameRenderer.viewport(drawableSize: CGSize(width: vr.width * scale, height: vr.height * scale), options: displayOptions)
                    let cropped = img.cropping(to: CGRect(x: 0, y: crop, width: Int(RN_VIDEO_WIDTH), height: Int(RN_VIDEO_HEIGHT) - 2 * crop)) ?? img
                    ctx.saveGState()
                    if frameView.isFlipped { ctx.scaleBy(x: 1, y: -1); ctx.translateBy(x: 0, y: -size.height) } // back to bottom-left
                    ctx.interpolationQuality = .none
                    ctx.draw(cropped, in: CGRect(x: vr.minX + r.minX / scale, y: vr.minY + r.minY / scale, width: r.width / scale, height: r.height / scale))
                    ctx.restoreGState()
                }
                if let img = ctx.makeImage() {
                    let r = NSBitmapImageRep(cgImage: img)
                    try? r.representation(using: .png, properties: [:])?.write(to: url.deletingPathExtension().appendingPathExtension("layers.png"))
                }
            }
        }

        let s = emu.latency.snapshot(audio: emu.audio)
        var info: [String: Any] = [
            "frame": frameIndex, "statusFrame": status.frame, "takeLength": status.takeLength,
            "recording": status.recording, "paused": status.paused,
            "presentedFPS": s.presentedFPS, "emulatedToPresentMs": s.emulatedToPresentMs,
            "sampleToEmulatedMs": s.sampleToEmulatedMs, "audioUnderruns": s.audioUnderruns,
            "audioFillMs": s.audioFillMs, "audioOutputLatencyMs": s.audioOutputLatencyMs,
            "lateTicks": s.lateTicks,
            "flashReduction": flashLevel.label, "flashActive": status.flashActive,
            "libraryROMs": library.roms.count, "libraryRoot": library.paths.root.path,
        ]
        info.merge(keyboardDiagnostics) { a, _ in a }
        if let data = try? JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: url.deletingPathExtension().appendingPathExtension("json"))
        }
    }

    private static func findMTKView(_ v: NSView) -> MTKView? {
        if let m = v as? MTKView { return m }
        for s in v.subviews { if let m = findMTKView(s) { return m } }
        return nil
    }
}
