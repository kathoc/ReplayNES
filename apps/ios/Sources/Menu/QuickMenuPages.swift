// The items of every Quick Menu page on iPhone. Same tree and item ids as macOS
// (apps/macos/Sources/App/QuickMenu/QuickMenuPages.swift; structure: Core/QuickMenuModel.swift,
// shared), with the iPhone's differences:
//  - Share: Make Video only (no Syphon stream); Settings › Controls › "Keyboard" is the touch
//    controls page here; System › Unlocks opens the Unlocks sheet (paid features, prototype switch)
//    and System › ReplayNES Version shows the version only (no updater on iPhone);
//  - paid features show a lock and open Unlocks when locked (FeatureGate);
//  - renames are SwiftUI prompts; Save As is folded into Save (projects are named automatically).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

struct QMAction {
    let label: String
    let run: () -> Void
}

struct QMItem: Identifiable {
    enum Value {
        case none
        case toggle(Bool)
        case choice([String], Int)
        case slider(Double, ClosedRange<Double>, String)
        case text(String)
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
    var page: QMPage?
    var confirm: (() -> Void)?
    var adjust: ((Int) -> Void)?
    var setChoice: ((Int) -> Void)?
    var setFraction: ((Double) -> Void)?
    var x: QMAction?
    var y: QMAction?
    var verb: String?
    /// iOS: the paid feature this item needs while it is locked (drawn with a lock).
    var locked: PaidFeature?
}

struct QMContent {
    var items: [QMItem]
    var pageCount = 1
    var caption: String?
    var x: QMAction?
    var y: QMAction?
    /// The Options button (shared QuickMenuController; no iPhone page sets one yet).
    var options: QMAction?
}

extension QMPage {
    /// Page titles on iPhone (the shared titles, except the pages iPhone uses differently).
    var iosTitle: String {
        switch self {
        case .bindings: return L("Touch Controls")
        default: return title
        }
    }
}

enum QuickMenuPages {
    static func content(_ p: QMPage, model m: AppModel, menu: QuickMenuController) -> QMContent {
        switch p {
        case .top: return QMContent(items: top(m, menu))
        case .retry: return QMContent(items: retry(m, menu))
        case .bookmarks: return bookmarks(m, menu)
        case .practice: return QMContent(items: practice(m, menu))
        case .share, .stream: return QMContent(items: share(m, menu))
        case .display: return QMContent(items: display(m))
        case .displayShape: return QMContent(items: displayShape(m))
        case .crtDetail:
            return QMContent(items: crtDetail(m), y: QMAction(label: String(localized: "Defaults")) { CRTSettingsModel.shared.resetToNestermDefaults() })
        case .controls: return QMContent(items: controls(m))
        case .controller: return controller(m, menu)
        case .assign: return assign(m, menu)
        case .bindings: return QMContent(items: touchControls(m))
        case .controlsDetail: return QMContent(items: controlsDetail(m))
        case .sound: return QMContent(items: sound(m))
        case .system: return QMContent(items: system(m, menu))
        case .about: return QMContent(items: about(m))
        case .game: return QMContent(items: game(m, menu))
        case .reset: return QMContent(items: reset(m, menu))
        // The frontend's Play and Turbo pages (macOS has them): iPhone's settings tabs leave them out and their
        // rows stay on iPhone's own pages (QMPage.settingsTabs), so nothing opens these here.
        case .play, .turbo: return QMContent(items: [])
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
                   detail: String(localized: "Play again from here, watch the tape, start over"), enabled: has, page: .retry),
            QMItem(id: "practice", icon: "target", title: String(localized: "Practice"),
                   detail: String(localized: "Repeat a section from A to B (nothing is recorded)"), enabled: has, page: .practice),
            QMItem(id: "share", icon: "square.and.arrow.up", title: String(localized: "Share"),
                   detail: L("Make a video"), page: .share),
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
        // docs/design/VTR_UX.md: no takes, no "Previous Attempt" (the ↶ Undo chip), no REC wording.
        let st = m.status
        let playback = !st.recording && !st.practicing
        let items = [
            QMItem(id: "rerecord", icon: "play.fill", title: String(localized: "Play from Here"),
                   detail: String(localized: "Play on from this point (the tape continues from here)"),
                   confirm: { menu.close(resume: false); m.rerecordHere() }),
            QMItem(id: "bookmarks", icon: "bookmark", title: String(localized: "Bookmarks"),
                   detail: String(localized: "Jump to a marked point, or mark this one"),
                   enabled: !st.practicing, subtitle: "\(m.bookmarks.count)", page: .bookmarks),
            QMItem(id: "playback", icon: "play.rectangle", title: String(localized: "Playback (menu)"),
                   detail: String(localized: "Watch the tape without changing it (a game button plays from there)"),
                   enabled: st.takeLength > 0 && !st.practicing,
                   subtitle: playback ? String(localized: "On") : String(localized: "Off"),
                   confirm: { menu.close(resume: false); m.toggleRecord() }),
            QMItem(id: "restart", icon: "backward.end", title: String(localized: "Start Over"),
                   detail: String(localized: "Erase the tape and play again from power-on"),
                   confirm: { menu.close(resume: false); m.resetProjectPrompt() }),
        ]
        assert(items.map(\.id) == QMPage.retryItemIDs, "the shared core's Retry page")
        return items
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
                              image: b.onActiveTake ? thumbnail(frame: b.frame) : nil,
                              confirm: { m.gotoBookmark(b.id); menu.close(resume: false) },
                              x: QMAction(label: String(localized: "Delete")) { m.removeBookmark(b.id) },
                              y: QMAction(label: String(localized: "Rename")) {
                                  m.prompt = AppPrompt(title: String(localized: "Bookmark Name"), text: b.name) { m.renameBookmark(b.id, $0) }
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
            let color = SlotPalette.color(n)
            guard slot.hasA else {
                return QMItem(id: "slot\(n)", icon: "plus", title: String(localized: "Set A Here"),
                              detail: String(localized: "Make the current position the start (A) of section \(n + 1)"),
                              enabled: !st.practicing, subtitle: String(localized: "Section \(n + 1)"), accent: color,
                              confirm: { m.practiceSetA(n) }, verb: String(localized: "Set A"))
            }
            var item = QMItem(id: "slot\(n)", icon: "target", title: slot.displayName, detail: "",
                              subtitle: slot.hasB ? Engine.timecode(forFrame: slot.length) : String(localized: "A only"),
                              image: slot.hasTakeFrame && slot.takeID == st.activeTake ? thumbnail(frame: slot.takeFrame) : nil,
                              accent: color, badge: active ? String(localized: "Practicing") : nil,
                              x: QMAction(label: String(localized: "Clear")) { m.practiceClear(n) },
                              y: QMAction(label: String(localized: "Rename")) {
                                  m.prompt = AppPrompt(title: String(localized: "Section Name"), text: slot.name) { m.practiceRename(n, $0) }
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
        let unlocked = m.entitlements.isUnlocked(.mp4Export)
        return [
            QMItem(id: "export", icon: "film", title: String(localized: "Make Video"),
                   detail: String(localized: "Turn the tape into a video file"),
                   enabled: m.status.hasSession && m.status.takeLength > 0,
                   confirm: { menu.close(resume: false); m.requestExport() },
                   locked: unlocked ? nil : .mp4Export),
        ]
    }

    // MARK: Settings › Display

    private static func display(_ m: AppModel) -> [QMItem] {
        let crt = CRTSettingsModel.shared
        let levels = FlashLevel.allCases
        let crtOK = m.entitlements.isUnlocked(.crt)
        var crtItem = toggle("crt", "tv", String(localized: "CRT"), String(localized: "Look like a CRT TV (display only)"), crt.enabled) {
            if crtOK { crt.enabled = $0 } else { m.showLocked(.crt) }
        }
        crtItem.locked = crtOK ? nil : .crt
        var detail = link("crtDetail", "slider.horizontal.3", String(localized: "CRT Details"), String(localized: "Scanlines, afterglow and signal of the CRT"), .crtDetail)
        detail.locked = crtOK ? nil : .crt
        return [
            choice("size", "arrow.up.left.and.arrow.down.right", String(localized: "Size"),
                   L("Integer: sharp pixels · FILL: as large as the screen allows"),
                   [String(localized: "Integer"), "FILL"], m.integerScale ? 0 : 1) { m.integerScale = $0 == 0 },
            crtItem,
            choice("flash", "bolt.trianglebadge.exclamationmark", String(localized: "Reduce Flashing"),
                   String(localized: "Tone down full-screen flashes (photosensitivity)"),
                   levels.map(\.label), levels.firstIndex(of: m.flashLevel) ?? 2) { m.flashReduction = levels[$0].rawValue },
            link("shape", "aspectratio", String(localized: "Picture Shape"), L("8:7 pixels, hide the edges, blurred sides, orientation"), .displayShape),
            detail,
        ]
    }

    /// Settings › Display › Picture Shape (as on the Mac: 8:7 and Hide Edges), plus the iPhone's
    /// screen orientation (Auto / Portrait / Landscape lock: OrientationLock).
    private static func displayShape(_ m: AppModel) -> [QMItem] {
        var items = [
            toggle("par", "rectangle.arrowtriangle.2.outward", "8:7", String(localized: "Pixel aspect ratio of a CRT TV"), m.displayPAR87) { m.displayPAR87 = $0 },
            toggle("overscan", "crop", String(localized: "Hide Edges"), String(localized: "Hide 8 pixels at each edge (overscan)"), m.hideOverscan) { m.hideOverscan = $0 },
            toggle("backdrop", "photo", String(localized: "Blurred Sides"),
                   String(localized: "The empty sides show the game enlarged, blurred and dimmed"), m.backdrop) { m.backdrop = $0 },
        ]
        if UIDevice.current.userInterfaceIdiom == .phone {
            items.append(choice("orientation", "rectangle.portrait.rotate", L("Orientation"),
                                L("Auto follows the iPhone · Portrait / Landscape lock the screen"),
                                [L("Auto"), L("Portrait"), L("Landscape")],
                                OrientationLock(rawValue: m.orientationLock)?.rawValue ?? 0) { m.orientationLock = $0 })
        }
        return items
    }

    private static func crtDetail(_ m: AppModel) -> [QMItem] {
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
        [
            link("controller", "gamecontroller", String(localized: "Controller"),
                 String(localized: "See and change the buttons on a picture of your controller"), .controller),
            link("bindings", "hand.tap", L("Touch Controls"), L("D-pad size and follow, opacity, turbo buttons and vibration of the on-screen controls"), .bindings),
            confirmButton(m),
            toggle("dpadPaused", "dpad", String(localized: "D-pad While Paused"),
                   String(localized: "← / → step one frame while paused"), m.dpadStepWhenPaused) { m.dpadStepWhenPaused = $0 },
            toggle("pauseRewind", "pause.circle", String(localized: "Pause After Rewind"),
                   String(localized: "Pause when rewind / fast-forward is released"), m.pauseAfterRewind) { m.pauseAfterRewind = $0 },
            link("controlsDetail", "slider.horizontal.3", String(localized: "Controls Details"),
                 String(localized: "Turbo, opposite directions, stick"), .controlsDetail),
        ]
    }

    /// Settings › Controls › Touch Controls (iPhone's page in place of Keyboard).
    private static func touchControls(_ m: AppModel) -> [QMItem] {
        [
            slider("touchOpacity", "circle.lefthalf.filled", L("Opacity"), L("How visible the on-screen controls are"),
                   m.touchOpacity, 0.15...1, step: 0.05, text: "\(Int((m.touchOpacity * 100).rounded()))%") { m.touchOpacity = $0 },
            // NESTERM 操作: 十字キーの大きさ (130...210 px), 追従 ↕ / ↔ (0...2x), and the 4/8-way switch.
            slider("touchDpadSize", "dpad", L("D-pad Size"), L("Size of the on-screen D-pad"),
                   m.touchDpadSize, Double(TouchLayout.dpadRange.lowerBound)...Double(TouchLayout.dpadRange.upperBound), step: 2,
                   text: "\(Int(m.touchDpadSize.rounded())) pt") { m.touchDpadSize = $0 },
            slider("touchFollowY", "arrow.up.and.down", L("D-pad Follow ↕\u{FE0E}"), L("How fast the D-pad follows your thumb up and down"),
                   m.touchFollowY, 0...2, step: 0.05, text: String(format: "%.2fx", m.touchFollowY)) { m.touchFollowY = $0 },
            slider("touchFollowX", "arrow.left.and.right", L("D-pad Follow ↔\u{FE0E}"), L("How fast the D-pad follows your thumb left and right"),
                   m.touchFollowX, 0...2, step: 0.05, text: String(format: "%.2fx", m.touchFollowX)) { m.touchFollowX = $0 },
            choice("touchWays", "arrow.up.and.down.and.arrow.left.and.right", L("D-pad Directions"),
                   L("4-way ignores diagonals (remembered per game)"), ["8", "4"], m.touchWays == 4 ? 1 : 0) {
                if ($0 == 1) != (m.touchWays == 4) { m.toggleTouchWays() }
            },
            toggle("touchTurbo", "bolt.circle", L("Turbo Buttons"), L("Show AA / BB (Turbo A / Turbo B) up-left of A / B"), m.touchAABB) { m.touchAABB = $0 },
            toggle("touchHaptics", "iphone.radiowaves.left.and.right", L("Vibration"), L("A short vibration when a button is pressed"), m.touchHaptics) { m.touchHaptics = $0 },
            toggle("touchHideWithPad", "gamecontroller", L("Hide With Controller"), L("Hide the on-screen controls while a controller is connected"),
                   m.touchHideWithPad) { m.touchHideWithPad = $0 },
        ]
    }

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
            // As on macOS (UI_REDESIGN.md, "Resume countdown"): Controls is full (six rows).
            choice("resumeCountdown", "play.circle", String(localized: "Resume Countdown"),
                   L("Before play resumes after a pause or a rewind, and before each practice run (1 s: the ring alone, no numeral)"),
                   [L("3 s"), L("1 s"), L("None")],
                   m.countdownSeconds >= 3 ? 0 : m.countdownSeconds >= 1 ? 1 : 2) { m.countdownSeconds = [3, 1, 0][$0] },
            QMItem(id: "resetBindings", icon: "arrow.counterclockwise", title: String(localized: "Default Buttons"),
                   detail: String(localized: "Restore the default keys and buttons"),
                   confirm: {
                       m.input.resetToDefaults()
                       m.restoreTouchBindings()
                       m.flash(String(localized: "Buttons restored to the defaults"))
                   }, verb: String(localized: "Restore")),
        ]
    }

    private static func controller(_ m: AppModel, _ menu: QuickMenuController) -> QMContent {
        let slot = menu.listPageIndex(.controller)
        let info = m.input.controllerMonitor.controllers.first { $0.slot == slot }
        let player = slot == 0 ? "1P" : "2P"
        let name = info?.name ?? String(localized: "Not Connected")
        return QMContent(items: [], pageCount: 2, caption: String(localized: "Pad \(slot + 1) (\(player)): \(name)"),
                         x: QMAction(label: String(localized: "Next Pad")) { menu.setListPage((slot + 1) % 2, on: .controller) },
                         y: QMAction(label: String(localized: "Defaults")) { m.input.resetController(slot: slot) })
    }

    /// Rows under the controller diagram, remembered for the controller in the slot shown
    /// (docs/plans/2026-11-21-apple-controller-kind-family.md): its kind (another core's layout only, as the
    /// desktop's "Kind: Automatic (Pad)") and its button layout. Confirm / a tap cycles the choice.
    static func controllerRows(_ m: AppModel, slot: Int) -> [QMItem] {
        guard let info = m.input.controllerMonitor.controllers.first(where: { $0.slot == slot }) else { return [] }
        let input = m.input
        var rows: [QMItem] = []
        if input.layoutActive {
            let choice = input.kindChoice(slot: slot)
            let name = { (stick: Bool) in stick ? String(localized: "Arcade Stick") : String(localized: "Pad") }
            let value = choice == .automatic ? String(format: String(localized: "Automatic (%@)"), name(info.detectedStick)) : name(choice == .stick)
            rows.append(QMItem(id: "controllerKind", icon: "gamecontroller", title: String(format: String(localized: "Kind: %@"), value),
                               detail: L("Pad or arcade stick: each has its own buttons for this game"), confirm: {
                let all = InputManager.KindChoice.allCases
                input.setKindChoice(all[((all.firstIndex(of: choice) ?? 0) + 1) % all.count], slot: slot)
            }, verb: String(localized: "Change")))
        }
        let chosen = input.familyChoice(slot: slot)
        let order: [ControllerFamily?] = [nil, .nintendo, .xbox, .playStation]
        let text = chosen.map(\.title) ?? String(format: String(localized: "Automatic (%@)"), (info.detectedFamily ?? info.family).title)
        rows.append(QMItem(id: "controllerFamily", icon: "circle.grid.cross", title: L("Button Layout: %@", text),
                           detail: L("Where A, B, X and Y are on this controller"), confirm: {
            let i = order.firstIndex(where: { $0 == chosen }) ?? 0
            input.setFamilyChoice(order[(i + 1) % order.count], slot: slot)
        }, verb: String(localized: "Change")))
        return rows
    }

    static func diagramFamily(_ m: AppModel, slot: Int) -> ControllerFamily {
        if let forced = UserDefaults.standard.string(forKey: "diagramFamily"), let f = ControllerFamily(rawValue: forced) { return f }
        return m.input.controllerMonitor.controllers.first { $0.slot == slot }?.family ?? .generic
    }

    static func assignChoices(slot: Int) -> [String] {
        let n = rnf_input_assign_choices(Int32(slot), nil, 0)
        var buf = [UnsafePointer<CChar>?](repeating: nil, count: n)
        _ = buf.withUnsafeMutableBufferPointer { rnf_input_assign_choices(Int32(slot), $0.baseAddress, n) }
        return buf.map { $0.map { String(cString: $0) } ?? "" }
    }

    private static func assign(_ m: AppModel, _ menu: QuickMenuController) -> QMContent {
        guard let id = menu.assignElement, let colon = id.firstIndex(of: ":") else { return QMContent(items: []) }
        let element = String(id[id.index(after: colon)...])
        let slot = Int(id.dropFirst(2).prefix { $0.isNumber }) ?? 0
        let current = ControllerAssignments.actions(element: element, slot: slot, config: m.inputConfig)
        let catalog = Dictionary(InputCatalog.allActions.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        let all = assignChoices(slot: slot).map { a -> QMItem in
            let action = catalog[a]
            let title = action.map { $0.group.playerPrefix + $0.label } ?? String(localized: "None")
            let on = a.isEmpty ? current.isEmpty : current.contains(a)
            return QMItem(id: "assign:" + a, icon: a.isEmpty ? "xmark" : action?.group == .hotkey ? "bolt" : "gamecontroller",
                          title: title, detail: action?.group == .hotkey ? String(localized: "Hotkeys (not recorded)") : "",
                          value: .text(on ? "✓" : ""),
                          confirm: { [weak menu] in
                              m.input.setAssignment(id, action: a.isEmpty ? nil : a)
                              menu?.back()
                          }, verb: String(localized: "Assign"))
        }
        let per = QMPage.assign.scrolls ? max(1, all.count) : QMPage.assign.capacity
        let pages = QuickMenuNav.pageCount(items: all.count, perPage: per)
        let range = QuickMenuNav.pageRange(items: all.count, perPage: per, page: QMPage.assign.scrolls ? 0 : menu.listPageIndex(.assign))
        let family = diagramFamily(m, slot: slot)
        let labels = m.input.controllerMonitor.controllers.first { $0.slot == slot }?.labels ?? [:]
        return QMContent(items: Array(all[range]), pageCount: pages,
                         caption: ControllerAssignments.title(element: element, family: family, labels: labels))
    }

    // MARK: Settings › Sound / System

    private static func sound(_ m: AppModel) -> [QMItem] {
        [
            slider("volume", "speaker.wave.2", String(localized: "Volume"),
                   String(localized: "Muted while paused, rewinding, in slow motion or fast-forward"),
                   m.volume, 0...1, step: 0.05, text: "\(Int((m.volume * 100).rounded()))%") { m.volume = $0 },
        ]
    }

    private static func system(_ m: AppModel, _ menu: QuickMenuController) -> [QMItem] {
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
            // Unlocks: the sheet (the debug switch, each paid feature); no updater on iPhone, so the
            // ReplayNES Version page (shared .about, which merges Updates on the Mac) shows the version only.
            QMItem(id: "unlocks", icon: "lock.open", title: L("Unlocks"), detail: L("Recording, MP4 export and CRT (test build: all unlocked)"),
                   confirm: { m.showUnlocks = true }, verb: String(localized: "Open")),
            link("about", "info.circle", String(localized: "ReplayNES Version"), L("Version and licenses"), .about),
        ]
    }

    private static func about(_ m: AppModel) -> [QMItem] {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = "\(info["CFBundleShortVersionString"] as? String ?? "?") (\(info["CFBundleVersion"] as? String ?? "?"))"
        let site = "https://github.com/kathoc/ReplayNES"
        return [
            QMItem(id: "version", icon: "info.circle", title: String(localized: "Version"), detail: "ReplayNES \(version) (iPhone test build)", value: .text(version)),
            QMItem(id: "core", icon: "cpu", title: String(localized: "Emulation Core"),
                   detail: String(localized: "Projects open only with the same core (exact replays)"), value: .text(Engine.coreCompatID)),
            QMItem(id: "site", icon: "safari", title: String(localized: "Website"),
                   detail: String(localized: "Source code and manual (GPL-2.0-or-later)"), value: .text("github.com/kathoc/ReplayNES"),
                   confirm: { if let u = URL(string: site) { UIApplication.shared.open(u) } }, verb: String(localized: "Open")),
            QMItem(id: "licenses", icon: "doc.text", title: String(localized: "Licenses"),
                   detail: String(localized: "Third-party software in ReplayNES"),
                   confirm: { if let u = URL(string: site + "/blob/main/THIRD_PARTY_NOTICES.md") { UIApplication.shared.open(u) } },
                   verb: String(localized: "Open")),
            // The on-screen controller is ported from Brickboy via NESTERM (Apache-2.0): its notice and
            // licence text ship in the app (NOTICE, LICENSE; apps/ios/ThirdParty/brickboy).
            QMItem(id: "brickboy", icon: "hand.tap", title: L("Touch Controller"),
                   detail: "Brickboy (NESTERM) © 2026 Katsuya Hokamura · Apache-2.0", value: .text("Apache-2.0"),
                   confirm: { showBundledText(m, title: L("Touch Controller"), file: "NOTICE", then: "LICENSE") },
                   verb: String(localized: "Open")),
        ]
    }

    /// A bundled text file in an alert (scrollable), optionally with a button for a second one.
    private static func showBundledText(_ m: AppModel, title: String, file: String, then next: String? = nil) {
        let text = Bundle.main.url(forResource: file, withExtension: nil).flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? ""
        var actions = [AppAlert.Action(title: String(localized: "OK"), role: .cancel)]
        if let next {
            actions.insert(AppAlert.Action(title: L("License Text")) { DispatchQueue.main.async { showBundledText(m, title: "Apache License 2.0", file: next) } }, at: 0)
        }
        m.alert = AppAlert(title: title, message: text, actions: actions)
    }

    // MARK: Game

    private static func game(_ m: AppModel, _ menu: QuickMenuController) -> [QMItem] {
        let has = m.status.hasSession
        let saveOK = m.entitlements.isUnlocked(.recording)
        return [
            QMItem(id: "choose", icon: "square.grid.2x2", title: String(localized: "Choose Game"),
                   detail: String(localized: "Open the library of your ROMs"), confirm: { menu.close(resume: false); m.showLibraryScreen() }),
            QMItem(id: "save", icon: "square.and.arrow.down", title: String(localized: "Save"),
                   detail: L("Keep this recording as a project (it is also saved automatically)"), enabled: has,
                   confirm: { m.save() }, locked: saveOK ? nil : .recording),
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
                   detail: String(localized: "Erase the tape and play again from power-on"),
                   confirm: { menu.close(resume: false); m.resetProjectPrompt() }),
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

    private static func thumbnail(frame: UInt64) -> CGImage? {
        FilmstripModel.shared.cache.image(before: frame + 1, within: 600)
    }
}
