// Timeline actions: A/B ranges set directly on the take timeline (rn_practice_set_range) and
// the thumbnail hooks the emulation thread runs.
// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

extension EmulationController {
    // MARK: thumbnails (emulation thread)

    /// Keeps the thumbnail cache on the active take; frames the old and new take share stay.
    func syncThumbnailTake(_ s: EngineSession) {
        let t = s.activeTake
        let old = thumbnails.takeID
        guard t != old else { return }
        thumbnails.rebase(toTake: t, keepThrough: TakeLineage.sharedPrefix(s.takes(), old, t))
    }

    /// Live capture: the picture at cursor `frame` is on screen anyway; keep it if the filmstrip
    /// grid wants it (one locked dictionary lookup otherwise).
    func captureThumbnail(_ s: EngineSession, frame: UInt64) {
        syncThumbnailTake(s)
        let take = s.activeTake
        guard thumbnails.wants(frame: frame, take: take), let v = s.video,
              let img = ThumbnailScaler.thumbnail(v) else { return }
        thumbnails.insert(frame: frame, take: take, image: img)
    }

    // MARK: A/B on the timeline (emulation thread)

    /// The slot as it lies on the active take (nil: empty or set on another take / in practice).
    func timelineRange(_ s: EngineSession, _ slot: Int) -> TimelineRange? {
        TimelineEditing.visibleRanges(slots: [s.practiceSlot(slot)], takes: s.takes(), activeTake: s.activeTake,
                                      takeLength: s.takeLength).first
    }

    /// Defines slot = [a, b) on the take (drag on the timeline / A-B handles).
    func practiceSetRange(_ slot: Int, a: UInt64, b: UInt64) {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE { notice(Self.practiceBlockedText); return }
        do {
            try s.practiceSetRange(slot, a: a, b: b)
            markStructureDirty()
            notice("区間 \(slot + 1): A \(Engine.timecode(forFrame: a)) → B \(Engine.timecode(forFrame: b))"
                   + "（長さ \(Engine.timecode(forFrame: b - a))）。区間をクリックで練習できます")
        } catch { reportError("区間を設定できませんでした", error) }
    }

    /// 「Aをここに」: A at the playhead, keeping B when it is still after A.
    func timelineMarkA(_ slot: Int) {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE { notice(Self.practiceBlockedText); return }
        switch TimelineEditing.markA(at: s.frame, existing: timelineRange(s, slot)) {
        case .setRange(let a, let b): practiceSetRange(slot, a: a, b: b)
        case .setAOnly: practiceSetA(slot)
        case .invalid(let msg): notice(msg)
        }
    }

    /// 「Bをここに」: B at the playhead for a slot whose A is on this take (no need to have played
    /// continuously from A).
    func timelineMarkB(_ slot: Int) {
        guard let s = session else { return }
        if s.mode == RN_MODE_PRACTICE { notice(Self.practiceBlockedText); return }
        switch TimelineEditing.markB(at: s.frame, existing: timelineRange(s, slot)) {
        case .setRange(let a, let b): practiceSetRange(slot, a: a, b: b)
        case .setAOnly: practiceSetA(slot)
        case .invalid(let msg): notice(msg)
        }
    }

    /// OSD 「Bを設定」 after a seek since A: falls back to the take frames when A lies on this take.
    func practiceSetBFromTake(_ slot: Int) -> Bool {
        guard let s = session, s.mode != RN_MODE_PRACTICE,
              case .setRange(let a, let b) = TimelineEditing.markB(at: s.frame, existing: timelineRange(s, slot))
        else { return false }
        practiceSetRange(slot, a: a, b: b)
        return true
    }
}

extension AppModel {
    func timelineSetRange(_ slot: Int, a: UInt64, b: UInt64) {
        emu.perform { e in e.practiceSetRange(slot, a: a, b: b) }
    }
    func timelineMarkA() {
        let slot = FilmstripModel.shared.selectedSlot
        emu.perform { e in e.timelineMarkA(slot) }
    }
    func timelineMarkB() {
        let slot = FilmstripModel.shared.selectedSlot
        emu.perform { e in e.timelineMarkB(slot) }
    }
}
