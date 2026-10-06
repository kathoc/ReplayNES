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
        .alert("区間の名前", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("名前", text: $renameText)
            Button("変更") { if let r = renaming { model.practiceRename(r.index, renameText) }; renaming = nil }
            Button("キャンセル", role: .cancel) { renaming = nil }
        }
    }

    // MARK: compact (playing)

    private func pill(_ st: EmuStatus) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "repeat").foregroundStyle(.orange)
            Text(currentName(st)).lineLimit(1)
            Text(progress(st)).font(.system(.caption, design: .monospaced)).foregroundStyle(.secondary)
            if st.practiceLoops > 0 { Text("\(st.practiceLoops + 1)回目").font(.caption).foregroundStyle(.secondary) }
            Button("練習をやめる") { model.practiceStop() }.controlSize(.small)
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
                Text(st.practicing ? "練習中：\(currentName(st))" : "練習（A/B リピート）").font(.system(size: 13, weight: .semibold))
                if st.practicing {
                    Text(progress(st)).font(.system(.caption, design: .monospaced)).foregroundStyle(.secondary)
                }
                Spacer(minLength: 8)
                if st.practicing {
                    Button("練習をやめる") { model.practiceStop() }.controlSize(.small)
                } else {
                    Button { model.showPracticePanel = false } label: { Image(systemName: "xmark") }
                        .buttonStyle(.borderless).help("閉じる")
                }
            }
            Text(st.practicing
                 ? "Bに着くと少し止まってAへ戻り、繰り返します。録画はされません。"
                 : "A＝区間の始まり、B＝Aから続けてプレイした終わりの位置。設定した区間を何度でも練習できます（録画はされません）。")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            VStack(spacing: 2) {
                ForEach(model.practiceSlots) { slot in row(slot, st) }
            }
        }
        .padding(10)
        .frame(width: 430)
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
            Text(slot.hasA ? slot.displayName : "（未設定）")
                .font(.system(size: 12))
                .foregroundStyle(slot.hasA ? .primary : .secondary)
                .lineLimit(1)
                .frame(maxWidth: .infinity, alignment: .leading)
            Text(slot.hasA && slot.hasB ? Engine.timecode(forFrame: slot.length) : "--:--.--")
                .font(.system(size: 11, design: .monospaced)).foregroundStyle(.secondary)
            marker("A", set: slot.hasA, help: "Aを設定（いまの位置を区間の始まりにする）") { model.practiceSetA(slot.index) }
            marker("B", set: slot.hasB, help: "Bを設定（Aから続けてプレイした、いまの位置を終わりにする）") { model.practiceSetB(slot.index) }
            Button { model.practiceStart(slot.index) } label: {
                Image(systemName: active ? "arrow.counterclockwise" : "play.fill").frame(width: 22, height: 18)
            }
            .buttonStyle(.borderless)
            .disabled(!slot.hasA)
            .help(active ? "Aからやり直す" : "この区間を練習")
            Menu {
                Button("この区間を練習") { model.practiceStart(slot.index) }.disabled(!slot.hasA)
                Button("Aを設定") { model.practiceSetA(slot.index) }
                Button("Bを設定") { model.practiceSetB(slot.index) }
                Divider()
                Button("名前変更…") { renameText = slot.name; renaming = slot }.disabled(!slot.hasA)
                Button("消去", role: .destructive) { model.practiceClear(slot.index) }.disabled(!slot.hasA)
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
        guard st.practiceSlot >= 0, st.practiceSlot < model.practiceSlots.count else { return "練習中" }
        return "\(st.practiceSlot + 1). " + model.practiceSlots[st.practiceSlot].displayName
    }

    private func progress(_ st: EmuStatus) -> String {
        let len = st.practiceLength > 0 ? Engine.timecode(forFrame: st.practiceLength) : "--:--.--"
        return "\(Engine.timecode(forFrame: min(st.practiceFrame, st.practiceLength > 0 ? st.practiceLength : st.practiceFrame))) / \(len)"
    }
}
