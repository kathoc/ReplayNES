// Transport controls + timeline scrubber (essentials only; the rest lives in menus / "…").
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct HoldButton: View {
    let systemImage: String
    let help: String
    let onChange: (Bool) -> Void
    @State private var held = false

    var body: some View {
        Image(systemName: systemImage)
            .font(.system(size: 14, weight: .medium))
            .frame(width: 34, height: 26)
            .background(held ? Color.accentColor.opacity(0.35) : Color.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in if !held { held = true; onChange(true) } }
                .onEnded { _ in held = false; onChange(false) })
            .help(help)
    }
}

/// Plain icon button matching HoldButton's look.
struct IconButton: View {
    let systemImage: String
    let help: String
    var prominent = false
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: prominent ? 16 : 14, weight: .medium))
                .frame(width: prominent ? 40 : 34, height: 26)
                .background(Color.secondary.opacity(prominent ? 0.2 : 0.12), in: RoundedRectangle(cornerRadius: 6))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
    }
}

/// The single record toggle: red glowing 「録画」 in record mode, gray in replay mode.
struct RecordToggleButton: View {
    @EnvironmentObject var model: AppModel
    @State private var pulse = false

    var body: some View {
        let st = model.status
        let on = st.recording && !st.practicing
        Button { model.toggleRecord() } label: {
            HStack(spacing: 6) {
                Circle().fill(on ? Color.red : Color.secondary.opacity(0.6)).frame(width: 9, height: 9)
                Text("録画").font(.system(size: 13, weight: .semibold))
            }
            .foregroundStyle(on ? Color.white : Color.secondary)
            .frame(width: 78, height: 26)
            .background(
                RoundedRectangle(cornerRadius: 13)
                    .fill(on ? Color.red.opacity(0.85) : Color.secondary.opacity(0.15))
            )
            .shadow(color: on ? Color.red.opacity(pulse ? 0.85 : 0.35) : .clear, radius: on ? (pulse ? 9 : 4) : 0)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(st.practicing)
        .onAppear { withAnimation(.easeInOut(duration: 1.1).repeatForever(autoreverses: true)) { pulse = true } }
        .help(on ? "録画モード（クリックで再生モード：録画したテイクを再生します）"
                 : "再生モード（クリックで録画モードに戻り、この位置から続きを録画します）")
    }
}

struct TimelineScrubber: View {
    @EnvironmentObject var model: AppModel
    @State private var dragFrame: UInt64?

    var body: some View {
        let st = model.status
        if st.practicing {
            practiceBar(st)
        } else {
            takeBar(st)
        }
    }

    /// While practicing the take is frozen: show progress inside the A->B section instead.
    private func practiceBar(_ st: EmuStatus) -> some View {
        GeometryReader { geo in
            let w = geo.size.width
            let len = max(st.practiceLength, 1)
            let p = st.practiceLength == 0 ? 0 : min(1, Double(st.practiceFrame) / Double(len))
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 3).fill(Color.orange.opacity(0.2)).frame(height: 6)
                RoundedRectangle(cornerRadius: 3).fill(Color.orange.opacity(0.8)).frame(width: w * p, height: 6)
            }
            .frame(height: 18)
        }
        .frame(height: 18)
        .help("練習中: A→B の進み具合（テイクは変更されません）")
    }

    private func takeBar(_ st: EmuStatus) -> some View {
        let length = max(st.takeLength, 1)
        return GeometryReader { geo in
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
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        let st = model.status
        VStack(spacing: 6) {
            HStack(spacing: 10) {
                Text(leftTime(st)).font(.system(.callout, design: .monospaced))
                TimelineScrubber()
                Text(rightTime(st)).font(.system(.callout, design: .monospaced)).foregroundStyle(.secondary)
            }
            HStack(spacing: 6) {
                RecordToggleButton()
                Divider().frame(height: 20).padding(.horizontal, 4)
                IconButton(systemImage: "backward.end.fill", help: st.practicing ? "Aへ戻る" : "先頭へ") { model.seek(to: 0) }
                HoldButton(systemImage: "backward.fill", help: "押している間 巻き戻し（R2 / Delete）") { model.setRewindHeld($0) }
                IconButton(systemImage: "backward.frame.fill", help: "1コマ戻る（一時停止中: 十字キー← / ,）") { model.stepBack() }
                    .opacity(st.paused ? 1 : 0).disabled(!st.paused)
                IconButton(systemImage: st.paused ? "play.fill" : "pause.fill", help: "再開 / 一時停止（R / Space）", prominent: true) {
                    model.togglePause()
                }
                IconButton(systemImage: "forward.frame.fill", help: "コマ送り（一時停止中: 十字キー→ / .）") { model.frameAdvance() }
                    .opacity(st.paused ? 1 : 0).disabled(!st.paused)
                HoldButton(systemImage: "forward.fill", help: "押している間 早送り（L2 / Tab）。録画済みの範囲だけを再生し、終端で止まります") {
                    model.setFastForwardHeld($0)
                }
                .opacity(st.practicing ? 0.35 : 1).disabled(st.practicing)
                Button { model.toggleSlow() } label: {
                    Text(st.slow == .normal ? "スロー" : "スロー \(st.slow.label)")
                        .font(.system(size: 12, weight: .medium))
                        .frame(minWidth: 58, minHeight: 26)
                        .background(st.slow == .normal ? Color.secondary.opacity(0.12) : Color.purple.opacity(0.35),
                                    in: RoundedRectangle(cornerRadius: 6))
                }
                .buttonStyle(.plain)
                .help("スロー 1/2 ⇔ 等速（L / L キー）")

                Spacer(minLength: 8)

                Button { model.togglePracticePanel() } label: {
                    Label(st.practicing ? "練習をやめる" : "練習", systemImage: "repeat")
                        .font(.system(size: 12, weight: .medium))
                        .padding(.horizontal, 10).frame(height: 26)
                        .background(st.practicing || model.showPracticePanel ? Color.orange.opacity(0.4) : Color.secondary.opacity(0.12),
                                    in: RoundedRectangle(cornerRadius: 6))
                }
                .buttonStyle(.plain)
                .help("練習モード（A/B リピート）: 区間を何度でも練習できます。録画はされません（⇧⌘P）")

                ScaleToggle()

                Menu {
                    Button("ブックマークを追加") { model.addBookmark() }.disabled(st.practicing)
                    Button("前の試行へ戻す") { model.undoTake() }.disabled(st.undoDepth == 0 || st.practicing)
                    Button("テイク一覧…") { openWindow(id: "takes") }
                    Divider()
                    Menu("指定フレーム数だけ進める") {
                        ForEach([5, 10, 30, 60], id: \.self) { n in Button("\(n) コマ") { model.frameAdvance(n) } }
                    }
                    Divider()
                    Button("ソフトリセット") { model.softReset() }.disabled(!st.recording && !st.practicing)
                    Button("電源再投入（パワーサイクル）") { model.powerCycle() }.disabled(!st.recording && !st.practicing)
                    Divider()
                    Toggle("レイテンシ表示", isOn: $model.showLatency)
                    Toggle("サイドバー", isOn: $model.showSidebar)
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .frame(width: 30)
                .help("その他（ブックマーク・テイク・リセットなど）")
            }
        }
        .padding(.horizontal, 14).padding(.vertical, 8)
    }

    private func leftTime(_ st: EmuStatus) -> String {
        st.practicing ? Engine.timecode(forFrame: st.practiceFrame) : Engine.timecode(forFrame: st.frame)
    }

    private func rightTime(_ st: EmuStatus) -> String {
        if st.practicing { return st.practiceLength > 0 ? Engine.timecode(forFrame: st.practiceLength) : "--:--.--" }
        return Engine.timecode(forFrame: st.takeLength) + (st.unsaved ? " •" : "")
    }
}

/// 等倍 / FILL segmented control.
struct ScaleToggle: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        Picker("", selection: $model.integerScale) {
            Text("等倍").tag(true)
            Text("FILL").tag(false)
        }
        .pickerStyle(.segmented).labelsHidden().fixedSize()
        .help("等倍: 収まる最大の整数倍（くっきり） / FILL: 縦横比を保ってウインドウいっぱいに表示（⌘F で切替）")
    }
}
