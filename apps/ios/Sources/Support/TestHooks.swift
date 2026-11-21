// Launch arguments for simulator checks and measurements (Xcode scheme or `simctl launch`):
//   -autoplayTestROM YES      write the engine's test ROM into ROM/ and play it (temporary session)
//   -autoplayROM <name.nes>   play that file of ROM/
//   -pauseAfter <s>           pause after s seconds (the seek bar)
//   -resumeAfter <s>          resume after s seconds (through the resume countdown when it is on)
//   -continueLast YES         press the library's Continue card at 1 s (resume check)
//   -openMenu <page>          open the Quick Menu at a page (top, retry, practice, display, ...) after 2 s
//   -controllerKind automatic|pad|stick, -controllerFamily automatic|nintendo|xbox|playStation: pad 1's, after 4 s
//   -assumeCoverPad YES       treat the pads as a FlipPad (the upper-half layout in portrait); -testDialog YES: a dialog
//   -assign <id>=<action>     after 6 s, assign as the action picker does (gc0:face.east=p1.b3.turbo)
//   -openAssign <id>          open the action picker of a controller button (gc0:face.east) after 2.5 s
//   -debugUnlockAll NO        free mode (the prototype switch off)
//   -pacingProbe <s>          after s seconds of play write Library/Caches/pacing.json (PacingProbe)
//   -holdTouch <button>       press an on-screen button at 2 s for 0.3 s (e.g. l2 = rewind) to check touch input;
//                             -holdTouchAt <s>: at s seconds instead (an arcade board's boot takes longer)
//   -touchScript b@s:d,...    on-screen buttons pressed at s seconds for d seconds each (select@12:0.3,start@13:0.3)
//   -setResumeCountdown NO    store Settings › Controls › Controls Details › Resume Countdown before the start:
//                             YES = 3 seconds (the numerals), NO = none (the default is 1 second, the ring alone)
//   -setDefaults k:v,k:v      store settings in the app's domain before the start (YES / NO, numbers, text)
//   -logTouch YES             log every on-screen press / release edge ("ReplayNES touch: a+ b-")
//   -logTouchEvents YES       log every UITouch the on-screen gamepad receives
//   -orientation landscape    rotate the window (screenshots of the landscape layout)
//   -rotateCycle <s>          rotate portrait → landscape right → portrait → landscape left ... every s seconds
//   -displayStress <s>        every s seconds one display change while playing (scale, CRT lines, CRT off / on,
//                             the menu, pause / resume, rotation), each logged to Library/Caches/display-log.txt;
//                             -displayStressSteps <n>: stop after n steps and restore the settings
//   -displayFault <list>      display path fault injection (DisplayFaults: crtStale, noPresent, gpuHang, linkStall,
//                             linkRestart, skipPresents, adaptiveFlip); -displayStats <s>; -displayProbe YES
//   -resetLibraryForTests YES empty Documents/ROM and Documents/Projects and forget the temporary session
//   -importTestBatch YES      at 1 s, open 7 files at once like a multi-item Share › ReplayNES: 5 distinct
//                             test ROMs (one name twice, from two folders), a duplicate, a non-ROM
//   -sessionCycle YES         with -autoplayTestROM: close, continue, switch to the Test Cartridge, save,
//                             background, close; writes Library/Caches/session-cycle.json with the longest
//                             main-thread stall (no main-thread waits on the emulation thread)
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import UIKit
import QuartzCore

enum TestHooks {
    /// Before the app model starts.
    static func prepare() {
        // Stored in the app's domain (a plain -resumeCountdown argument would pin the setting:
        // the Controls page's switch could not change it).
        if let on = UserDefaults.standard.object(forKey: "setResumeCountdown") as? String {
            UserDefaults.standard.set(on == "YES" || on == "1" ? 3 : 0, forKey: "countdownSeconds")
        }
        // -setDefaults key:value,key:value - stored in the app's domain (YES / NO, integers, numbers, text).
        if let list = UserDefaults.standard.string(forKey: "setDefaults") {
            for pair in list.split(separator: ",") {
                let kv = pair.split(separator: ":", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
                guard kv.count == 2, !kv[0].isEmpty else { continue }
                let (k, v) = (kv[0], kv[1])
                let value: Any = v == "YES" ? true : v == "NO" ? false : Int(v).map { $0 as Any } ?? Double(v).map { $0 as Any } ?? v
                UserDefaults.standard.set(value, forKey: k)
            }
        }
        guard UserDefaults.standard.bool(forKey: "resetLibraryForTests") else { return }
        let docs = LibraryPaths.documents
        for dir in [docs.roms, docs.projects] { try? FileManager.default.removeItem(at: dir) }
    }

    static func run(_ m: AppModel) {
        let d = UserDefaults.standard
        if d.bool(forKey: "resetLibraryForTests") { m.discardTemporarySession() }
        if d.bool(forKey: "importTestBatch") {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                do { m.open(urls: try importTestBatch()) } catch { NSLog("ReplayNES: importTestBatch: \(error)") }
            }
        }
        var rom: URL?
        if d.bool(forKey: "autoplayTestROM") {
            m.library.ensureFolders()
            let url = m.library.paths.roms.appendingPathComponent("ReplayNES Test.nes")
            if !FileManager.default.fileExists(atPath: url.path) { try? Engine.writeTestROM(to: url) }
            rom = url
        } else if let name = d.string(forKey: "autoplayROM") {
            rom = m.library.paths.roms.appendingPathComponent(name)
        }
        if let rom {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                m.discardTemporarySession()
                let entry = LibraryROM(url: rom, name: rom.deletingPathExtension().lastPathComponent, relativePath: rom.lastPathComponent,
                                       size: 0, modified: Date(), sha256: nil, game: nil)
                m.playFromLibrary(entry, autoplay: true)
            }
        }
        if d.bool(forKey: "continueLast") { DispatchQueue.main.asyncAfter(deadline: .now() + 1) { m.continueLast() } }
        let pause = d.double(forKey: "pauseAfter")
        if pause > 0 { DispatchQueue.main.asyncAfter(deadline: .now() + pause) { m.setPaused(true) } }
        let resume = d.double(forKey: "resumeAfter")
        if resume > 0 { DispatchQueue.main.asyncAfter(deadline: .now() + resume) { m.setPaused(false) } }
        if let page = d.string(forKey: "openMenu"), let p = QMPage(rawValue: page) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { m.quickMenu.open(at: p) }
        }
        if let k = d.string(forKey: "controllerKind").flatMap(InputManager.KindChoice.init(rawValue:)) {   // pad 1's kind
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) { m.input.setKindChoice(k, slot: 0); NSLog("ReplayNES test hook: kind %@", k.rawValue) }
        }
        if let f = d.string(forKey: "controllerFamily") {   // pad 1's button layout (automatic = from its name)
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) { m.input.setFamilyChoice(ControllerFamily(rawValue: f), slot: 0); NSLog("ReplayNES test hook: family %@", f) }
        }
        if let spec = d.string(forKey: "assign"), let eq = spec.firstIndex(of: "=") {   // "gc0:face.east=p1.b3.turbo"
            let id = String(spec[..<eq]), action = String(spec[spec.index(after: eq)...])
            DispatchQueue.main.asyncAfter(deadline: .now() + 6) {
                m.input.setAssignment(id, action: action)
                NSLog("ReplayNES test hook: assigned %@ = %@", id, action)
            }
        }
        if d.bool(forKey: "testDialog") {   // a two-button dialog after 4 s (TopDialog with -assumeCoverPad YES)
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
                m.alert = AppAlert(title: "Reset this project?", message: "Test dialog", actions: [
                    AppAlert.Action(title: String(localized: "Cancel"), role: .cancel),
                    AppAlert.Action(title: "Reset", role: .destructive)])
            }
        }
        if let id = d.string(forKey: "openAssign") {   // the action picker of a controller button ("gc0:face.east")
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { m.quickMenu.open(at: .controller); m.quickMenu.openAssign(id) }
        }
        let probe = d.double(forKey: "pacingProbe")
        if probe > 0 { PacingProbe.shared.start(after: probe) }
        if let b = d.string(forKey: "holdTouch"), let button = TouchButton(rawValue: b) {
            let at = d.double(forKey: "holdTouchAt") > 0 ? d.double(forKey: "holdTouchAt") : 2
            DispatchQueue.main.asyncAfter(deadline: .now() + at) { m.touchButtons(pressed: [button], released: []) }
            DispatchQueue.main.asyncAfter(deadline: .now() + at + 0.3) { m.touchButtons(pressed: [], released: [button]) }
        }
        // -touchScript b@s:d,... : press button b at s seconds for d seconds (e.g. select@12:0.3,start@13:0.3,turboB@16:2).
        for step in (d.string(forKey: "touchScript") ?? "").split(separator: ",") {
            let p = step.split(separator: "@"), t = p.count == 2 ? p[1].split(separator: ":") : []
            guard p.count == 2, t.count == 2, let b = TouchButton(rawValue: String(p[0])), let at = Double(t[0]), let len = Double(t[1]) else { continue }
            DispatchQueue.main.asyncAfter(deadline: .now() + at) { m.touchButtons(pressed: [b], released: []) }
            DispatchQueue.main.asyncAfter(deadline: .now() + at + len) { m.touchButtons(pressed: [], released: [b]) }
        }
        if d.string(forKey: "orientation") == "landscape" {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { rotate(to: .landscapeRight) }
        }
        if d.bool(forKey: "sessionCycle") { SessionCycle.run(m) }
        let cycle = d.double(forKey: "rotateCycle")
        if cycle > 0 { rotateCycle(every: cycle, step: 0) }
        let stress = d.double(forKey: "displayStress")
        if stress > 0 {
            let crt = CRTSettingsModel.shared
            let saved = (m.integerScale, crt.reducedLines, crt.enabled)
            let steps = d.integer(forKey: "displayStressSteps")
            displayStress(m, every: stress, step: 0, steps: steps > 0 ? steps : .max) {
                // The settings as they were (the stress run must not leave them changed).
                m.integerScale = saved.0; crt.reducedLines = saved.1; crt.enabled = saved.2
                DisplayLog.write("stress done: settings restored")
            }
        }
    }

    /// -displayStress <s>: display changes back to back (DisplayMonitor logs any stall).
    private static func displayStress(_ m: AppModel, every s: Double, step: Int, steps: Int, done: @escaping () -> Void) {
        DispatchQueue.main.asyncAfter(deadline: .now() + s) {
            if step >= steps { done(); return }
            let crt = CRTSettingsModel.shared
            let actions: [(String, () -> Void)] = [
                ("integer scale", { m.integerScale.toggle() }),
                ("CRT lines", { crt.reducedLines.toggle() }),
                ("CRT off", { crt.enabled = false }),
                ("CRT on", { crt.enabled = true }),
                ("menu open", { m.quickMenu.open(at: .display) }),
                ("menu close", { m.quickMenu.close(resume: true) }),
                ("integer scale", { m.integerScale.toggle() }),
                ("pause", { m.setPaused(true) }),
                ("resume", { m.setPaused(false) }),
                ("landscape", { rotate(to: .landscapeRight) }),
                ("CRT lines", { crt.reducedLines.toggle() }),
                ("portrait", { rotate(to: .portrait) }),
            ]
            let (name, f) = actions[step % actions.count]
            DisplayLog.write("stress step \(step + 1): \(name)")
            f()
            displayStress(m, every: s, step: step + 1, steps: steps, done: done)
        }
    }

    /// Files outside the library as another app would hand them over.
    static func importTestBatch() throws -> [URL] {
        let fm = FileManager.default
        let dir = fm.temporaryDirectory.appendingPathComponent("ImportBatch-\(UUID().uuidString)", isDirectory: true)
        let other = dir.appendingPathComponent("Other", isDirectory: true)
        try fm.createDirectory(at: other, withIntermediateDirectories: true)
        let base = dir.appendingPathComponent("base.tmp")
        try Engine.writeTestROM(to: base)
        let rom = try Data(contentsOf: base)
        try fm.removeItem(at: base)
        var urls: [URL] = []
        let names = ["Alpha", "Bravo", "Charlie", "Delta"]
        for (i, name) in names.enumerated() {
            var d = rom
            d[16 + 0x2000 + i] &+= UInt8(1 + i)
            let u = dir.appendingPathComponent("\(name).nes")
            try d.write(to: u)
            urls.append(u)
        }
        // Same name as Alpha, other contents (-> "Alpha 2.nes").
        var e = rom
        e[16 + 0x2000 + 9] &+= 7
        let sameName = other.appendingPathComponent("Alpha.nes")
        try e.write(to: sameName)
        urls.append(sameName)
        // A duplicate of Bravo under another name, and a file that is no ROM.
        let dup = other.appendingPathComponent("Bravo (copy).nes")
        try fm.copyItem(at: urls[1], to: dup)
        urls.append(dup)
        let junk = dir.appendingPathComponent("readme.nes")
        try Data("not a ROM".utf8).write(to: junk)
        urls.append(junk)
        return urls
    }

    private static func rotate(to o: UIInterfaceOrientationMask) {
        let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        scene?.requestGeometryUpdate(.iOS(interfaceOrientations: o)) { NSLog("ReplayNES: rotate: \($0)") }
    }

    /// -rotateCycle <s>: portrait, landscape right, portrait, landscape left, ... every s seconds
    /// (rotation checks: the layout must be the same after any number of turns).
    private static func rotateCycle(every s: Double, step: Int) {
        DispatchQueue.main.asyncAfter(deadline: .now() + s) {
            let seq: [UIInterfaceOrientationMask] = [.landscapeRight, .portrait, .landscapeLeft, .portrait]
            let o = seq[step % seq.count]
            NSLog("ReplayNES: rotateCycle step \(step + 1): \(o == .portrait ? "portrait" : o == .landscapeLeft ? "landscapeLeft" : "landscapeRight")")
            rotate(to: o)
            rotateCycle(every: s, step: step + 1)
        }
    }
}

/// -sessionCycle: session changes back to back while a 5 ms main-thread timer measures the longest
/// gap between its fires (a blocking wait on the main thread shows up as a long stall).
enum SessionCycle {
    static func run(_ m: AppModel) {
        var maxGap = 0.0, windowGap = 0.0
        var last = CACurrentMediaTime()
        let timer = Timer(timeInterval: 0.005, repeats: true) { _ in
            let t = CACurrentMediaTime()
            maxGap = max(maxGap, t - last)
            windowGap = max(windowGap, t - last)
            last = t
        }
        RunLoop.main.add(timer, forMode: .common)
        var log: [String] = []
        func note(_ s: String) {
            log.append(String(format: "%.2f %@ hasSession=%d stallMs=%.0f", CACurrentMediaTime(), s, m.status.hasSession ? 1 : 0, windowGap * 1000))
            windowGap = 0
        }
        let cart = m.library.paths.roms.appendingPathComponent("ReplayNES Test Cartridge.nes")
        let steps: [(Double, String, () -> Void)] = [
            (3.0, "close", { m.closeProject() }),
            (4.5, "continue", { m.continueLast() }),
            (6.5, "switch", {
                try? Engine.writeTestCartridge(to: cart)
                let e = LibraryROM(url: cart, name: "ReplayNES Test Cartridge", relativePath: cart.lastPathComponent,
                                   size: 0, modified: Date(), sha256: nil, game: nil)
                m.playFromLibrary(e, autoplay: true)
            }),
            (9.0, "save", { m.save() }),
            (11.0, "background", { m.enterBackground() }),
            (12.0, "close", { m.closeProject() }),
        ]
        for (t, name, f) in steps {
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { note(name + " begin"); f(); note(name + " called") }
            DispatchQueue.main.asyncAfter(deadline: .now() + t + 0.2) { note(name + " +0.2s") }
            DispatchQueue.main.asyncAfter(deadline: .now() + t + 0.9) { note(name + " +0.9s") }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 14) {
            timer.invalidate()
            let out: [String: Any] = ["maxMainStallMs": (maxGap * 1000).rounded(), "log": log, "hasSessionAtEnd": m.status.hasSession]
            let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("session-cycle.json")
            if let data = try? JSONSerialization.data(withJSONObject: out, options: [.prettyPrinted, .sortedKeys]) {
                try? data.write(to: url)
                NSLog("ReplayNES session cycle: \(String(decoding: data, as: UTF8.self))")
            }
        }
    }
}

/// Basic pacing check: the latency meter's counters over a window of play, written as JSON.
final class PacingProbe {
    static let shared = PacingProbe()
    private(set) var active = false
    private var first: LatencyMeter.Snapshot?
    private var startTime = 0.0
    private var duration = 0.0

    func start(after seconds: Double) {
        active = true
        duration = seconds
        startTime = ProcessInfo.processInfo.systemUptime + 2   // let the game boot first
    }

    /// Main thread, 4x a second while active.
    func observe(_ s: LatencyMeter.Snapshot) {
        guard active else { return }
        let t = ProcessInfo.processInfo.systemUptime
        if t < startTime { return }
        if first == nil { first = s; return }
        guard t >= startTime + duration, let a = first else { return }
        active = false
        let out: [String: Any] = [
            "seconds": duration,
            "pacing": s.pacing,
            "refreshHz": s.refreshHz,
            "emulatedFPS": s.emulatedFPS,
            "presentedFPS": s.presentedFPS,
            "onScreenFPS": s.onScreenFPS,
            "presents": s.presentCount - a.presentCount,
            "presentHitches": s.presentHitches - a.presentHitches,
            "offCadence": s.offCadence - a.offCadence,
            "skippedFrames": s.skippedFrames - a.skippedFrames,
            "droppedFrames": s.droppedFrames - a.droppedFrames,
            "missedRefreshes": s.missedRefreshes - a.missedRefreshes,
            "repeatPresents": s.repeatPresents - a.repeatPresents,
            "foreignCallbacks": s.foreignCallbacks - a.foreignCallbacks,
            "inputLeadMs": s.inputLeadMs,
            "sampleToPresentMs": s.sampleToPresentMs,
            "stepMs": s.stepMs,
            "audioUnderruns": s.audioUnderruns - a.audioUnderruns,
            "audioFillMs": s.audioFillMs,
            "audioRatio": s.audioRatio,
            "audioSession": AppModel.shared.audioSession.describe,
            "audioRunning": AppModel.shared.emu.audio.running,
            "audioPulledSamples": AppModel.shared.emu.audio.stats().pulled_samples,
            "layerPixels": s.layerPixels,
        ]
        let url = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("pacing.json")
        if let data = try? JSONSerialization.data(withJSONObject: out, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: url)
            NSLog("ReplayNES pacing: \(String(decoding: data, as: UTF8.self))")
        }
    }
}
