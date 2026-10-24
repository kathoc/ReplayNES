# 0.9.1 音量残差の分離測定

- Result: done（mixed の位相原因とSMBの主要残差を分離。受け入れ判定は親報告）
- Changed: `docs/reviews/replaynescore-volume-3-residual.md`
- Key decisions:
  1. 本体19abd41と製品Nestopiaを変更しない。同一mixed ROMのミキサー入口を分離し、単独ROM同士の起動位相から推定しない。
  2. Nestopia reset timerの変更は `/tmp` の診断版だけ。基準試験を通すための製品変更には採用しない。
  3. 同tupleのNestopia式は32767/49152へ戻してから現Pipelineへ渡し、ホスト倍率の二重適用とunipolar int16飽和を避ける（量子化差は残る）。
- Verification: 以下のコマンドと表。
- Follow-up: mixedの本体／Nestopia起動divider仕様の比較と、priming候補の製品実装・回帰確認は親担当へ引継ぎ。

## 確認範囲

指定レビュー2本と `AudioVolumeRom.h` は引継ぎコンテキストの全文を確認。`apu_mixer.hpp` と `apu_triangle_control.cpp` は全文、`apu_pulse_control.cpp` はlevel/clockと書込み処理、Nestopia `NstApu.cpp` はSquare/TriangleのReset・WriteReg・GetLevel・Advance、Oscillator::Reset、MixLevel/WalkSpan周辺を読んだ。関連ファイル全体を通読したという意味ではない。

## 全混合の残差: pulse 起動dividerによる位相差

同じ既知mixed ROMを両コアで120 frames実行し、最初60 framesを除く47921 samplesを比較した。診断版Nestopiaは `MixLevel` の返却値だけを指定チャンネルのDAC×128へ変更。ReplayNEScoreは各CPU slotのDAC×128を同じadapterへ通した。ホストの倍率が異なるため絶対振幅は比較せず、同一コア内のtriangle−pulse1位相差を見る。

基音は `(39375000/22)/(16*254)` Hz、絶対sample番号の複素正弦波へ射影した。

|測定|pulse1位相 rad|triangle位相 rad|triangle−pulse1（折返し）rad|
|---|---:|---:|---:|
|ReplayNEScore 0.9.1|−2.525329|1.155339|−2.602517|
|製品Nestopia|1.547644|1.043863|−0.503781|
|診断Nestopia: pulse reset timerのみ2 cycles|−2.632405|1.043863|−2.606917|

`third_party/nestopia/source/core/NstApu.cpp:1450` のOscillator::Resetは `RESET_CYCLES * fixed`、`NstApu.hpp:432` のRESET_CYCLESは2048。Square::Resetはfrequencyを2 cyclesにするがtimerは2048のまま。対して本体pulseの初期timer_dividerは0で、以後2CPU cyclesごとに進む。ROMが起動直後に周期を設定するため、この初期divider差が残ったまま同周波数のtriangleと混ざる。

介入は `/tmp/replaynes-volume-3/NstApu-reset.cpp` のSquare::Resetへ `timer = frequency` を1行足すだけ。最初のpulse上昇は製品Nestopia sample55、診断版sample19、core sample18で確認した（異なる窓平均による端の1sample差は残る）。triangle出力は同一。これにより位相差がcoreと約0.0044radまで近づき、全混合RMSも下記のとおり変わる。

|同一mixed ROMの混合出力|後半RMS|coreとの比（core/対象）|
|---|---:|---:|
|ReplayNEScore正確式＋現adapter|6279.018145|1|
|同core tupleをNestopia近似式へ再混合＋現adapter|6241.346499|1.006036|
|製品Nestopia|8041.976（既存試験値）|0.780781|
|診断Nestopia: pulse reset timerのみ変更|6144.274395|1.021930|

近似式への交換だけでは0.6%しか動かず、約22%の不足を説明できない。reset timerの単一介入が位相とRMSの両方を揃えるため、mixedの主要残差はホスト尺度ではなく両コアのpulse起動divider差である。どちらの起動状態が実機に適切かは本測定では決定しない。テストの基準やROMを変更せず失敗を残す。

## SMBへの適用範囲

同じ診断NestopiaでSMB600 frames / start@120+4を実行したところ、製品Nestopiaとの全479210 samplesで差分サンプル数0、最大差0。mixedの起動divider原因をSMBにも適用する説明は棄却する。

## 再現コマンド・成果物

```sh
python3 /tmp/replaynes-volume-3/prepare-probes.py
c++ -std=c++20 -O2 -I$HOME/Playground/ReplayNEScore/include \
 /tmp/replaynes-volume-3/tuple-probe.cpp -Wl,--start-group \
 /tmp/replaynes-volume-3/on/engine/experimental-core/*.a -Wl,--end-group \
 -o /tmp/replaynes-volume-3/tuple-probe
python3 /tmp/replaynes-volume-3/run.py 500 /tmp/replaynes-volume-3/mixed-probe.log \
 /tmp/replaynes-volume-3/tuple-probe /tmp/replaynes-volume-2/mixed.nes 120 \
 /tmp/replaynes-volume-3/mixed-tuple
RN_PROBE_CHANNEL=0 /tmp/replaynes-volume-3/nestopia-probe dump-audio \
 /tmp/replaynes-volume-2/mixed.nes --core nestopia --frames 120 \
 --out /tmp/replaynes-volume-3/mixed-nest-0.wav
python3 /tmp/replaynes-volume-3/analyze-residual.py
```

すべてexit 0。tupleプローブには600 frames上限、各frame262×341 dots上限、内部480秒のdeadline、外側500秒のプロセスグループ終了を設けた。Nestopiaの診断コピーは本ビルドと同じflagsでコンパイルし、コピーしたstatic archiveのNstApu objectだけ差し替え、本CLI objectとリンク。製品archiveを上書きしていない。

`mixed-tuple-{0..4}.raw` はpulse1/pulse2/triangle/noise/DMC、5はnative PCM、6は同tupleのNestopia再混合。診断後半の位相・RMSは `mixed-residual.json`。画面に関わる変更なし、音声波形と数値を検証した。

## 起動過渡の飽和とpriming候補の独立測定

後半だけでなく全sampleを調べると、未primingのmixed出力95842 samplesはmin−20054/max32767、rail sample29件。起動時点で停止triangle=15のunipolar DCが8074あるのに、DCフィルタの前入力が0から始まるため、起動ステップが残る。

診断プローブのnative用Pipelineだけ初期 `dcInput=8074` をrestoreし、同じ120framesを実行した。結果はmin−20054/max27415、rail sample0件。後半47921samplesは未priming版との最大差0・差分数0、RMS6279.018145。尺度や定常波形を変えずに初期DCだけを除けることを確認した。8074は0.9.1の `mix_level(0,0,15,0,0)` に対応し、任意に選んだリミッタ閾値ではない。

```sh
python3 /tmp/replaynes-volume-3/run.py 500 /tmp/replaynes-volume-3/mixed-prime.log \
 /tmp/replaynes-volume-3/tuple-prime /tmp/replaynes-volume-2/mixed.nes 120 \
 /tmp/replaynes-volume-3/mixed-prime
```

exit0。`tuple-prime.cpp` は同一tuple-probeの初期stateに上記DC入力を設定しただけ。製品コードへの反映と回帰試験は親担当に引き継いだ。


## SMB: 停止triangle修正後に残るDC周波数応答差

600 frames / start@120+4を同tupleプローブで完走（内部480秒/外側500秒、exit0）。nativeと再混合は各479210 samples、rail sampleはいずれも0。native範囲−12555〜14763、再混合範囲−12879〜14942。

|秒|正確式＋19Hz|Nest式＋19Hz|正確式＋0.70Hz|Nest式＋0.70Hz|製品Nestopia|
|---:|---:|---:|---:|---:|---:|
|4|2196.334|2261.927|2408.885|2464.330|2442.877|
|5|2570.918|2663.746|3633.211|3667.619|3655.572|
|6|1286.821|1347.161|1880.350|1975.348|1955.303|
|7|2112.365|2194.092|3146.782|3257.295|3232.694|
|8|2141.640|2223.846|2905.725|3007.908|2987.561|

ホスト倍率の丸め写像は非飽和域で単射なので、`round(output*32767/49152)` でDC後整数を復元し、再倍率が全sample一致するassertを通した。その後19Hzの再帰を逆算して0.70Hzへ交換し、最後に49152/32767を再適用した。再混合経路はslot時点でNest式を32767/49152へ戻しており、量子化差は厳密な同一性を保証しない。

DCだけを合わせると正確式のままでもNestopiaの0.962〜0.994倍、式も合わせると1.003〜1.010倍になる。前回の1.29〜1.90倍という過大出力は0.9.1では再現しない。停止triangleを0へ落としていた差がなくなった後のSMBの大きな残差は、19Hz対0.70HzのDCブロッカによる低域・DC遷移の減衰で説明できる。正確式とNestopia近似式の差を意図せず消す変更は採用しない。

### 同じSMBのチャンネル別分離による独立確認

各チャンネルDAC×128を個別に出し、core出力のホスト倍率を逆算し、両方0.70Hzへ揃えたRMS比を計算した。単独チャンネルROMへの置換ではなく同じゲーム内実行の同じチャンネルを観測した。

|チャンネル|秒4|秒5|秒6|秒7|秒8|
|---|---:|---:|---:|---:|---:|
|pulse1|1.001691|1.001558|1.000921|0.998783|1.007801|
|pulse2|1.002623|1.002379|0.992956|1.001176|1.005380|
|triangle|0.999679|1.000486|0.999347|1.000891|1.000762|
|noise|1.041960|1.048240|1.044386|1.070370|1.032016|
|DMC|1.003646|0.999030|0.994105|0.990276|—（両方0）|

DMCの秒5では19Hz出力RMS19.050→0.70Hzへ交換後1180.716、Nestopia1181.863。DMC DACの遅い変動がホストDCで落ちる寄与を直接確認した。triangleも秒6で212.467→348.016、Nestopia348.244。tone系の差は約1%以内まで縮む。noiseの3〜7%差は残り、異なる初期LFSR・矩形窓対三角窓等が候補だが、この表だけで各寄与の全量を断定しない。総混合で式・DCを揃えた残差は約1%であり、元の30%程度の不足とは分離できる。

```sh
python3 /tmp/replaynes-volume-3/run.py 500 /tmp/replaynes-volume-3/smb-tuple.log \
 /tmp/replaynes-volume-3/tuple-probe 'roms/Super Mario Bros. (World).NES' 600 \
 /tmp/replaynes-volume-3/smb-tuple
python3 /tmp/replaynes-volume-3/analyze-smb-channels.py
python3 /tmp/replaynes-volume-3/analyze-smb-remix.py
```

全exit0。測定結果は `smb-channels.json` と `smb-channels.log`。担当したtuple/primeプローブと診断Nestopiaの全ジョブは終了結果を取得済み。
