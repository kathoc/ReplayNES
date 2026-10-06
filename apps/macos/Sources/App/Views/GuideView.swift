// 操作ガイド (ヘルプ menu): controller / keyboard layout, record toggle, practice mode.
// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct GuideView: View {
    private let pad: [(String, String)] = [
        ("十字キー / 左スティック", "移動（一時停止中の十字キー ←/→ はコマ戻し / コマ送り）"),
        ("B(右) / A(下)", "ファミコンの A / B"),
        ("Y(上) / X(左)", "連射 A / 連射 B"),
        ("Menu / Options", "START / SELECT"),
        ("R2（押している間）", "巻き戻し"),
        ("L2（押している間）", "早送り（録画済みの範囲だけ。終端で一時停止）"),
        ("R", "一時停止 / 再開"),
        ("L", "スロー 1/2 ⇔ 等速"),
    ]
    private let keys: [(String, String)] = [
        ("矢印キー / X / Z", "移動 / A / B"),
        ("Return / 右Shift・\\", "START / SELECT"),
        ("Delete（押している間）", "巻き戻し"),
        ("Tab（押している間）", "早送り"),
        ("Space", "一時停止 / 再開"),
        ("L", "スロー 1/2 ⇔ 等速"),
        (", / .", "1コマ戻る / コマ送り"),
        ("B", "ブックマーク追加"),
        ("⇧⌘P", "練習パネル（練習中は練習をやめる）"),
        ("⇧⌘M", "録画 / 再生 切替"),
        ("⌘F", "等倍 / FILL 切替"),
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text("操作ガイド").font(.title2.bold())
                section("コントローラー（初期設定・設定で変更可）", pad)
                section("キーボード（ReplayNES が前面のときだけ）", keys)
                VStack(alignment: .leading, spacing: 6) {
                    Text("録画ボタン").font(.headline)
                    Text("赤く光っている「録画」は録画モード。クリックすると灰色の再生モードになり、録画したテイクを再生します（終端にいたら先頭から）。もう一度クリックすると、その位置から続きを録画できる状態（一時停止）に戻ります。巻き戻してからプレイすると自動で新しいテイクに分岐し、以前の続きも残ります。")
                        .font(.callout).fixedSize(horizontal: false, vertical: true)
                }
                VStack(alignment: .leading, spacing: 6) {
                    Text("練習モード（A/B リピート）").font(.headline)
                    Text("「練習」ボタンで区間パネルを開き、区間の始まりで「A」、Aから続けてプレイした終わりで「B」を押します。▶︎ でその区間を繰り返し練習できます。Bに着くと0.5秒止まり、巻き戻るようにAへ戻って再スタートします。練習中は何も録画されず、「練習をやめる」でテイクの元の位置に戻ります。区間はプロジェクトに8つまで保存されます。")
                        .font(.callout).fixedSize(horizontal: false, vertical: true)
                }
            }
            .padding(20)
        }
        .frame(minWidth: 520, minHeight: 480)
    }

    private func section(_ title: String, _ rows: [(String, String)]) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.headline)
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                ForEach(rows, id: \.0) { r in
                    GridRow {
                        Text(r.0).font(.system(.callout, design: .monospaced))
                        Text(r.1).font(.callout).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}
