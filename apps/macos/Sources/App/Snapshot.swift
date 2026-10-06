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
            "lateTicks": s.lateTicks, "emulatedFPS": s.emulatedFPS, "presentCount": s.presentCount,
            "presentHitches": s.presentHitches, "skippedFrames": s.skippedFrames, "presentIntervalMaxMs": s.presentIntervalMaxMs,
            "drawCount": s.drawCount, "drawLate": s.drawLate, "drawGapMaxMs": s.drawGapMaxMs,
            "tickWakeLate": s.tickWakeLate, "tickWakeMaxMs": s.tickWakeMaxMs, "presentLeadMs": s.presentLeadMs,
            "displayGPUMs": s.displayGPUMs, "displayGPUMaxMs": s.displayGPUMaxMs, "crtInfo": s.crtInfo,
            "crtEnabled": CRTSettingsModel.shared.enabled,
            "flashReduction": flashLevel.label, "flashActive": status.flashActive,
            "libraryROMs": library.roms.count, "libraryRoot": library.paths.root.path,
            "practicing": status.practicing, "practiceSlot": status.practiceSlot,
            "practiceFrame": status.practiceFrame, "practiceLength": status.practiceLength,
            "practiceLooping": status.practiceLooping, "practiceLoops": status.practiceLoops,
            "practiceSlots": practiceSlots.filter { $0.hasA }.map { ["slot": $0.index, "hasB": $0.hasB, "length": $0.length, "name": $0.name] },
            "takeCount": status.takeCount, "activeTake": status.activeTake, "unsaved": status.unsaved,
            "projectPath": status.projectPath, "tempSession": isTempSession(status.projectPath),
            "fastForward": status.fastForward, "slow": status.slow.label, "endOfTake": status.endOfTake,
            "integerScale": integerScale, "menuItemAdds": MenuBarCleaner.shared.mainMenuAdds, "showPracticePanel": showPracticePanel,
            "controllerHotkeys": InputCatalog.controllerHotkeys.map { "\($0.0)=\($0.1)" },
        ]
        info["menus"] = NSApp.mainMenu.map { Self.describe($0) } ?? []
        info.merge(keyboardDiagnostics) { a, _ in a }
        if let data = try? JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: url.deletingPathExtension().appendingPathExtension("json"))
        }
    }


    /// `--stats-log <path>`: one JSON line of pacing / latency counters per second (perf smoke
    /// test; much cheaper than a window snapshot, so it does not disturb what it measures).
    /// `frameLog`: also one CSV line per presented frame (LatencyMeter.frameLogHeader), written
    /// off the main thread.
    func startStatsLog(to url: URL, frameLog: URL? = nil) {
        FileManager.default.createFile(atPath: url.path, contents: nil)
        guard let h = try? FileHandle(forWritingTo: url) else { NSLog("ReplayNES: cannot write \(url.path)"); return }
        var frames: FileHandle?
        if let frameLog {
            FileManager.default.createFile(atPath: frameLog.path, contents: Data((LatencyMeter.frameLogHeader + "\n").utf8))
            frames = try? FileHandle(forWritingTo: frameLog)
            frames?.seekToEndOfFile()
            emu.latency.startFrameLog()
        }
        let writer = DispatchQueue(label: "ReplayNES.statsLog", qos: .utility)
        let start = HostClock.now()
        var mainCPU0 = HostClock.threadCPUSeconds()
        let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
            guard let self else { return }
            let s = self.emu.latency.snapshot(audio: self.emu.audio)
            var ru = rusage()
            getrusage(RUSAGE_SELF, &ru)
            let processCPU = Double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + Double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6
            let mainCPU = HostClock.threadCPUSeconds()
            defer { mainCPU0 = mainCPU }
            let row: [String: Any] = [
                "t": HostClock.seconds(HostClock.now() - start), "frame": self.status.frame, "paused": self.status.paused,
                "emulatedFPS": s.emulatedFPS, "presentedFPS": s.presentedFPS, "presentCount": s.presentCount,
                "presentHitches": s.presentHitches, "offCadence": s.offCadence, "skippedFrames": s.skippedFrames,
                "presentIntervalMaxMs": s.presentIntervalMaxMs, "missedRefreshes": s.missedRefreshes,
                "repeatPresents": s.repeatPresents, "backlogDrains": s.backlogDrains,
                "drawCount": s.drawCount, "drawLate": s.drawLate, "drawGapMaxMs": s.drawGapMaxMs,
                "tickWakeLate": s.tickWakeLate, "tickWakeMaxMs": s.tickWakeMaxMs, "lateTicks": s.lateTicks,
                "presentLeadMs": s.presentLeadMs, "inputLeadMs": s.inputLeadMs, "pacing": s.pacing, "refreshHz": s.refreshHz,
                "emulatedToPresentMs": s.emulatedToPresentMs, "sampleToPresentMs": s.sampleToPresentMs,
                "displayGPUMs": s.displayGPUMs, "audioUnderruns": s.audioUnderruns, "audioDropped": s.audioDropped,
                "audioFillMs": s.audioFillMs, "audioRatio": s.audioRatio,
                "emulationCPU": s.emulationCPU, "presentCPU": s.presentCPU, "mainCPU": mainCPU, "processCPU": processCPU,
                "mainCPUWindow": mainCPU - mainCPU0,
                "fullScreen": self.mainWindow?.styleMask.contains(.fullScreen) ?? false,
                "chromeHidden": self.immersive,
            ]
            let rows = frames != nil ? self.emu.latency.takeFrameLog() : []
            writer.async {
                if var line = try? JSONSerialization.data(withJSONObject: row, options: [.sortedKeys]) {
                    line.append(0x0A)
                    h.write(line)
                }
                if let frames, !rows.isEmpty { frames.write(Data((rows.joined(separator: "\n") + "\n").utf8)) }
            }
        }
        RunLoop.main.add(t, forMode: .common)
    }

    /// Menu bar structure for scripted checks: ["title", "  item ⌘k", ...].
    private static func describe(_ menu: NSMenu, depth: Int = 0) -> [String] {
        var out: [String] = []
        for item in menu.items {
            if item.isSeparatorItem || item.isHidden { continue }
            let key = item.keyEquivalent.isEmpty ? "" : " [\(item.keyEquivalentModifierMask.contains(.shift) ? "⇧" : "")\(item.keyEquivalentModifierMask.contains(.option) ? "⌥" : "")⌘\(item.keyEquivalent)]"
            out.append(String(repeating: "  ", count: depth) + item.title + key)
            if let sub = item.submenu, depth < 2 { out += describe(sub, depth: depth + 1) }
        }
        return out
    }
}
