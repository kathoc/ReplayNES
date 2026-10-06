// Debug/smoke-test helper: `--snapshot <png>` writes a capture of the main window (with the
// current emulated frame composited into the Metal viewport, since layer-backed Metal content
// is not captured by cacheDisplay) plus a JSON with runtime statistics.
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import MetalKit

extension AppModel {
    /// Captures the window. The current emulated frame is first handed to SwiftUI
    /// (SnapshotFrameView, under the badges / OSD) because layer-backed Metal content is not
    /// captured by cacheDisplay; the capture happens after that view has been laid out.
    func writeSnapshot(to url: URL, completion: (() -> Void)? = nil) {
        guard mainWindow != nil else { completion?(); return }
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
        snapshotFrame = frameImage
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            guard let self else { completion?(); return }
            self.captureWindow(to: url, frameIndex: frameIndex)
            self.snapshotFrame = nil
            completion?()
        }
    }

    private func captureWindow(to url: URL, frameIndex: UInt64) {
        guard let window = mainWindow, let content = window.contentView else { return }
        let scale = window.backingScaleFactor
        if let rep = content.bitmapImageRepForCachingDisplay(in: content.bounds) {
            content.cacheDisplay(in: content.bounds, to: rep)
            if let png = rep.representation(using: .png, properties: [:]) { try? png.write(to: url) }
        }
        // Second capture through the layer tree of the whole window frame (includes toolbar).
        if let frameView = content.superview, let layer = frameView.layer {
            let size = frameView.bounds.size
            if let ctx = CGContext(data: nil, width: Int(size.width * scale), height: Int(size.height * scale), bitsPerComponent: 8,
                                   bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) {
                ctx.scaleBy(x: scale, y: scale)
                if frameView.isFlipped { ctx.translateBy(x: 0, y: size.height); ctx.scaleBy(x: 1, y: -1) }
                layer.render(in: ctx)
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
            "practicing": status.practicing, "practiceSlot": status.practiceSlot,
            "practiceFrame": status.practiceFrame, "practiceLength": status.practiceLength,
            "practiceLooping": status.practiceLooping, "practiceLoops": status.practiceLoops,
            "practiceSlots": practiceSlots.filter { $0.hasA }.map { ["slot": $0.index, "hasB": $0.hasB, "length": $0.length, "name": $0.name] },
            "takeCount": status.takeCount, "activeTake": status.activeTake, "unsaved": status.unsaved,
            "fastForward": status.fastForward, "slow": status.slow.label, "endOfTake": status.endOfTake,
            "integerScale": integerScale, "showPracticePanel": showPracticePanel,
            "controllerHotkeys": InputCatalog.controllerHotkeys.map { "\($0.0)=\($0.1)" },
        ]
        info.merge(keyboardDiagnostics) { a, _ in a }
        if let data = try? JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: url.deletingPathExtension().appendingPathExtension("json"))
        }
    }

}
