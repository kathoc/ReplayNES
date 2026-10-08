// The Swift side of the shared frontend core (Sources/Core/*.swift over frontend.h): value
// semantics of the copy-on-write wrappers, C struct / string / list conversion and the
// macOS-only pieces (persisted raw values, Finder collation, glyph dictionaries). The logic is
// tested in tests/test_frontend_*.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

final class FrontendBridgeTests: XCTestCase {
    func testWrappersKeepValueSemantics() {
        var a = DisplayCadence()
        for i in 0..<100 { _ = a.refresh(presentation: 10 + Double(i) / 120) }
        var b = a                                   // copy, then diverge (another display)
        for i in 1...240 { _ = b.refresh(presentation: 20 + Double(i) / 60) }
        XCTAssertEqual(b.refreshesPerFrame, 1)
        XCTAssertEqual(a.refreshesPerFrame, 2, "the original is untouched")
        XCTAssertEqual(a.refresh, 1.0 / 120, accuracy: 1e-9)

        var p = PracticeLoop()
        let saved = p
        XCTAssertEqual(p.tick(now: 0, counter: 10, length: 10), .beginHold)
        XCTAssertEqual(p.phase, .holding(since: 0))
        XCTAssertEqual(saved.phase, .playing, "the copy did not change")
        XCTAssertNotEqual(p, saved)

        var r = CallbackRegularity(lateAfter: 1)
        let r0 = r
        r.lateAfter = 2
        r.tick(at: 1)
        XCTAssertEqual(r0.lateAfter, 1)
        XCTAssertEqual(r0.count, 0)
        XCTAssertEqual(r.count, 1)
    }

    func testPacingAndAudioWrappers() {
        XCTAssertEqual(FramePacing.period, Double(RN_FPS_DEN) / Double(RN_FPS_NUM))
        XCTAssertEqual(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 120), 2)
        XCTAssertNil(DisplayCadence.refreshesPerFrame(refresh: 1.0 / 144))
        XCTAssertEqual(BacklogDrain.policy(variableRefresh: false, direct: true), BacklogDrain.fast)
        var drc = AudioRateControl(targetFill: 1400)
        XCTAssertNil(drc.smoothedFill)
        drc.setFrameRate(60)
        XCTAssertEqual(drc.update(fill: 1400), drc.base, accuracy: 1e-12)
        XCTAssertEqual(drc.smoothedFill, 1400)
        var res = AudioResampler()
        var out: [Int16] = [7]
        let input = [Int16](repeating: 1000, count: 800)
        input.withUnsafeBufferPointer { res.process($0, ratio: 1, into: &out) }
        XCTAssertEqual(out.count, 1 + 799, "appended, one sample of look-ahead held back")
        XCTAssertEqual(out[0], 7)
        var f = FramePacing()
        f.present(frame: 1, at: 1)
        XCTAssertEqual(f.lastPresent?.frame, 1)
    }

    func testInputCatalogBridge() {
        XCTAssertEqual(InputCatalog.gameActions.count, 20)
        XCTAssertEqual(InputCatalog.hotkeyActions.first?.id, "hk.rewind")
        XCTAssertEqual(InputCatalog.gameActions.first?.label, "↑ Up")
        XCTAssertEqual(InputAction.Group.hotkey.title, "Hotkeys")
        XCTAssertEqual(InputCatalog.controllerLayoutVersion, 5)
        let c = InputCatalog.parse(InputCatalog.defaultConfigJSON())!
        XCTAssertEqual(c.turboPeriod, 4)
        XCTAssertEqual(c.socd, "neutral")
        XCTAssertEqual(c.bindings.count, InputCatalog.defaultBindings.count)
        XCTAssertNil(InputCatalog.parse("[]"))
        XCTAssertEqual(InputCatalog.pausedStepDirections(c)["gc0:dpad.left"], -1)
        XCTAssertTrue(InputCatalog.faceLayoutMigration(c).bind.isEmpty)
        let reset = InputCatalog.controllerResetPlan(c, slot: 1)
        XCTAssertTrue(reset.bind.allSatisfy { $0.0.hasPrefix("gc1:") })
        let pro = ControllerInfo(slot: 0, name: "Pro", productCategory: "Switch Pro Controller", family: .nintendo)
        XCTAssertEqual(InputCatalog.displayName("gc0:face.east", controllers: [pro]), "Pad 1 Right Button(A)")
        XCTAssertEqual(InputCatalog.displayName("gc1:face.east", controllers: [pro]), "Pad 2 Right Button")
        XCTAssertEqual(InputCatalog.displayName("kb:126"), "Key ↑")
        // GameController glyph dictionaries (nil = not reported).
        let m = GCFaceMapping.positions(productCategory: "Switch Pro Controller",
                                        symbols: ["buttonA": "b.circle", "buttonB": "a.circle", "buttonX": "y.circle", "buttonY": "x.circle"])
        XCTAssertEqual(m, ["buttonA": .south, "buttonB": .east, "buttonX": .west, "buttonY": .north])
        let partial = GCFaceMapping.positions(productCategory: "Switch Pro Controller", symbols: ["buttonA": nil, "buttonB": "a.circle"])
        XCTAssertEqual(partial["buttonA"], .east)
        let legacy = InputCatalog.legacyFaceTranslation(InputCatalog.Config(bindings: [(input: "gc0:buttonA", action: "p1.a")],
                                                                            turboPeriod: 2, turboDuty: 1, socd: "neutral",
                                                                            analogThreshold: 0.5),
                                                        slot: 0, positions: ["buttonA": .east])
        XCTAssertEqual(legacy.bind.first?.0, "gc0:face.east")
        XCTAssertEqual(ControllerFamily.from(productCategory: "Steam Deck"), .steamDeck, "its own diagram layout")
        XCTAssertEqual(ControllerFamily.playStation.label("face.east"), "○")
        let layout = ControllerDiagramLayout(family: .playStation)
        XCTAssertTrue(layout.decor.contains { $0.kind == .pad }, "the touchpad")
        XCTAssertEqual(layout.element("leftThumb")?.kind, .stickClick)
        XCTAssertEqual(layout.element("dpad.up")?.group, "dpad")
        XCTAssertEqual(ControllerAssignments.group("dpad", slot: 0, config: c), .movement("Move"))
        XCTAssertEqual(ControllerAssignments.badge(element: "home", slot: 0, config: c), nil)
        XCTAssertEqual(ControllerAssignments.title(element: "face.south", family: .xbox, labels: ["face.south": "Ⓐ"]),
                       "Bottom Button (Ⓐ)")
    }

    func testResumeRecordConversion() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("rn-bridge-\(UUID().uuidString)")
        let paths = SessionPaths(root: dir)
        try paths.ensure()
        defer { try? FileManager.default.removeItem(at: dir) }
        let r = ResumeRecord(projectPath: "/a/b.nesrec", isTemp: false, romPath: "/r.nes", romSHA256: "ff", frame: 7, atTakeEnd: false,
                             mode: .record, practiceSlot: -1, hasContent: false, updated: Date(timeIntervalSince1970: 1_700_000_000))
        try ResumeStore.write(r, to: paths.resumeFile)
        XCTAssertEqual(try ResumeStore.read(paths.resumeFile), r)
        XCTAssertEqual(SessionResume.targetFrame(r, takeLength: 3), 3)
        XCTAssertTrue(paths.isTempProject(paths.tempProject.path + "/"))
        try Data("x".utf8).write(to: paths.resumeFile)
        XCTAssertThrowsError(try ResumeStore.read(paths.resumeFile))
    }

    func testTimelineAndExportBridges() {
        let g = TimelineGeometry(width: 1000, length: 1000)
        XCTAssertEqual(TimelineEditing.hitTest(x: 102, ranges: [TimelineRange(slot: 3, a: 100, b: nil)], in: g), .handle(slot: 3, .a))
        XCTAssertEqual(TimelineEditing.markB(at: 5, existing: nil), .invalid("Set A of this section first"))
        XCTAssertTrue(TimelineEditing.drag(.b, of: (200, 500), toX: 10, in: g) == (200, 201))
        XCTAssertEqual(ThumbnailGrid.targets(length: 1_000, step: 300), [60, 300, 600, 900])
        XCTAssertEqual(ThumbnailGrid.tiles(length: 420, step: 300, tileWidth: 10).map(\.picture), [60, 300])
        var s = ExportSettings()
        s.cropTop = 0; s.cropBottom = 0
        let geo = ExportGeometry(s)
        XCTAssertEqual([geo.canvasWidth, geo.dstWidth, geo.dstX], [1280, 1024, 128])
        XCTAssertEqual(Array(geo.columnMap()[0..<8]), [0, 0, 0, 0, 1, 1, 1, 1])
        XCTAssertEqual(geo.rowMap().count, geo.dstHeight)
        XCTAssertEqual(ExportSettings.SizePreset.all.map(\.label),
                       ["Native ×1", "Native ×2", "Native ×3", "Native ×4", "1280×960", "1920×1440", "1920×1080"])
        s.endFrame = 1
        s.startFrame = 5
        XCTAssertThrowsError(try s.validate())
        // Raw values are persisted in user defaults: keep them stable.
        XCTAssertEqual(StreamOutputSize.allCases.map(\.rawValue), ["x1", "x2", "x3", "x4", "w1280", "w1920"])
        XCTAssertEqual(StreamOutputLayout(size: .w1920, par87: true).picture, CGRect(x: 82, y: 0, width: 1755, height: 1440))
        XCTAssertEqual(StreamOutputSize.x4.label(par87: false), "4× 1024×960")
        XCTAssertEqual(FlashLevel.standard.label, "Standard")
        XCTAssertEqual(SlowRate.quarter.toggled, .normal)
        XCTAssertEqual(SlowRate.normal.label, "Normal")
        XCTAssertEqual(UILanguage.choose(systemPreferred: ["ja-JP"]), "ja")
        XCTAssertEqual(UILanguage.choose(systemPreferred: []), "en")
        XCTAssertEqual(LibraryScanner.sanitize("a/b"), "a_b")
        let tail = [Int16](repeating: 100, count: 4).withUnsafeBufferPointer { AudioFade.tail(from: $0, repeats: 2) }
        XCTAssertEqual(tail.count, 8)
        XCTAssertEqual(tail.last, 0)
    }

    /// Regression (0.3.0): the global language list was read through
    /// UserDefaults(suiteName: globalDomain), which is nil, so the UI was always pinned to English.
    func testSystemLanguagesReadsTheGlobalDomain() {
        let global = CFPreferencesCopyValue("AppleLanguages" as CFString, kCFPreferencesAnyApplication,
                                            kCFPreferencesCurrentUser, kCFPreferencesAnyHost) as? [String] ?? []
        XCTAssertEqual(UILanguage.systemLanguages(), global)
        XCTAssertFalse(UILanguage.systemLanguages().isEmpty, "a logged-in user always has a language list")
    }
}
