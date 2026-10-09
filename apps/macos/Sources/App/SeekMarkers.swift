// The paused seek bar's A/B markers from the controller (docs/design/UI_REDESIGN.md, "A/B markers"):
// the selected A/B slot (FilmstripModel.selectedSlot, "A/B n") on the active take as zero, one or
// two flags. The state machine is the shared core's (rnf_markers_*, tests/test_frontend_seekbar.cpp);
// this applies its results (seek, write the slot) like the Linux / Windows frontend
// (apps/desktop/src/ui_play.cpp). Main thread only.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

final class SeekMarkers: ObservableObject {
    /// What the views draw (refreshed after every change).
    @Published private(set) var frames: [UInt64] = []
    @Published private(set) var focus = -1          // -1: the bar; else the marker (left to right)
    @Published private(set) var editing = false
    @Published private(set) var active = false      // paused on the seek bar, not practicing, no menu
    /// "Two markers already (X deletes one)" / "A marker is already here", for a moment.
    @Published private(set) var hint: String?

    private let handle: OpaquePointer = rnf_markers_new()!
    private var repeater = StepRepeater()          // D-pad left / right held while editing
    private var holdDir = 0, holdTicks = 0          // L / R held while editing (rnf_hold_speed)
    private var timer: Timer?
    private var hintWork: DispatchWorkItem?
    /// A write was sent to the emulation thread: the slot list (as it was then) is stale until a
    /// changed one comes back (or a second passed).
    private var writeSent: (at: Date, slots: [PracticeSlotInfo])?
    weak var model: AppModel?

    deinit { rnf_markers_free(handle) }

    /// 0 = the bar, 1 = a marker, 2 = editing (InputManager.setSeekFocus).
    var seekFocus: Int { !active ? 0 : editing ? 2 : focus >= 0 ? 1 : 0 }

    private var slot: Int { min(7, max(0, FilmstripModel.shared.selectedSlot)) }

    private func isActive(_ m: AppModel) -> Bool {
        let st = m.status
        return st.hasSession && st.paused && !st.practicing && !m.quickMenu.isOpen && !m.showLibrary && !m.showExport
    }

    /// After every status / structure / menu change: follow the selected slot (unless a marker is
    /// being edited) or leave (an edit is reverted; the picture goes back while still paused).
    func sync() {
        guard let m = model else { return }
        let now = isActive(m)
        if !now {
            let r = rnf_markers_leave(handle)
            let st = m.status
            if r.seek != 0 && st.hasSession && st.paused && !st.practicing { m.scrub(to: r.seek_frame) }
            holdDir = 0
            repeater.reset()
        } else if writeSent.map({ m.practiceSlots != $0.slots || Date().timeIntervalSince($0.at) > 1 }) ?? true {
            writeSent = nil
            let st = m.status
            let range = TimelineEditing.visibleRanges(slots: m.practiceSlots, takes: m.takes, activeTake: st.activeTake,
                                                      takeLength: st.takeLength).first { $0.slot == slot }
            if var c = range?.cValue {
                rnf_markers_sync(handle, &c, st.takeLength)
            } else {
                rnf_markers_sync(handle, nil, st.takeLength)
            }
        }
        active = now
        publish()
    }

    private func publish() {
        let n = Int(rnf_markers_count(handle))
        let f = (0..<n).map { rnf_markers_frame(handle, $0) }
        if f != frames { frames = f }
        let fo = Int(rnf_markers_focus(handle))
        if fo != focus { focus = fo }
        let ed = rnf_markers_editing(handle) != 0
        if ed != editing { editing = ed }
        if !ed { holdDir = 0; repeater.reset() }
        // D-pad repeat and L / R held move the edited marker every 60 Hz tick.
        if ed && active && timer == nil {
            let t = Timer(timeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.tick() }
            RunLoop.main.add(t, forMode: .common)
            timer = t
        } else if (!ed || !active), let t = timer {
            t.invalidate()
            timer = nil
        }
        model?.input.setSeekFocus(seekFocus)
    }

    private func tick() {
        guard active, editing else { return }
        let d = repeater.tick()
        if d != 0 { apply(rnf_markers_nudge(handle, Int64(d))) }
        if holdDir != 0 {
            holdTicks += 1
            apply(rnf_markers_nudge(handle, Int64(holdDir) * Int64(rnf_hold_speed(Int32(holdTicks)))))
        }
    }

    private func apply(_ r: rnf_markers_result) {
        guard let m = model else { return }
        let s = slot
        if r.seek != 0 { m.scrub(to: r.seek_frame) }
        switch r.write {
        case RNF_MARKERS_WRITE_A_ONLY:
            let a = r.a
            writeSent = (Date(), m.practiceSlots)
            m.emu.perform { e in e.practiceSetAAt(s, frame: a) }
        case RNF_MARKERS_WRITE_RANGE:
            writeSent = (Date(), m.practiceSlots)
            m.timelineSetRange(s, a: r.a, b: r.b, fromMarkers: true)
        case RNF_MARKERS_WRITE_CLEAR:
            writeSent = (Date(), m.practiceSlots)
            m.practiceClear(s)
        default: break
        }
        if r.outcome == RNF_MARKERS_FULL { showHint(String(localized: "Two markers already (X deletes one)")) }
        else if r.outcome == RNF_MARKERS_OCCUPIED { showHint(String(localized: "A marker is already here")) }
        publish()
    }

    private func showHint(_ text: String) {
        hint = text
        hintWork?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.hint = nil }
        hintWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5, execute: w)
    }

    // MARK: input (InputManager hooks)

    func seekInput(_ input: InputManager.SeekInput, down: Bool) {
        guard let m = model, active, isActive(m) else { return }
        let head = m.status.frame
        switch input {
        case .left, .right:
            let dir = input == .left ? -1 : 1
            if editing {
                if down { apply(rnf_markers_nudge(handle, Int64(repeater.press(dir)))) } else { repeater.release(dir) }
            } else if down {
                apply(rnf_markers_move(handle, Int32(dir), 0, head))
            }
        case .up: if down { apply(rnf_markers_move(handle, 0, -1, head)) }
        case .down: if down { apply(rnf_markers_move(handle, 0, 1, head)) }
        case .ok: if down { apply(rnf_markers_confirm(handle, head)) }
        case .cancel: if down { apply(rnf_markers_cancel(handle)) }
        case .x, .y:
            // X: delete the focused marker, or (on the bar) the next A/B slot; Y: practice the
            // selected slot's section from its A (rnf_markers_face).
            guard down else { return }
            switch rnf_markers_face(handle, input == .y ? 1 : 0) {
            case RNF_SEEK_FACE_DELETE_MARKER: apply(rnf_markers_delete(handle))
            case RNF_SEEK_FACE_NEXT_SLOT:
                FilmstripModel.shared.selectedSlot = (slot + 1) % 8
                _ = rnf_markers_leave(handle)
                writeSent = nil
                sync()
            case RNF_SEEK_FACE_PRACTICE:
                _ = rnf_markers_leave(handle)
                m.practiceStart(slot)   // plays from A (after the countdown)
            default: break
            }
        }
    }

    func markerHold(_ dir: Int, down: Bool) {
        guard down, active, editing else {
            if !down || dir == holdDir { holdDir = 0 }
            return
        }
        holdDir = dir
        holdTicks = 1
        apply(rnf_markers_nudge(handle, Int64(dir) * Int64(rnf_hold_speed(1))))
    }

    /// L / R released while paused: one frame back / forward (not while editing: they move it).
    func pausedShoulder(_ dir: Int) {
        guard let m = model, m.status.hasSession, m.status.paused, !m.quickMenu.isOpen, !m.showLibrary, !editing else { return }
        m.stepFrame(dir)
    }
}
