# ブラウン管 (CRT) 表示: nesterm からの移植

ReplayNES の「表示 → ブラウン管 (CRT)」は、nesterm（kathoc, MIT）の Web 版に同梱された物理 CRT モデル
「CRT（物理モデル・実験）」を Metal に移植したものです。nesterm の `vendor/crt/` は作者の研究プロジェクト
crt-physical-model から `EXPORT-MANIFEST.json`（commit `919c156329dae4f2a832460fb3726a5f09c20213`, dirty）で
書き出されたコードで、移植元はこのスナップショットです。新規設計ではなく、各 WebGL シェーダー・CPU 処理を
1 対 1 で対応させ、定数・式・評価順・既定値をそのまま使っています。

モデルは nesterm 自身が明記しているとおり**未校正の実験モデル**で、実在のテレビ（National 等）の再現ではありません。
台帳 ID（M1-*, M3-*, M4A-* など）はソースのコメントに残しています。

## パイプライン（nesterm の配信モードと同じ順序）

`web/physical-worker.mjs` の `frame` 処理（source = `pixels`）:

```
PPU コード (9bit) ─ RF/IF (FFT overlap-save, 1025 タップ複素 FIR) ─ 受信機 (クランプ・バースト・同期) ─ AGC
  ─ [走査線 110〜220 の仮想実験] ─ [高圧電源/ABL] ─ [水平スポット] ─ 管面 (スロット + 検出器 + 散乱)
  ─ [蛍光体残光] ─ sRGB 表示
```

ReplayNES ではすべて 1 つの Metal コマンドバッファ内の compute パスで、新しいエミュレーションフレームを受け取った
描画コールバックで符号化し、その直後に `present` します（ワーカー往復・フレームキュー・GPU→CPU 読み戻しなし）。

## 対応表

| nesterm (ファイル / 関数・シェーダー) | ReplayNES (ファイル / 関数・カーネル) |
|---|---|
| `vendor/crt/adapters/nesjs.mjs` `installCodeTap` (PPU 9bit コード + 行開始サンプル) | `engine` `rn_video_indices` / `rn_renderer_video_indices`（`NestopiaCore::stepFrame` が描画と同時に `Ppu::GetScreen()` と `GetBurstPhase()` をコピー）、`CRTComposite.rowPhases(burstPhase:)` |
| `reference/composite.mjs` `signalFixture`, `signalVoltage` | `CRTModel.swift` `CRTComposite`（`voltageLUT` = rf-webgl の `voltageTexture`） |
| `reference/rf.mjs` `designIF`, `noiseSigma`, `carrierToNoiseDb`, `hash32`, `noiseKey`, `gaussianAt` | `CRTRF.designIF/noiseSigma/carrierToNoiseDb/hash32/noiseKey`、MSL `lowbias`/`gaussAt` |
| `backends/rf-webgl.mjs` `kernelSpectrum`, twiddles | `CRTRF.kernelSpectrum`, `CRTRF.twiddles` |
| rf-webgl `initCodes` | MSL `rf_init_codes`（`rfInitValue`） |
| rf-webgl `butterfly2` / `butterfly` ×12 段, `multiply`, `extract` | MSL `rf_butterfly2` / `rf_butterfly` / `rf_multiply` / `rf_extract`（直訳版）と、同じ演算をスレッドグループメモリで行う `rf_forward_tg` / `rf_inverse_tg`（既定） |
| `backends/receiver-webgl.mjs` `reduction` | MSL `rx_reduce` |
| receiver-webgl `process()` の CPU 側 AGC ループ（`readPixels` 後に `GatedAGC.advance` を 240 行） + `reference/agc.mjs` | MSL `rx_agc`（1 スレッドで同じ行ループ・同じゲート条件・同じ制御則を GPU 上で実行） |
| receiver-webgl `prepare`, `decode` | MSL `rx_prepare`, `rx_decode` |
| `backends/raster-webgl.mjs` `RasterWebGL`（走査線数の面積積分、合成 RGB は sRGB 復号） | MSL `raster_area` |
| `backends/supply-webgl.mjs` `meanProgram/rowProgram/stateProgram/resampleProgram` + `reference/supply.mjs` 定数 | MSL `supply_mean/supply_row/supply_state/supply_resample`、`CRTSupply` |
| `backends/spot-h-webgl.mjs` + `reference/spot-h.mjs` `extraSigmaSamples` | MSL `spot_h`、`CRTTube.extraSigmaSamples` |
| `reference/tube.mjs` `createTubePlan`（`detectorMaps`, `growthTables`, 環境光 `renderRawTube(powered:false)`, 散乱カーネル） | `CRTTube.makePlan`（CPU、出力サイズ変更時のみ・バックグラウンド） |
| `backends/tube-webgl.mjs` `packMaps`/`packGrowth`/`colsum` | `CRTTube.Plan`（同じ並び） |
| tube-webgl `hprog`, `vprog`（固定／成長）, `sprogs[0/1]`, `mixprog`, `litprog`, `persistprog`, `showprog` | MSL `tube_h`, `tube_v` / `tube_v_growth`, `tube_scatter`（直訳）と `tube_scatter_x16/_y16`（既定）, `tube_mix`, `tube_lit`, `tube_persist`, `show_fragment` / `show_kernel` |
| tube-webgl `setPersistence`, `persistenceWeights`, slots（M4B-GAP） | `CRTRenderer.encode` の slot 管理, `persistenceWeights()` |
| `reference/phosphor.mjs` `frameFractions` | `CRTPhosphor.frameFractions` |
| `web/physical-worker.mjs` `configure`/`reset`/`frame`（段の順序・効果の既定値） | `CRTRenderer.configure` / `reset` / `encode` |
| `web/physical-preview.mjs` `displaySize`（CSS×DPR を 4:3・256〜1600 に） | `CRTRenderer.tubeSize(forDestination:)`、`GameRenderer.crtViewport` |
| `web/index.html` / `web/app.mjs` の CRT 設定（成長・残光・電源・電波 20〜90 dBµV・走査線） | `App/CRTSettings.swift`（設定 → 表示・音声 → ブラウン管 (CRT)、`@AppStorage` で保存） |

既定値（nesterm の出荷時と同じ）: 走査線 240、明るい所ほど走査線が太る = オン、蛍光体の残光 = オン、
明るい画面で幅が広がり暗くなる = オン、電波の強さ 65 dBµV、環境光 40 lx（nesterm の UI に項目なし）、
サンプル数 4、ノイズ seed 1。nesterm にはプリセットがないため、ReplayNES も「nesterm の既定値に戻す」だけです。
CRT 表示そのものの既定はオフ（従来のくっきり表示）。

## 入力と決定性

- nesterm は @nesjs/core の PPU ビットマップ（色 6bit + 強調 3bit）と、PPU クロックを数えた行開始サンプル位置を
  使います。ReplayNES は Nestopia の `Video::Screen`（同じ 9bit 形式: `(色 & greyscale) | 強調<<6`）を
  `rn_video_indices` で公開しました（表示専用の追加 API。状態・ハッシュには含まれず、`tests/test_render.cpp` で検証）。
- 搬送波位相: nesjs のタップは `8 × PPU ドット数 mod 12` を使います。Nestopia の colour-burst phase `b` は
  89342 ドットのフレームで +1、奇数フレームのドット省略（89341）で +2 進むので `b ≡ −ドット数 (mod 3)`、
  すなわち `8 × ドット数 mod 12 = 4b`（原点の違いは nesterm の `phaseOrigin: 'assumed-relative'` の範囲）。
- フレーム番号（残光の履歴と RF ノイズの鍵）はコアのフレーム番号です。巻き戻し・シークなどで番号が増えないときは
  受信機・電源・残光の状態をリセットします（nesterm の `reset` と同じ扱い）。
- フラッシュ低減がそのフレームを変更した場合は、安全のため低減後の RGB 画像を nesterm の「合成 RGB」入力
  （`COMPOSE-ASCII-RGB`: 512×240、sRGB 復号、RF なし）として管面へ送ります。通常のフレームは PPU コード → RF 経路です。
- MP4 書き出し・Syphon も同じ `CRTRenderer` を使います。同じフレーム列からは同じ画像になります
  （`CRTTests.testSameFrameSequenceIsBitIdentical`）。

## 意図的な差分（正直な一覧）

1. **AGC を GPU で実行**: nesterm は受信機の統計を `readPixels` で CPU に戻し、`GatedAGC`（倍精度）を回して
   ゲインを再アップロードします。ここでは同じループを 1 スレッドの Metal カーネル（単精度）で実行し、
   読み戻しをなくしました。参照との差は 1e-6 程度。信号消失（振幅 < 1e-12）の行があってもフレームを捨てません
   （nesterm はそのフレームを表示しない。バーストは常に合成されるため実際には起きません）。
2. **パケット検証・ギャップ上限を省略**: nesterm は 1 秒を超えるサンプルギャップや重複パケットを例外にしますが、
   ReplayNES では早送り・巻き戻しが普通にあるため、ギャップは無視（AGC はゲート無効時に変化しないので結果は同じ）、
   番号が戻ったらリセットします。
3. **速度のための再構成（数値は同一）**: FFT の 12 段をスレッドグループメモリで、散乱（約 90 タップ）を
   レジスタ窓で計算します。演算・順序・重みは同じで、直訳版とビット単位で一致します
   （`testOptimizedKernelsAreBitIdenticalToDirectPort`）。
4. **表示の配置**: 管面は常に 4:3（nesterm と同じ幾何）。「ピクセル比 8:7」は CRT 表示では使いません。
   「オーバースキャンを隠す」は管面の上下 8/240 を切り取ります。等倍/FILL は通常表示と同じ高さに 4:3 で合わせます。
5. **内部解像度の上限 1600×1200**: nesterm の OUTPUT-RESOLUTION-SPEC（256×192〜1600×1200、検出器の作業量上限）と
   同じです。全画面などそれより大きい表示先へは、表示パスで線形光のまま双一次補間で拡大します
   （nesterm ではブラウザがキャンバスを拡大）。
6. **未移植**: nesterm の「TV（従来モデル）」（`web/tv-raster.mjs` / `tv-signal.mjs`）と ASCII 表示は対象外です。
   数値精度は WebGL と同じ単精度（残光リングは nesterm 同様 half float）。

## 検証

- `apps/macos/Tests/CRTTests.swift` + `CRTConformance.swift`: nesterm の CPU 参照モデル
  （`AGCReceiver`, `receiveRF`+`decodeLine`(ノイズ付き), `renderTube`, `evaluateGrowthPlan`+`mixTubeOutput`,
  `rasterRowWeights`, `SupplyState`+`applySupply`(3 フレーム), `applySpotH`, `frameFractions` による残光）で生成した
  `tests/fixtures/crt/reference.json` と Metal 出力を比較。生成: 
  `NESTERM_CRT=<nesterm>/vendor/crt node tools/crt-reference/generate-fixtures.mjs`。
  M1 Max での最大誤差: 受信機 1e-6、管面 2e-7、電源 3e-5（25 kV の漸化式を単精度で）、残光 2.4e-4（half）。
- 実 ROM のフレーム（スーパーマリオブラザーズ）を nesterm の CPU 参照チェーン（受信機→電源→水平スポット→管面成長）で
  512×384 に描いたものと Metal 版を 8bit で比較し、最大差 1/255（平均 0.13）。
- `tests/test_render.cpp` "video indices": コードと RGB の対応・状態ハッシュ不変・セッションと書き出しの一致・
  モックコアは未対応を返すこと。

## レイテンシと GPU 時間（M1 Max, 32-core GPU, 120 Hz 内蔵ディスプレイ）

アプリ内の既存計測（エミュ完了→表示、`--snapshot` の JSON、SMB を 12 秒自動再生）:

| 条件 | エミュ完了→表示 | 表示 fps | 表示 GPU 平均 / 最大 |
|---|---|---|---|
| 通常表示・ウインドウ（等倍） | 34.2 ms | 60.0 | 0.15 / 0.17 ms |
| CRT・ウインドウ（等倍, 管面 1600×1200） | 37.2 ms | 61.0 | 7.9 / 11.1 ms |
| 通常表示・全画面 FILL | 31.3 ms | 60.0 | 0.17 / 0.32 ms |
| CRT・全画面 FILL（管面 1600×1200 → 拡大） | 30.9 ms | 60.5 | 7.3 / 11.3 ms |

GPU 単体（連続実行、クロック最大）: 管面 1600×1200・全効果オンで 4.0 ms/フレーム（受信機 0.75 ms）。
60 fps のペースでは GPU のクロック制御により同じ処理が 8〜12 ms になります。再構成前（直訳カーネル）は
同条件で 6.6 ms / 2400×1800 では 16.9 ms でした。
