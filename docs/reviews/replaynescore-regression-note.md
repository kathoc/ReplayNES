# 描画オフ音声回帰試験の追加

- Result: done
- Changed: `tests/test_replaynescore_adapter.cpp`
- Key decisions:
  - PPUMASKへ一度も書かない試験ROMを620フレーム実行し、描画オフの29780 2/3 CPUサイクルを継続させる。ホストの描画省略とは区別する。
  - 各フレームの音声数・累計数と非無音を確認し、600フレーム後から別インスタンスへの復元結果をPCMとmachineHashで比較する。
  - 610フレーム目のsoft reset直後にも保存・復元し、RESETが生成したCPU slotとフィルタ履歴の保持を検証する。識別子期待値はadapter-v16 / stateFormatVersion 3に更新。
- Verification:
  - `git diff --check`: 成功。
  - ビルド、新旧実装での実行、全ctestは主担当が受け入れ確認として実施する。

## 実装差分の静的レビュー

- Result: partial
- Findings:
  - `engine/src/core/ReplayNEScoreAdapter.cpp:249`: pending slot数は上限・lastSlot以下しか検査せず、machineのフレーム内進行との不整合を受理する。例えば1フレーム実行後の正常stateにpending slotを7個挿入してenvelopeを再生成すると、frame/phase/cursorとmachine stateは正常なままloadに成功するが、次のstepFrameで29781 slot上限を越える。soft reset直後の7 slotを除去する破損も同様に見逃す。
- Key decisions:
  - `ReplayNEScoreAudio.h`の変更箇所と周辺全文、Adapterの差分およびload/softReset周辺を確認。整数積の上限、幅の総和、連続phase/area/DC、cursor overflow事前判定に欠陥は見つからなかった。
  - 追加試験は実フレーム620回、別個の絶対区間積分oracle、保存復元、RESET直後復元をカバーする。上記の範囲内pending数とmachineの不整合は未検査。
- Suggested next step: pending数をmachineのraster進行に照合し、範囲内の挿入・除去でもload元を変更せず拒否する回帰試験を追加する。
- Confidence: high（分岐条件に基づく静的確認。破損stateでの実行再現は主担当へ依頼済み）

## 指摘修正後の再レビュー

- Result: done
- Changed: `tests/test_replaynescore_adapter.cpp`
- Key decisions:
  - loadStateがrenderの実PPU dot累計と現在raster位置からフレーム開始時のCPU slot数を求め、pending数と照合する修正を確認した。省略dotは前フレーム末尾で発生するため、現在フレームのraster位置をそのまま引ける。上記指摘は解消。
  - core側の`nrom_cpu_ppu.cpp`ではrender処理のdot累計更新とrestore時の整合条件のみを確認し、`ppu_dots`が省略dotを含まない物理dot数であることを確かめた。外部coreファイルは変更していない。
  - pending 0個の正常stateに7個挿入する場合と、soft reset直後の7個を削除する場合の試験を追加した。正常stateのload成功、不正stateの期待理由による拒否、拒否前後の保存バイト列一致を検証する。
- Verification:
  - `git diff --check`: 成功。
  - ビルド・実行は主担当へ引き継ぎ。state envelopeにはCRCがないため長さを再生成し、内部machine snapshotとそのchecksumを無変更で保持した。
