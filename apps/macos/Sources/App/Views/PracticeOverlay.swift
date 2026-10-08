// While a practice run plays: a compact pill with the section and its progress (the sections
// themselves are in the Quick Menu: Practice).
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct PracticeOverlay: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let st = model.status
        HStack(spacing: 10) {
            Image(systemName: "target").foregroundStyle(SlotColors.color(max(0, st.practiceSlot)))
            Text(currentName(st)).lineLimit(1)
            ClockView { Text(verbatim: progress($0)).font(.system(.caption, design: .monospaced)).foregroundStyle(.white.opacity(0.65)) }
            if st.practiceLoops > 0 { Text("Loop \(st.practiceLoops + 1)").font(.caption).foregroundStyle(.white.opacity(0.65)) }
        }
        .font(.system(size: 12, weight: .medium))
        .foregroundStyle(.white)
        .padding(.horizontal, 12).padding(.vertical, 6)
        .background(.black.opacity(0.55), in: Capsule())
        .allowsHitTesting(false)
    }

    private func currentName(_ st: EmuStatus) -> String {
        guard st.practiceSlot >= 0, st.practiceSlot < model.practiceSlots.count else { return String(localized: "Practicing") }
        return "\(st.practiceSlot + 1). " + model.practiceSlots[st.practiceSlot].displayName
    }

    private func progress(_ st: EmuStatus) -> String {
        let len = st.practiceLength > 0 ? Engine.timecode(forFrame: st.practiceLength) : "--:--.--"
        return "\(Engine.timecode(forFrame: min(st.practiceFrame, st.practiceLength > 0 ? st.practiceLength : st.practiceFrame))) / \(len)"
    }
}
