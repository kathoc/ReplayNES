// Transport controls + timeline scrubber.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct HoldButton: View {
    let systemImage: String
    let help: String
    let onChange: (Bool) -> Void
    @State private var held = false

    var body: some View {
        Image(systemName: systemImage)
            .font(.system(size: 15, weight: .medium))
            .frame(width: 34, height: 26)
            .background(held ? Color.accentColor.opacity(0.35) : Color.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in if !held { held = true; onChange(true) } }
                .onEnded { _ in held = false; onChange(false) })
            .help(help)
    }
}

struct TimelineScrubber: View {
    @EnvironmentObject var model: AppModel
    @State private var dragFrame: UInt64?

    var body: some View {
        let st = model.status
        let length = max(st.takeLength, 1)
        GeometryReader { geo in
            let w = geo.size.width
            let cur = Double(dragFrame ?? st.frame) / Double(length)
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 3).fill(Color.secondary.opacity(0.25)).frame(height: 6)
                RoundedRectangle(cornerRadius: 3).fill(st.recording ? Color.red.opacity(0.7) : Color.green.opacity(0.7))
                    .frame(width: max(0, min(w, w * cur)), height: 6)
                ForEach(model.bookmarks.filter { $0.onActiveTake }) { b in
                    Rectangle().fill(Color.yellow)
                        .frame(width: 2, height: 14)
                        .offset(x: w * Double(b.frame) / Double(length) - 1)
                        .help(b.name)
                }
                Circle().fill(Color.white).shadow(radius: 1)
                    .frame(width: 14, height: 14)
                    .offset(x: max(0, min(w, w * cur)) - 7)
            }
            .frame(height: 18)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { v in
                    let f = UInt64(max(0, min(1, v.location.x / max(w, 1))) * Double(st.takeLength))
                    if f != dragFrame { dragFrame = f; model.scrub(to: f) }
                }
                .onEnded { _ in
                    // Keep showing the drag position until the emulation thread catches up.
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { dragFrame = nil }
                })
        }
        .frame(height: 18)
        .help("タイムライン: ドラッグで移動（無音・一時停止）")
    }
}

struct TransportBar: View {
    @EnvironmentObject var model: AppModel
    @State private var advanceCount = 10

    var body: some View {
        let st = model.status
        VStack(spacing: 8) {
            HStack(spacing: 10) {
                Text(Engine.timecode(forFrame: st.frame)).font(.system(.body, design: .monospaced))
                TimelineScrubber()
                Text(Engine.timecode(forFrame: st.takeLength)).font(.system(.body, design: .monospaced)).foregroundStyle(.secondary)
            }
            HStack(spacing: 8) {
                Picker("", selection: Binding(get: { st.recording }, set: { model.setRecording($0) })) {
                    Text("● 録画").tag(true)
                    Text("▶︎ 再生").tag(false)
                }
                .pickerStyle(.segmented).labelsHidden().frame(width: 140)
                .help("録画: 入力を記録 / 再生: 記録済みのテイクを再生")

                Divider().frame(height: 22)
                Button { model.seek(to: 0) } label: { Image(systemName: "backward.end.fill") }.help("先頭へ")
                HoldButton(systemImage: "backward.fill", help: "押している間 巻き戻し（Delete キー / L1）") { model.setRewindHeld($0) }
                Button { model.stepBack() } label: { Image(systemName: "backward.frame.fill") }.help("1コマ戻る ( , )")
                Button { model.togglePause() } label: {
                    Image(systemName: st.paused ? "play.fill" : "pause.fill").frame(width: 22)
                }
                .help("再開 / 一時停止 (Space)")
                Button { model.frameAdvance() } label: { Image(systemName: "forward.frame.fill") }.help("コマ送り ( . )")
                HoldButton(systemImage: "forward.fill", help: "押している間 早送り（Tab / R1）") { model.setFastForwardHeld($0) }

                HStack(spacing: 2) {
                    TextField("", value: $advanceCount, format: .number)
                        .frame(width: 44).multilineTextAlignment(.trailing)
                    Button("コマ進める") { model.frameAdvance(max(1, advanceCount)) }.fixedSize()
                }
                .help("指定したフレーム数だけ進めて一時停止")

                Picker("", selection: Binding(get: { st.slow }, set: { model.setSlow($0) })) {
                    ForEach(SlowRate.allCases) { Text($0.label).tag($0) }
                }
                .pickerStyle(.segmented).labelsHidden().frame(width: 130)
                .help("スロー再生（無音）。記録されるフレームは変わりません")

                Divider().frame(height: 22)
                Button { model.rerecordHere() } label: { Label("ここから録り直す", systemImage: "record.circle").fixedSize() }
                    .help("現在のフレームから録画を再開します。以前の続きは別テイクとして残ります")
                Button { model.undoTake() } label: { Label("前の試行へ戻す", systemImage: "arrow.uturn.backward").fixedSize() }
                    .disabled(st.undoDepth == 0)
                    .help("直前のテイク切替・録り直しを取り消します")

                Spacer(minLength: 0)
                Button { model.addBookmark() } label: { Image(systemName: "bookmark") }.help("ブックマーク追加 (B)")
                Menu {
                    Button("ソフトリセット") { model.softReset() }
                    Button("電源再投入（パワーサイクル）") { model.powerCycle() }
                } label: { Image(systemName: "power") }
                .menuStyle(.borderlessButton).frame(width: 40)
                .disabled(!st.recording)
                .help("リセットは録画モードでフレーム単位のイベントとして記録されます")
            }
            HStack {
                Text("フレーム \(st.frame) / \(st.takeLength)")
                Text("テイク #\(st.activeTake)（全\(st.takeCount)）").foregroundStyle(.secondary)
                if st.unsaved { Text("未保存の変更あり").foregroundStyle(.orange) }
                Spacer()
                Text(model.controllers.isEmpty ? "コントローラー: 未接続（キーボード操作）" : "コントローラー: \(model.controllers.count)台接続")
                    .foregroundStyle(.secondary)
            }
            .font(.caption)
        }
        .padding(.horizontal, 14).padding(.vertical, 10)
        .buttonStyle(.bordered)
    }
}
