// Practice OSD (A/B repeat) drawn over the game viewport: 8 slots per project.
// Compact pill while a practice run is playing; full panel when paused, hovered or idle.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct PracticeOverlay: View {
    @EnvironmentObject var model: AppModel
    @State private var hovering = false
    @State private var renaming: PracticeSlotInfo?
    @State private var renameText = ""

    var body: some View {
        let st = model.status
        let compact = st.practicing && !st.paused && !hovering
        Group {
            if compact { pill(st) } else { panel(st) }
        }
        .environment(\.colorScheme, .dark)
        .onHover { h in withAnimation(.easeOut(duration: 0.15)) { hovering = h } }
        .alert("Section Name", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $renameText)
            Button("Rename") { if let r = renaming { model.practiceRename(r.index, renameText) }; renaming = nil }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
    }

    // MARK: compact (playing)

    private func pill(_ st: EmuStatus) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "repeat").foregroundStyle(.orange)
            Text(currentName(st)).lineLimit(1)
            ClockView { Text(progress($0)).font(.system(.caption, design: .monospaced)).foregroundStyle(.secondary) }
            if st.practiceLoops > 0 { Text("Loop \(st.practiceLoops + 1)").font(.caption).foregroundStyle(.secondary) }
            Button("Stop Practicing") { model.practiceStop() }.controlSize(.small)
        }
        .font(.system(size: 12, weight: .medium))
        .foregroundStyle(.white)
        .padding(.horizontal, 12).padding(.vertical, 6)
        .background(.black.opacity(0.55), in: Capsule())
        .transition(.opacity)
    }

    // MARK: full panel

    private func panel(_ st: EmuStatus) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: "repeat").foregroundStyle(.orange)
                Text(st.practicing ? String(localized: "Practicing: \(currentName(st))") : String(localized: "Practice (A/B Repeat)")).font(.system(size: 13, weight: .semibold))
                if st.practicing {
                    ClockView { Text(progress($0)).font(.system(.caption, design: .monospaced)).foregroundStyle(.secondary) }
                }
                Spacer(minLength: 8)
                if st.practicing {
                    Button("Stop Practicing") { model.practiceStop() }.controlSize(.small)
                } else {
                    Button { model.showPracticePanel = false } label: { Image(systemName: "xmark") }
                        .buttonStyle(.borderless).help("Close")
                }
            }
            Text(st.practicing
                 ? String(localized: "At B it pauses briefly, returns to A and repeats. Nothing is recorded.")
                 : String(localized: "A = start of the section, B = the end you reach by playing on from A. Practice a section as often as you like (nothing is recorded)."))
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            VStack(spacing: 2) {
                ForEach(model.practiceSlots) { slot in row(slot, st) }
            }
        }
        .padding(10)
        .frame(width: 400)
        .background(.black.opacity(0.62), in: RoundedRectangle(cornerRadius: 10))
        .foregroundStyle(.white)
        .transition(.opacity)
    }

    private func row(_ slot: PracticeSlotInfo, _ st: EmuStatus) -> some View {
        let active = st.practicing && st.practiceSlot == slot.index
        return HStack(spacing: 6) {
            Text("\(slot.index + 1)").font(.system(size: 12, weight: .bold, design: .monospaced))
                .frame(width: 16)
                .foregroundStyle(active ? .orange : .secondary)
            Text(slot.hasA ? slot.displayName : String(localized: "(not set)"))
                .font(.system(size: 12))
                .foregroundStyle(slot.hasA ? .primary : .secondary)
                .lineLimit(1)
                .frame(maxWidth: .infinity, alignment: .leading)
            Text(slot.hasA && slot.hasB ? Engine.timecode(forFrame: slot.length) : "--:--.--")
                .font(.system(size: 11, design: .monospaced)).foregroundStyle(.secondary)
            marker("A", set: slot.hasA, help: String(localized: "Set A (make the current position the start of the section)")) { model.practiceSetA(slot.index) }
            marker("B", set: slot.hasB, help: String(localized: "Set B (make the current position, reached by playing on from A, the end)")) { model.practiceSetB(slot.index) }
            Button { model.practiceStart(slot.index) } label: {
                Image(systemName: active ? "arrow.counterclockwise" : "play.fill").frame(width: 22, height: 18)
            }
            .buttonStyle(.borderless)
            .disabled(!slot.hasA)
            .opacity(slot.hasA ? 1 : 0.3)
            .help(active ? String(localized: "Restart from A") : String(localized: "Practice This Section"))
            Menu {
                Button("Practice This Section") { model.practiceStart(slot.index) }.disabled(!slot.hasA)
                Button("Set A") { model.practiceSetA(slot.index) }
                Button("Set B") { model.practiceSetB(slot.index) }
                Divider()
                Button("Rename…") { renameText = slot.name; renaming = slot }.disabled(!slot.hasA)
                Button("Clear", role: .destructive) { model.practiceClear(slot.index) }.disabled(!slot.hasA)
            } label: { Image(systemName: "ellipsis") }
                .menuStyle(.borderlessButton).menuIndicator(.hidden).frame(width: 22)
        }
        .padding(.horizontal, 6).padding(.vertical, 3)
        .background(active ? Color.orange.opacity(0.22) : Color.clear, in: RoundedRectangle(cornerRadius: 5))
    }

    private func marker(_ label: String, set: Bool, help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label).font(.system(size: 11, weight: .bold))
                .frame(width: 22, height: 18)
                .foregroundStyle(set ? Color.black : Color.white.opacity(0.8))
                .background(set ? Color.orange : Color.white.opacity(0.14), in: RoundedRectangle(cornerRadius: 4))
        }
        .buttonStyle(.plain)
        .help(help)
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
