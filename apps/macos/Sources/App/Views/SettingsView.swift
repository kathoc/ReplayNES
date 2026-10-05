// Settings: key/controller remap, hotkeys, turbo/SOCD, display/audio, general.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            BindingsTab(groups: [.player1, .player2]).tabItem { Label("ゲーム入力", systemImage: "gamecontroller") }
            BindingsTab(groups: [.hotkey]).tabItem { Label("ホットキー", systemImage: "keyboard") }
            TurboTab().tabItem { Label("連射・同時押し", systemImage: "bolt") }
            DisplayTab().tabItem { Label("表示・音声", systemImage: "display") }
        }
        .frame(width: 620, height: 520)
    }
}

struct BindingsTab: View {
    @EnvironmentObject var model: AppModel
    let groups: [InputAction.Group]
    @State private var group: InputAction.Group?

    var body: some View {
        let g = group ?? groups[0]
        VStack(alignment: .leading, spacing: 8) {
            if groups.count > 1 {
                Picker("", selection: Binding(get: { g }, set: { group = $0 })) {
                    ForEach(groups, id: \.self) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented).labelsHidden()
            } else {
                Text("ホットキーはゲーム入力とは別に処理され、記録されません。一時停止中も有効です。")
                    .font(.caption).foregroundStyle(.secondary)
            }
            List {
                ForEach(InputCatalog.allActions.filter { $0.group == g }) { action in
                    HStack(alignment: .firstTextBaseline) {
                        Text(action.label).frame(width: 190, alignment: .leading)
                        FlowChips(action: action)
                        Spacer()
                        if model.capturingAction == action.id {
                            Text("キー/ボタンを押す… (Escで中止)").font(.caption).foregroundStyle(.orange)
                        } else {
                            Button("追加…") { capture(action.id) }.controlSize(.small)
                        }
                    }
                }
            }
            HStack {
                Text("設定ファイル: ~/Library/Application Support/ReplayNES/bindings.json")
                    .font(.caption2).foregroundStyle(.secondary)
                Spacer()
                Button("初期設定に戻す") { model.input.resetToDefaults() }
            }
        }
        .padding()
    }

    private func capture(_ action: String) {
        model.capturingAction = action
        model.input.captureHandler = { id in
            model.capturingAction = nil
            if let id { model.input.bind(id, to: action) }
        }
    }
}

struct FlowChips: View {
    @EnvironmentObject var model: AppModel
    let action: InputAction
    var body: some View {
        let ids = model.inputConfig.inputs(for: action.id).sorted()
        HStack(spacing: 4) {
            if ids.isEmpty { Text("未割り当て").font(.caption).foregroundStyle(.secondary) }
            ForEach(ids, id: \.self) { id in
                HStack(spacing: 2) {
                    Text(InputCatalog.displayName(id)).font(.caption)
                    Button { model.input.unbind(id, from: action.id) } label: { Image(systemName: "xmark.circle.fill") }
                        .buttonStyle(.borderless).font(.caption2)
                }
                .padding(.horizontal, 6).padding(.vertical, 2)
                .background(Color.secondary.opacity(0.15), in: Capsule())
            }
        }
    }
}

struct TurboTab: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        let c = model.inputConfig
        Form {
            Section("連射（Turbo A / B）") {
                Stepper("周期: \(c.turboPeriod) フレーム", value: Binding(get: { c.turboPeriod }, set: { model.input.setTurbo(period: $0, duty: min(c.turboDuty, $0)) }), in: 2...30)
                Stepper("押す長さ: \(c.turboDuty) フレーム", value: Binding(get: { c.turboDuty }, set: { model.input.setTurbo(period: c.turboPeriod, duty: $0) }), in: 1...max(1, c.turboPeriod - 1))
                Text(String(format: "約 %.1f 回/秒。記録されるのは連射変換後のボタン状態なので、再生は設定に依存しません。", 60.0988 / Double(max(1, c.turboPeriod))))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("左右・上下の同時押し（SOCD）") {
                Picker("方針", selection: Binding(get: { c.socd }, set: { model.input.setSOCD($0) })) {
                    Text("両方離す（ニュートラル）").tag("neutral")
                    Text("後から押した方を優先").tag("last_wins")
                    Text("両方押す（実機では不可能な入力）").tag("allow")
                }
            }
            Section("アナログスティック") {
                Slider(value: Binding(get: { c.analogThreshold }, set: { model.input.setAnalogThreshold($0) }), in: 0.2...0.9) {
                    Text("十字キー判定のしきい値 \(String(format: "%.2f", c.analogThreshold))")
                }
            }
        }
        .formStyle(.grouped)
    }
}

struct DisplayTab: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        Form {
            Section("表示") {
                Toggle("整数倍で拡大（にじみ・ムラなし）", isOn: $model.integerScale)
                Toggle("ピクセル比 8:7（ブラウン管の見た目）", isOn: $model.displayPAR87)
                Toggle("オーバースキャンを隠す（上下 8px）", isOn: $model.hideOverscan)
                Toggle("レイテンシ計測を表示", isOn: $model.showLatency)
            }
            Section("音声") {
                Slider(value: $model.volume, in: 0...1) { Text("音量") }
                Text("一時停止・巻き戻し・シーク・スロー・早送り中は無音になります。").font(.caption).foregroundStyle(.secondary)
            }
            Section("操作") {
                Toggle("巻き戻しを離したら一時停止する", isOn: $model.pauseAfterRewind)
                Picker("自動保存の間隔", selection: $model.autosaveInterval) {
                    Text("3 秒").tag(3.0)
                    Text("5 秒").tag(5.0)
                    Text("10 秒").tag(10.0)
                    Text("30 秒").tag(30.0)
                }
            }
        }
        .formStyle(.grouped)
    }
}
