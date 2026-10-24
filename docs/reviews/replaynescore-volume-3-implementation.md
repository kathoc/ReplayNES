# 0.9.1 同期・ホスト音量尺度の実装報告

- Result: done（実装・音声単体試験。ROM統合測定は親担当で継続）
- Changed:
  - `engine/src/core/ReplayNEScoreAdapter.h`
  - `engine/src/core/ReplayNEScoreAudio.h`
  - `tests/test_replaynescore_adapter.cpp`
  - `tests/test_replaynescore_audio.cpp`
- Key decisions:
  1. Compat を `replaynescore/nrom-live/accuracy-c5-v17`、build を `replaynescore-0.9.1+adapter-v17` とする。stateFormat/payload schema は構造不変のため3を維持する。外部本体の統合文書67行でも同じ方針が推奨されている。コアMACH 6.4は外部コアの保存復帰処理が担当する。
  2. audio profile は1から2へ更新。DC再帰・面積平均・保存するDC状態の尺度は維持し、出力にだけ49152/32767を掛ける。64bit整数で対称最近接丸め（中間値はゼロから遠い側）、最後にint16範囲へ飽和する。正のunipolar入力を先に増幅しない。実ROMのヘッドルーム・飽和数は親担当の測定で評価する。
  3. 既存面積積分oracleの出力尺度を独立した浮動小数計算へ更新。全65536通りのDC出力で丸め・境界・飽和と帰還状態を検証し、旧identity/profileの原子的拒否を追加する。音量9種のROM・閾値は変更していない。
- Verification:
  - `c++ -std=c++20 -O2 -Itests -Iengine/src tests/test_replaynescore_audio.cpp tests/support/rn_test_main.cpp -o /tmp/replaynes-volume-3/audio-unit && /tmp/replaynes-volume-3/audio-unit`: exit 0、6 cases・失敗0。
  - `git diff --check`: exit 0。
- Follow-up:
  - 親担当による両構成CTestと9種ROM・SMB測定。飽和が観測される場合はヘッドルームを再設計する。

確認範囲: 計画書追加節を含む全文、0.9.0同期コミット2件の差分全文、ReplayNEScoreAudio.hと音声試験の変更前全文、Adapter.h全文、Adapter.cppの保存復帰周辺、adapter試験のidentityと状態改変周辺を読んだ。外部本体はapu_mixer.hpp全文、統合文書の版と互換性節のみ参照した。本体とsources.mdは変更していない。

## 追加設計調査: 初期DACとDC履歴の整合（未実装）

確認範囲は `ReplayNEScoreAdapter.cpp:44–176` のboot、フレーム採取・出力、softReset全文と、外部コアのPCM生成・RESET・last_cpu_audio周辺。初期コアは未実行なのでboot直後のlast_cpu_audioへ依存しない。

最小案はフレーム終端の最初の `pushFrame` 直前に、`!audioPipeline.state().hasInput` のときだけDC初期値を `pendingSlots.front()` に揃えること。既存Stateを複製してdcInputだけを設定し、restoreする。Pipeline一般のpush/pushFrameやDC再帰を変更せず、初めて実コアPCMを消費するadapterの境界で起動時の定常DCを設定する。8074のハードコードは不要。

- boot/loadROM/powerCycleはPipelineを空へ戻すため、次の最初のフレームだけ設定する。無音JMP ROMなら保持DACの定常値から偽の起動パルスを作らない。
- softResetはPipelineを空にしない。再生開始後のRESETで履歴を再初期化して実際のDAC変化を隠さない。最初のframeより前のRESETではpendingSlotsの先頭（RESET中最初の実PCM）で一度だけ初期化される。
- 保存復帰はhasInput・dcInputとpendingSlotsがすでに含まれる。初回frame前のsnapshotなら復元後に同じ先頭PCMで初期化し、再生済みsnapshotなら保存したDC履歴を使う。追加boolやschema変更は不要。
- 全経路のスロット採取でprimeする方式は通常frameとRESETの2箇所へ分岐を増やし、hasInputを先に立てるか別flagを追加する必要があるため見送る。
- 検証は初回frame前/RESET直後の保存復帰一致、静止JMP ROMの初回frame完全無音、既存hard powerとPCM再現性、およびmixedの全区間clip数再測定を使う。一般PipelineのDC step試験は変更しない。

採用は診断で原因を確認した後とし、この節の時点ではコード未変更。

## 初期DC履歴の実装

親担当の診断結果（mixed起動29clip、初期DC整合の途中計測で0clip）と計画追記を受け、上の最小案を実装した。
`ReplayNEScoreAdapter.cpp` の最初のpushFrame直前で、hasInputがfalseの場合に限りpendingSlots先頭の実PCMをdcInputへ設定する。空配列を先に拒否し、restoreの結果を確認する。
初回出力前の無操作JMP ROMを、RESETなし・RESETありの両方で保存復帰し、初回PCMの完全無音・サンプル数・別instanceのPCM/state一致を検証するケースを追加した。追加実行量は合計4 frames。Pipeline一般の試験・演算・state layoutは変更していない。

- Changed（追加）: `engine/src/core/ReplayNEScoreAdapter.cpp`, `tests/test_replaynescore_adapter.cpp`
- Verification: `git diff --check` exit 0。更新後の統合試験と全区間clip測定は親担当で実施する。
