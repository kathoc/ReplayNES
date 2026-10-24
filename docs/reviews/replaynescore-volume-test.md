# 音量回帰試験の実装

- Result: partial（実装・測定完了、音量の受け入れはFAIL）
- Changed:
  - tests/test_replaynescore_volume.cpp
  - tests/support/AudioVolumeRom.h
  - tests/CMakeLists.txt
- Key decisions:
  1. 32 KiB PRG / 8 KiB CHR の NROM を C++ で生成。pulse1 / pulse2 は各 volume 15・7、triangle、noise、DMC、pulse pair、全チャンネル混合の 9 ケース。定常音が持続するレジスタ設定と固定 DMC データを ROM 自体に持つ。
  2. 120 フレームを両コアで実行し、先頭 60 フレームを除外。残りの RMS・絶対ピーク・平均を出力。ReplayNEScore / Nestopia の RMS 比を 0.90〜1.10 とし、失敗を非致命 CHECK で記録して全ケースを測る。フレームごとのサンプル数・無音・DC のみ・クリップも別途検査。
  3. 既定構成でも Nestopia の既知波形生成を検証する。新コア有効時のみコア間比較。`RN_VOLUME_ARTIFACT_DIR` を指定すると生成 ROM と全 120 フレームの WAV を保存でき、独立した raw コア測定にも同じ ROM を渡せる。描画は不要。
- Verification:
  - `git diff --check -- tests/CMakeLists.txt tests/test_replaynescore_volume.cpp tests/support/AudioVolumeRom.h`: exit 0
  - Release / Makeの既定構成で33/33 CTest通過。新コアありの既存34件通過。
  - 新規試験は300秒上限で一度timeout。9種の実行時間に合わせてTIMEOUT=900を設定し、単独再実行は450.01秒で完走。音量比8件のアサーション失敗、無音・DCのみ・クリップ・サンプル数の失敗なし。
  - 保存WAV9組の独立Python計算は、試験のRMSと比に1e-6未満で一致。詳細は `replaynescore-volume-2.md`。
- Follow-up:
  - コア本体の修正後に両構成を再検証する。比率失敗を隠さず修正依頼の受け入れゲートにする。

## 波形の条件

|ケース|設定|
|---|---|
|pulse1|50% duty、constant volume 15 または 7、halt length、period 253（約440 Hz）|
|pulse2|50% duty、constant volume 15 または 7、halt length、period 211（約528 Hz）|
|triangle|control=1、linear reload=127、period 126（約440 Hz）|
|noise|constant volume 15、halt length、long LFSR、period index 10（380 CPU cycles）|
|DMC|period index 15（54 CPU cycles）、loop、初期DAC=64、$c000 の17 bytesを反復|

DMC データは 4 bytes の `00` / 4 bytes の `ff` を2回繰り返し、最後に `55`。下り64・上り64のランプが2回続き、最後の8 bitsは増減を相殺する。`55` の先頭bitは64へ2を加えるため、実測DAC範囲は0〜66。飽和に依存しない。

リセット後に全 APU レジスタを明示初期化するが、無効 triangle の DAC 値を人工的に揃える操作は行わない。Nestopia の停止時保持値と ReplayNEScore の停止時ゼロ化が異なる場合、その差は noise / DMC の TND mixer bias に現れる。本試験はその実際の相違も検出対象とする。
