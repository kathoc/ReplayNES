// The items of every Quick Menu page (structure: Core/QuickMenuModel.swift). Labels are 1–2 words;
// the only explanation is the one-line `detail` of the focused item (docs/design/UI_REDESIGN.md).
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI

struct QMAction {
    let label: String
    let run: () -> Void
}

struct QMItem: Identifiable {
    enum Value {
        case none
        case toggle(Bool)
        case choice([String], Int)
        case slider(Double, ClosedRange<Double>, String)   // value, range, shown text
        case text(String)
        /// Button glyphs with their verbs ("Ⓑ Confirm  Ⓐ Back"), switched with ← / →.
        case buttons([(glyph: String, verb: String)])
    }
    let id: String
    var icon: String
    var title: String
    var detail: String
    var value: Value = .none
    var enabled = true
    var subtitle: String?
    var image: CGImage?
    var accent: Color?
    var badge: String?
    /// Opens a sub-page.
    var page: QMPage?
    var confirm: (() -> Void)?
    /// Rows: left / right (-1 / +1) change the value.
    var adjust: ((Int) -> Void)?
    /// Click on a choice segment / a slider position (0...1).
    var setChoice: ((Int) -> Void)?
    var setFraction: ((Double) -> Void)?
    var x: QMAction?
    var y: QMAction?
    /// Hint-bar verb of confirm.
    var verb: String?
}

struct QMContent {
    var items: [QMItem]
    /// Paged pages: number of list pages (items already cut to the current one).
    var pageCount = 1
    /// Line under the breadcrumb (e.g. the controller shown, "1P · 1/2").
    var caption: String?
    /// Page-level contextual actions (no item focused / for the whole page).
    var x: QMAction?
    var y: QMAction?
}

enum QuickMenuPages {
    static func content(_ p: QMPage, model m: AppModel, menu: QuickMenuController) -> QMContent {
        switch p {
        case .top: return QMContent(items: top(m, menu))
        case .retry: return QMContent(items: retry(m, menu))
        case .takes: return takes(m, menu)
        case .bookmarks: return bookmarks(m, menu)
        case .practice: return QMContent(items: practice(m, menu))
        case .share: return QMContent(items: share(m, menu))
        case .stream: return QMContent(items: stream())
        case .display: return QMContent(items: display(m))
        case .crtDetail:
            return QMContent(items: crtDetail(), y: QMAction(label: String(localized: "Defaults")) { CRTSettingsModel.shared.resetToNestermDefaults() })
        case .controls: return QMContent(items: controls(m))
        case .controller: return controller(m, menu)
        case .assign: return assign(m, menu)
        case .bindings: return bindings(m, menu)
        case .controlsDetail: return QMContent(items: controlsDetail(m))
        case .sound: return QMContent(items: sound(m))
        case .system: return QMContent(items: system(m))
        case .updates: return QMContent(items: updates())
        case .about: return QMContent(items: about())
        case .game: return QMContent(items: game(m, menu))
        case .reset: return QMContent(items: reset(m, menu))
        }
    }

    // MARK: top level

    private static func top(_ m: AppModel, _ menu: QuickMenuController) -> [QMItem] {
        let has = m.status.hasSession
        let items = [
            QMItem(id: "resume", icon: "play.fill", title: String(localized: "Resume"),
                   detail: String(localized: "Close the menu and keep playing"), enabled: has,
                   confirm: { menu.close(resume: true) }),
            QMItem(id: "retry", icon: "clock.arrow.circlepath", title: String(localized: "Retry"),
                   detail: String(localized: "Record again from here or go back to an earlier attempt"), enabled: has, page: .retry),
            QMItem(id: "practice", icon: "target", title: String(localized: "Practice"),
                   detail: String(localized: "Repeat a section from A to B (nothing is recorded)"), enabled: has, page: .practice),
            QMItem(id: "share", icon: "square.and.arrow.up", title: String(localized: "Share"),
                   detail: String(localized: "Export a video or stream the picture"), page: .share),
            QMItem(id: "settings", icon: "gearshape.fill", title: String(localized: "Settings"),
                   detail: String(localized: "Display, controls, sound and system"), page: .display),
            QMItem(id: "game", icon: "gamecontroller.fill", title: String(localized: "Game"),
                   detail: String(localized: "Choose a game, save, reset"), page: .game),
        ]
        assert(items.map(\.id) == QMPage.topTileIDs, "the shared core's Quick Menu tiles")
        return items
    }

    // MARK: Retry

    private static func retry(_ m: AppModel, _ menu: QuickMenuController) -> [QMItem] {
        let st = m.status
        let replay = !st.recording && !st.practicing
        return [
            QMItem(id: "rerecord", icon: "record.circle", title: String(localized: "Record from Here"),
                   detail: String(localized: "Continue recording from this point as a new take (the old one is kept)"),
                   confirm: { menu.close(resume: false); m.rerecordHere() }),
            QMItem(id: "undo", icon: "arrow.uturn.backward", title: String(localized: "Previous Attempt"),
                   detail: String(localized: "Go back to the take before your last retry"),
                   enabled: st.undoDepth > 0 && !st.practicing, confirm: { m.undoTake() }),
            QMItem(id: "watch", icon: replay ? "record.circle.fill" : "play.rectangle", title: replay ? String(localized: "Back to Recording") : String(localized: "Watch Replay"),
                   detail: replay ? String(localized: "Return to record mode and continue from here")
                                  : String(localized: "Play the recorded take without recording"),
                   enabled: st.takeLength > 0 && !st.practicing,
                   confirm: { menu.close(resume: false); m.toggleRecord() }),
            QMItem(id: "takes", icon: "square.stack.3d.up", title: String(localized: "Takes"),
                   detail: String(localized: "Every take of this project; switch to another one"),
                   enabled: !st.practicing, subtitle: "\(st.takeCount)", page: .takes),
            QMItem(id: "bookmarks", icon: "bookmark", title: String(localized: "Bookmarks"),
                   detail: String(localized: "Jump to a marked point, or mark this one"),
                   enabled: !st.practicing, subtitle: "\(m.bookmarks.count)", page: .bookmarks),
        ]
    }

    private static func takes(_ m: AppModel, _ menu: QuickMenuController) -> QMContent {
        // The active take first, then the newest.
        let all = m.takes.sorted { a, b in
            if a.isActive != b.isActive { return a.isActive }
            return a.createdSeq > b.createdSeq
        }
        let per = QMPage.takes.capacity
        let pages = QuickMenuNav.pageCount(items: all.count, perPage: per)
        let range = QuickMenuNav.pageRange(items: all.count, perPage: per, page: menu.listPageIndex(.takes))
        let items = all[range].map { t in
            QMItem(id: "take\(t.id)", icon: "square.stack.3d.up", title: String(localized: "Take #\(t.id)"),
                   detail: t.parentID == 0 ? String(localized: "First take")
                                           : String(localized: "Branched from #\(t.parentID) at \(Engine.timecode(forFrame: t.branchFrame))"),
                   subtitle: Engine.timecode(forFrame: t.length),
                   badge: t.isActive ? String(localized: "Playing") : nil,
                   confirm: t.isActive ? { menu.close(resume: false) } : { m.activateTake(t.id); menu.close(resume: false) },
                   verb: String(localized: "Switch"))
        }
        return QMContent(items: items, pageCount: pages, caption: pages > 1 ? "\(menu.listPageIndex(.takes) + 1) / \(pages)" : nil)
    }

    private static func bookmarks(_ m: AppModel, _ menu: QuickMenuController) -> QMContent {
        var all: [QMItem] = [
            QMItem(id: "addBookmark", icon: "plus", title: String(localized: "Add Here"),
                   detail: String(localized: "Mark the current position"),
                   subtitle: Engine.timecode(forFrame: m.status.frame), confirm: { m.addBookmark() }, verb: String(localized: "Add")),
        ]
        for b in m.bookmarks.reversed() {
            all.append(QMItem(id: "bm\(b.id)", icon: "bookmark.fill", title: b.name,
                              detail: b.onActiveTake ? String(localized: "Jump back to this point (paused)")
                                                     : String(localized: "Jump to this point on another take (paused)"),
                              subtitle: Engine.timecode(forFrame: b.frame) + (b.onActiveTake ? "" : String(localized: " · other take")),
                              image: b.onActiveTake ? thumbnail(m, frame: b.frame) : nil,
                              confirm: { m.gotoBookmark(b.id); menu.close(resume: false) },
                              x: QMAction(label: String(localized: "Delete")) { m.removeBookmark(b.id) },
                              y: QMAction(label: String(localized: "Rename")) {
                                  if let n = promptName(String(localized: "Bookmark Name"), b.name) { m.renameBookmark(b.id, n) }
                              },
                              verb: String(localized: "Jump")))
        }
        let per = QMPage.bookmarks.capacity
        let pages = QuickMenuNav.pageCount(items: all.count, perPage: per)
        let range = QuickMenuNav.pageRange(items: all.count, perPage: per, page: menu.listPageIndex(.bookmarks))
        return QMContent(items: Array(all[range]), pageCount: pages,
                         caption: pages > 1 ? "\(menu.listPageIndex(.bookmarks) + 1) / \(pages)" : nil)
    }

    // MARK: Practice

    private static func practice(_ m: AppModel, _ menu: QuickMenuController) -> [QMItem] {
        let st = m.status
        return m.practiceSlots.map { slot in
            let n = slot.index
            let active = st.practicing && st.practiceSlot == n
            let color = SlotColors.color(n)
            guard slot.hasA else {
                return QMItem(id: "slot\(n)", icon: "plus", title: String(localized: "Set A Here"),
                              detail: String(localized: "Make the current position the start (A) of section \(n + 1)"),
                              enabled: !st.practicing, subtitle: String(localized: "Section \(n + 1)"), accent: color,
                              confirm: { m.practiceSetA(n) }, verb: String(localized: "Set A"))
            }
            var item = QMItem(id: "slot\(n)", icon: "target", title: slot.displayName, detail: "",
                              subtitle: slot.hasB ? Engine.timecode(forFrame: slot.length) : String(localized: "A only"),
                              image: slot.hasTakeFrame && slot.takeID == st.activeTake ? thumbnail(m, frame: slot.takeFrame) : nil,
                              accent: color, badge: active ? String(localized: "Practicing") : nil,
                              x: QMAction(label: String(localized: "Clear")) { m.practiceClear(n) },
                              y: QMAction(label: String(localized: "Rename")) {
                                  if let name = promptName(String(localized: "Section Name"), slot.name) { m.practiceRename(n, name) }
                              })
            if active && !slot.hasB {
                item.detail = String(localized: "Make the current position the end (B) of this section")
                item.confirm = { m.practiceSetB(n) }
                item.verb = String(localized: "Set B")
            } else if active {
                item.detail = String(localized: "Stop practicing and return to the take")
                item.confirm = { m.practiceStop(); menu.close(resume: false) }
                item.verb = String(localized: "Stop")
            } else {
                item.detail = slot.hasB ? String(localized: "Practice this section: play A to B, then it starts again at A")
                                        : String(localized: "Practice from A; open the menu where the section ends to set B")
                item.confirm = { menu.close(resume: false); m.practiceStart(n) }
                item.verb = String(localized: "Practice")
            }
            return item
        }
    }

    // MARK: Share

    private static func share(_ m: AppModel, _ menu: QuickMenuController) -> [QMItem] {
        let stream = StreamOutputModel.shared
        return [
            QMItem(id: "export", icon: "film", title: String(localized: "Export MP4"),
                   detail: String(localized: "Save the take as one continuous video"),
                   enabled: m.status.hasSession && m.status.takeLength > 0,
                   confirm: { menu.close(resume: false); m.showExport = true }),
            QMItem(id: "stream", icon: "dot.radiowaves.left.and.right", title: String(localized: "Stream Output"),
                   detail: String(localized: "Send the picture to OBS (Syphon)"),
                   subtitle: stream.isOn ? String(localized: "On") : String(localized: "Off"), page: .stream),
        ]
    }

    private static func stream() -> [QMItem] {
        let s = StreamOutputModel.shared
        let crt = CRTSettingsModel.shared
        let sizes = StreamOutputSize.allCases
        let sizeIndex = sizes.firstIndex(of: s.size) ?? 0
        let modes = CRTSyphonMode.allCases
        let modeIndex = modes.firstIndex(of: crt.syphonMode) ?? 0
        let status: String
        if let f = s.failure { status = String(localized: "Couldn’t start: \(f)") }
        else if s.active { status = String(localized: "Live: choose “\(SyphonPublisher.serverName)” in an OBS Syphon Client source") }
        else { status = String(localized: "Send the game picture to OBS (Syphon Client source)") }
        return [
            toggle("streamOn", "dot.radiowaves.left.and.right", String(localized: "Stream Output"), status, s.isOn) { s.setOn($0) },
            choice("streamSize", "aspectratio", String(localized: "Size"), String(localized: "Size of the streamed picture"),
                   sizes.map { $0.label(par87: s.par87) }, sizeIndex) { s.sizeRaw = sizes[$0].rawValue },
            toggle("streamPAR", "rectangle.arrowtriangle.2.outward", "8:7", String(localized: "Pixel aspect ratio of a CRT TV"), s.par87) { s.par87 = $0 },
            choice("streamCRT", "tv", String(localized: "Picture"), String(localized: "Stream the CRT picture or the original pixels"),
                   [String(localized: "Follow Display"), String(localized: "Original"), String(localized: "CRT")], modeIndex) { crt.syphonModeRaw = modes[$0].rawValue },
        ]
    }

    // MARK: Settings › Display

    private static func display(_ m: AppModel) -> [QMItem] {
        let crt = CRTSettingsModel.shared
        let levels = FlashLevel.allCases
        return [
            choice("size", "arrow.up.left.and.arrow.down.right", String(localized: "Size"),
                   String(localized: "Integer: sharp pixels · FILL: fill the window"),
                   [String(localized: "Integer"), "FILL"], m.integerScale ? 0 : 1) { m.integerScale = $0 == 0 },
            toggle("crt", "tv", String(localized: "CRT"), String(localized: "Look like a CRT TV (display only)"), crt.enabled) { crt.enabled = $0 },
            toggle("par", "rectangle.arrowtriangle.2.outward", "8:7", String(localized: "Pixel aspect ratio of a CRT TV"), m.displayPAR87) { m.displayPAR87 = $0 },
            choice("flash", "bolt.trianglebadge.exclamationmark", String(localized: "Reduce Flashing"),
                   String(localized: "Tone down full-screen flashes (photosensitivity)"),
                   levels.map(\.label), levels.firstIndex(of: m.flashLevel) ?? 2) { m.flashReduction = levels[$0].rawValue },
            toggle("overscan", "crop", String(localized: "Hide Edges"), String(localized: "Hide 8 pixels at each edge (overscan)"), m.hideOverscan) { m.hideOverscan = $0 },
            link("crtDetail", "slider.horizontal.3", String(localized: "CRT Details"), String(localized: "Scanlines, afterglow and signal of the CRT"), .crtDetail),
        ]
    }

    private static func crtDetail() -> [QMItem] {
        let c = CRTSettingsModel.shared
        var lines = slider("crtLineCount", "line.3.horizontal", String(localized: "Line Count"),
                           String(localized: "Number of lines in the experiment"),
                           Double(c.lines), 110...220, step: 10, text: "\(c.lines)") { c.lines = Int($0) }
        lines.enabled = c.reducedLines
        return [
            toggle("beam", "line.3.horizontal.decrease", String(localized: "Thick Bright Lines"),
                   String(localized: "Scanlines grow thicker where the picture is brighter"), c.beamGrowth) { c.beamGrowth = $0 },
            toggle("persist", "sparkles", String(localized: "Afterglow"), String(localized: "Phosphor afterglow"), c.persistence) { c.persistence = $0 },
            toggle("supply", "sun.max", String(localized: "Bright Shrink"),
                   String(localized: "Very bright screens grow and dim slightly"), c.supply) { c.supply = $0 },
            slider("antenna", "antenna.radiowaves.left.and.right", String(localized: "Signal"),
                   String(localized: "Antenna signal strength (lower = noisier)"),
                   c.antennaDbuv, 20...90, step: 5, text: "\(Int(c.antennaDbuv)) dBµV") { c.antennaDbuv = $0 },
            choice("crtLines", "line.3.horizontal", String(localized: "Scanlines"),
                   String(localized: "Standard 240 lines, or an experiment with fewer"),
                   ["240", String(localized: "Fewer")], c.reducedLines ? 1 : 0) { c.reducedLines = $0 == 1 },
            lines,
        ]
    }

    // MARK: Settings › Controls

    private static func controls(_ m: AppModel) -> [QMItem] {
        return [
            link("controller", "gamecontroller", String(localized: "Controller"),
                 String(localized: "See and change the buttons on a picture of your controller"), .controller),
            link("bindings", "keyboard", String(localized: "Keyboard"), String(localized: "Keys for the game and the hotkeys"), .bindings),
            confirmButton(m),
            toggle("dpadPaused", "dpad", String(localized: "D-pad While Paused"),
                   String(localized: "← / → step one frame while paused"), m.dpadStepWhenPaused) { m.dpadStepWhenPaused = $0 },
            toggle("pauseRewind", "pause.circle", String(localized: "Pause After Rewind"),
                   String(localized: "Pause when rewind / fast-forward is released"), m.pauseAfterRewind) { m.pauseAfterRewind = $0 },
            link("controlsDetail", "slider.horizontal.3", String(localized: "Controls Details"),
                 String(localized: "Turbo, opposite directions, stick"), .controlsDetail),
        ]
    }

    /// Settings › Controls › Confirm Button: which face button confirms in menus (east by default,
    /// Nintendo style, on every family), as the glyph pair of the connected controller family.
    private static func confirmButton(_ m: AppModel) -> QMItem {
        let south = m.southConfirm
        let info = m.input.controllerMonitor.controllers.min { $0.slot < $1.slot }
        let label = { (e: String) in info?.label(e) ?? ControllerFamily.xbox.label(e) }
        let ok = String(cString: rnf_ui_confirm_element(south ? 1 : 0)), back = String(cString: rnf_ui_cancel_element(south ? 1 : 0))
        let set = { (s: Bool) in m.southConfirm = s }
        return QMItem(id: "confirmButton", icon: "checkmark.circle", title: String(localized: "Confirm Button"),
                      detail: String(localized: "Which button confirms in menus (the other goes back)"),
                      value: .buttons([(label(ok), String(localized: "Confirm")), (label(back), String(localized: "Back"))]),
                      confirm: { set(!south) }, adjust: { _ in set(!south) }, verb: String(localized: "Change"))
    }

    private static func controlsDetail(_ m: AppModel) -> [QMItem] {
        let c = m.inputConfig
        let policies = ["neutral", "last_wins", "allow"]
        let rate = 60.0988 / Double(max(1, c.turboPeriod))
        return [
            QMItem(id: "turbo", icon: "bolt", title: String(localized: "Turbo Speed"),
                   detail: String(localized: "Presses per second of Turbo A / B"),
                   value: .slider(Double(32 - c.turboPeriod), 2...30, String(format: "%.1f/s", rate)),
                   adjust: { d in
                       let p = min(30, max(2, c.turboPeriod - d))
                       m.input.setTurbo(period: p, duty: min(c.turboDuty, p - 1))
                   },
                   setFraction: { f in
                       let p = min(30, max(2, 30 - Int((f * 28).rounded())))
                       m.input.setTurbo(period: p, duty: min(c.turboDuty, p - 1))
                   }),
            choice("socd", "arrow.left.and.right", String(localized: "Opposite Directions"),
                   String(localized: "What ←+→ or ↑+↓ pressed together does"),
                   [String(localized: "Neither"), String(localized: "Last Wins"), String(localized: "Both")],
                   policies.firstIndex(of: c.socd) ?? 0) { m.input.setSOCD(policies[$0]) },
            slider("stick", "l.joystick", String(localized: "Stick Threshold"),
                   String(localized: "How far the stick must tilt to count as the D-pad"),
                   c.analogThreshold, 0.2...0.9, step: 0.05, text: String(format: "%.2f", c.analogThreshold)) { m.input.setAnalogThreshold($0) },
            slider("duty", "timer", String(localized: "Turbo Press Length"), String(localized: "Frames each turbo press is held"),
                   Double(c.turboDuty), 1...Double(max(2, c.turboPeriod - 1)), step: 1,
                   text: String(localized: "\(c.turboDuty) f")) { m.input.setTurbo(period: c.turboPeriod, duty: Int($0)) },
            QMItem(id: "resetBindings", icon: "arrow.counterclockwise", title: String(localized: "Default Buttons"),
                   detail: String(localized: "Restore the default keys and buttons"),
                   confirm: {
                       m.input.resetToDefaults()
                       m.flash(String(localized: "Buttons restored to the defaults"))
                   }, verb: String(localized: "Restore")),
        ]
    }

    /// Controller diagram: the item list is empty; the page draws the diagram (QuickMenuView) and
    /// the controller focus moves between its elements (QuickMenuController.handleController).
    private static func controller(_ m: AppModel, _ menu: QuickMenuController) -> QMContent {
        let slot = menu.listPageIndex(.controller)
        let monitor = m.input.controllerMonitor
        let info = monitor.controllers.first { $0.slot == slot }
        let player = slot == 0 ? "1P" : "2P"
        let name = info?.name ?? String(localized: "Not Connected")
        return QMContent(items: [], pageCount: 2, caption: String(localized: "Pad \(slot + 1) (\(player)): \(name)"),
                         x: QMAction(label: String(localized: "Next Pad")) { menu.setListPage((slot + 1) % 2, on: .controller) },
                         y: QMAction(label: String(localized: "Defaults")) { m.input.resetController(slot: slot) })
    }

    /// The diagram's family for pad `slot`: the connected controller's, or the setting
    /// "diagramFamily" (a ControllerFamily raw value; "" / "auto" = the controller's).
    static func diagramFamily(_ m: AppModel, slot: Int) -> ControllerFamily {
        if let forced = UserDefaults.standard.string(forKey: "diagramFamily"), let f = ControllerFamily(rawValue: forced) { return f }
        return m.input.controllerMonitor.controllers.first { $0.slot == slot }?.family ?? .generic
    }

    /// "" (None) + the pad's own player's actions, the other player's, the hotkeys (shared core).
    static func assignChoices(slot: Int) -> [String] {
        let n = rnf_input_assign_choices(Int32(slot), nil, 0)
        var buf = [UnsafePointer<CChar>?](repeating: nil, count: n)
        _ = buf.withUnsafeMutableBufferPointer { rnf_input_assign_choices(Int32(slot), $0.baseAddress, n) }
        return buf.map { $0.map { String(cString: $0) } ?? "" }
    }

    /// Settings › Controls › Controller › a button: its action, 6 rows per sheet (L / R), no scrolling.
    private static func assign(_ m: AppModel, _ menu: QuickMenuController) -> QMContent {
        guard let id = menu.assignElement, let colon = id.firstIndex(of: ":") else { return QMContent(items: []) }
        let element = String(id[id.index(after: colon)...])
        let slot = Int(id.dropFirst(2).prefix { $0.isNumber }) ?? 0
        let current = ControllerAssignments.actions(element: element, slot: slot, config: m.inputConfig)
        let all = assignChoices(slot: slot).map { a -> QMItem in
            let action = InputCatalog.allActions.first { $0.id == a }
            let title: String
            switch action?.group {
            case nil: title = String(localized: "None")
            case .player1?: title = "1P " + (action?.label ?? a)
            case .player2?: title = "2P " + (action?.label ?? a)
            case .hotkey?: title = action?.label ?? a
            }
            let on = a.isEmpty ? current.isEmpty : current.contains(a)
            return QMItem(id: "assign:" + a, icon: a.isEmpty ? "xmark" : action?.group == .hotkey ? "bolt" : "gamecontroller",
                          title: title, detail: action?.group == .hotkey ? String(localized: "Hotkeys (not recorded)") : "",
                          value: .text(on ? "✓" : ""),
                          confirm: { [weak menu] in
                              m.input.setAssignment(id, action: a.isEmpty ? nil : a)
                              menu?.back()
                          }, verb: String(localized: "Assign"))
        }
        let per = QMPage.assign.capacity
        let pages = QuickMenuNav.pageCount(items: all.count, perPage: per)
        let range = QuickMenuNav.pageRange(items: all.count, perPage: per, page: menu.listPageIndex(.assign))
        let family = diagramFamily(m, slot: slot)
        let labels = m.input.controllerMonitor.controllers.first { $0.slot == slot }?.labels ?? [:]
        return QMContent(items: Array(all[range]), pageCount: pages,
                         caption: ControllerAssignments.title(element: element, family: family, labels: labels))
    }

    /// Bindings, 6 per page: 1P, 2P, hotkeys.
    static func bindingPages() -> [(InputAction.Group, [InputAction])] {
        var out: [(InputAction.Group, [InputAction])] = []
        for g in InputAction.Group.allCases {
            let actions = InputCatalog.allActions.filter { $0.group == g }
            var i = 0
            while i < actions.count {
                out.append((g, Array(actions[i..<min(actions.count, i + 6)])))
                i += 6
            }
        }
        return out
    }

    private static func bindings(_ m: AppModel, _ menu: QuickMenuController) -> QMContent {
        let pages = bindingPages()
        let k = min(menu.listPageIndex(.bindings), pages.count - 1)
        let (group, actions) = pages[k]
        let sameGroup = pages.filter { $0.0 == group }.count
        let nth = pages[..<k].filter { $0.0 == group }.count + 1
        let controllers = m.input.controllerMonitor.controllers
        let items = actions.map { a -> QMItem in
            let ids = m.inputConfig.inputs(for: a.id).sorted()
            let names = ids.map { InputCatalog.displayName($0, controllers: controllers) }
            let capturing = m.capturingAction == a.id
            let text = capturing ? String(localized: "Press a key…")
                : (names.isEmpty ? String(localized: "None") : names.joined(separator: ", "))
            return QMItem(id: a.id, icon: a.group == .hotkey ? "command" : "gamecontroller", title: a.label,
                          detail: names.isEmpty ? String(localized: "Not assigned · Enter adds a key")
                                                : String(localized: "Assigned: \(names.joined(separator: ", "))"),
                          value: .text(text),
                          confirm: { startCapture(m, menu, a.id) },
                          x: ids.isEmpty ? nil : QMAction(label: String(localized: "Clear")) { for id in ids { m.input.unbind(id, from: a.id) } },
                          verb: String(localized: "Add"))
        }
        let caption = group.title + (sameGroup > 1 ? " · \(nth)/\(sameGroup)" : "")
        return QMContent(items: items, pageCount: pages.count, caption: caption)
    }

    private static func startCapture(_ m: AppModel, _ menu: QuickMenuController, _ action: String) {
        m.capturingAction = action
        m.input.captureHandler = { [weak menu] id in
            m.capturingAction = nil
            if let id { m.input.bind(id, to: action) }
            menu?.touch()
        }
    }

    // MARK: Settings › Sound / System

    private static func sound(_ m: AppModel) -> [QMItem] {
        [
            slider("volume", "speaker.wave.2", String(localized: "Volume"),
                   String(localized: "Muted while paused, rewinding, in slow motion or fast-forward"),
                   m.volume, 0...1, step: 0.05, text: "\(Int((m.volume * 100).rounded()))%") { m.volume = $0 },
        ]
    }

    private static func system(_ m: AppModel) -> [QMItem] {
        let langs = ["auto", "ja", "en"]
        let current = langs.firstIndex(of: UILanguage.preference) ?? 0
        let intervals: [Double] = [2, 3, 5, 10, 30]
        return [
            choice("language", "globe", String(localized: "Language"), String(localized: "Applies the next time ReplayNES starts"),
                   [String(localized: "Auto"), UILanguage.nativeName("ja"), UILanguage.nativeName("en")], current) {
                UILanguage.preference = langs[$0]
            },
            choice("autosave", "clock", String(localized: "Autosave"), String(localized: "How often your recording is saved in the background"),
                   intervals.map { "\(Int($0))s" }, intervals.firstIndex(of: m.autosaveInterval) ?? 0) { m.autosaveInterval = intervals[$0] },
            toggle("latency", "speedometer", String(localized: "Latency Meter"),
                   String(localized: "Show input and display latency over the game"), m.showLatency) { m.showLatency = $0 },
            toggle("flashBadge", "bolt.badge.checkmark", String(localized: "Flash Notice"),
                   String(localized: "Show a badge while flashing is being reduced"), m.showFlashIndicator) { m.showFlashIndicator = $0 },
            link("updates", "arrow.triangle.2.circlepath", String(localized: "Updates"),
                 String(localized: "Automatic checks and installing updates"), .updates),
            link("about", "info.circle", String(localized: "About"), String(localized: "Version and licenses"), .about),
        ]
    }

    private static func updates() -> [QMItem] {
        let u = UpdaterModel.shared
        var auto = toggle("autoInstall", "arrow.down.circle", String(localized: "Install Automatically"),
                          String(localized: "Download in the background and install when you quit"), u.automaticallyDownloads) { u.automaticallyDownloads = $0 }
        auto.enabled = u.automaticallyChecks
        let last = u.lastCheck.map { String(localized: "Last checked: \($0.formatted(date: .abbreviated, time: .shortened))") }
            ?? String(localized: "Not checked yet")
        return [
            toggle("autoCheck", "arrow.triangle.2.circlepath", String(localized: "Check Automatically"),
                   String(localized: "Look for new versions on GitHub (nothing personal is sent)"), u.automaticallyChecks) { u.automaticallyChecks = $0 },
            auto,
            QMItem(id: "checkNow", icon: "magnifyingglass", title: String(localized: "Check Now"), detail: last,
                   enabled: u.canCheckForUpdates, confirm: { u.checkForUpdates() }, verb: String(localized: "Check")),
        ]
    }

    private static func about() -> [QMItem] {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = "\(info["CFBundleShortVersionString"] as? String ?? "?") (\(info["CFBundleVersion"] as? String ?? "?"))"
        let site = "https://github.com/kathoc/ReplayNES"
        return [
            QMItem(id: "version", icon: "info.circle", title: String(localized: "Version"), detail: "ReplayNES \(version)", value: .text(version)),
            QMItem(id: "core", icon: "cpu", title: String(localized: "Emulation Core"),
                   detail: String(localized: "Projects open only with the same core (exact replays)"), value: .text(Engine.coreCompatID)),
            QMItem(id: "site", icon: "safari", title: String(localized: "Website"),
                   detail: String(localized: "Source code and manual (GPL-2.0-or-later)"), value: .text("github.com/kathoc/ReplayNES"),
                   confirm: { if let u = URL(string: site) { NSWorkspace.shared.open(u) } }, verb: String(localized: "Open")),
            QMItem(id: "licenses", icon: "doc.text", title: String(localized: "Licenses"),
                   detail: String(localized: "Third-party software in ReplayNES"),
                   confirm: { if let u = URL(string: site + "/blob/main/THIRD_PARTY_NOTICES.md") { NSWorkspace.shared.open(u) } },
                   verb: String(localized: "Open")),
        ]
    }

    // MARK: Game

    private static func game(_ m: AppModel, _ menu: QuickMenuController) -> [QMItem] {
        let has = m.status.hasSession
        return [
            QMItem(id: "choose", icon: "square.grid.2x2", title: String(localized: "Choose Game"),
                   detail: String(localized: "Open the library of your ROMs"), confirm: { menu.close(resume: false); m.showLibraryScreen() }),
            QMItem(id: "save", icon: "square.and.arrow.down", title: String(localized: "Save"),
                   detail: String(localized: "Save the project (it is also saved automatically)"), enabled: has,
                   confirm: { m.saveSync() }),
            QMItem(id: "saveAs", icon: "square.and.arrow.down.on.square", title: String(localized: "Save As"),
                   detail: String(localized: "Save the project to a new place"), enabled: has, confirm: { m.saveAs() }),
            QMItem(id: "reset", icon: "power", title: String(localized: "Reset"),
                   detail: String(localized: "Reset, power cycle, or start the project over"), enabled: has, page: .reset),
            QMItem(id: "close", icon: "xmark.circle", title: String(localized: "Close"),
                   detail: String(localized: "Close this game and return to the library"), enabled: has,
                   confirm: { menu.close(resume: false); m.closeProject() }),
        ]
    }

    private static func reset(_ m: AppModel, _ menu: QuickMenuController) -> [QMItem] {
        let st = m.status
        let live = st.recording || st.practicing
        return [
            QMItem(id: "soft", icon: "arrow.counterclockwise", title: String(localized: "Soft Reset"),
                   detail: String(localized: "Press the console’s RESET button (recorded)"), enabled: live,
                   confirm: { menu.close(resume: true); m.softReset() }),
            QMItem(id: "power", icon: "power", title: String(localized: "Power Cycle"),
                   detail: String(localized: "Turn the console off and on again (recorded)"), enabled: live,
                   confirm: { menu.close(resume: true); m.powerCycle() }),
            QMItem(id: "startOver", icon: "trash", title: String(localized: "Start Over"),
                   detail: String(localized: "Delete every take and record again from power-on"),
                   confirm: { m.resetProjectPrompt() }),
        ]
    }

    // MARK: helpers

    private static func toggle(_ id: String, _ icon: String, _ title: String, _ detail: String, _ on: Bool,
                               _ set: @escaping (Bool) -> Void) -> QMItem {
        QMItem(id: id, icon: icon, title: title, detail: detail, value: .toggle(on),
               confirm: { set(!on) }, adjust: { d in set(d > 0) }, verb: String(localized: "Switch"))
    }

    private static func choice(_ id: String, _ icon: String, _ title: String, _ detail: String, _ options: [String], _ index: Int,
                               _ set: @escaping (Int) -> Void) -> QMItem {
        QMItem(id: id, icon: icon, title: title, detail: detail, value: .choice(options, index),
               confirm: { set((index + 1) % options.count) },
               adjust: { d in set(min(options.count - 1, max(0, index + d))) },
               setChoice: { set($0) }, verb: String(localized: "Change"))
    }

    private static func slider(_ id: String, _ icon: String, _ title: String, _ detail: String, _ value: Double,
                               _ range: ClosedRange<Double>, step: Double, text: String,
                               _ set: @escaping (Double) -> Void) -> QMItem {
        let clamp = { (v: Double) in min(range.upperBound, max(range.lowerBound, (v / step).rounded() * step)) }
        return QMItem(id: id, icon: icon, title: title, detail: detail, value: .slider(value, range, text),
                      adjust: { d in set(clamp(value + Double(d) * step)) },
                      setFraction: { f in set(clamp(range.lowerBound + f * (range.upperBound - range.lowerBound))) })
    }

    private static func link(_ id: String, _ icon: String, _ title: String, _ detail: String, _ page: QMPage) -> QMItem {
        QMItem(id: id, icon: icon, title: title, detail: detail, page: page, verb: String(localized: "Open"))
    }

    /// A thumbnail of the active take near `frame` (filmstrip cache), if one was captured.
    private static func thumbnail(_ m: AppModel, frame: UInt64) -> CGImage? {
        FilmstripModel.shared.cache.image(before: frame + 1, within: 600)
    }

    /// Small modal text prompt (rename).
    static func promptName(_ title: String, _ current: String) -> String? {
        let a = NSAlert()
        a.messageText = title
        let field = NSTextField(string: current)
        field.frame = NSRect(x: 0, y: 0, width: 260, height: 24)
        a.accessoryView = field
        a.addButton(withTitle: String(localized: "Rename"))
        a.addButton(withTitle: String(localized: "Cancel"))
        a.window.initialFirstResponder = field
        guard a.runModal() == .alertFirstButtonReturn else { return nil }
        return field.stringValue
    }
}
