# ReplayNESCore 0.9.1 統合・ホスト音量尺度の検証

- Result: partial（FAIL）。0.9.1同期・ホスト尺度統一・起動クリップ修正は完了。音量9種中mixedだけが位相差で不合格のため、新コア全CTest通過の条件は未達。
- Changed:
  - `engine/src/core/ReplayNEScoreAdapter.cpp`
  - `engine/src/core/ReplayNEScoreAdapter.h`
  - `engine/src/core/ReplayNEScoreAudio.h`
  - `tests/test_replaynescore_adapter.cpp`
  - `tests/test_replaynescore_audio.cpp`
  - `tests/test_replaynescore_volume.cpp`
  - `docs/plans/2026-10-24-replaynescore-volume.md`
  - `docs/CORE_SELECTION_PLAN.md`
  - `docs/reviews/replaynescore-volume-3-implementation.md`
  - `docs/reviews/replaynescore-volume-3-residual.md`
  - `docs/reviews/replaynescore-volume-3.md`
- Key decisions:
  1. 本体19abd41（0.9.1）のnesdev正確式、32767 unipolar、停止triangle DAC保持、APM4/mixer v4、MACH 6.4を維持して統合。ホストはDC除去後だけ49152/32767を掛ける。DC再帰へ倍率後の値を帰還させない。
  2. compat/buildをv17、audio profileを2へ更新。構造不変の公開state format/payload schema 3とaudio state version 2は維持し、旧compat/profileを拒否する。整数演算は64bit、対称最近接丸め、最後にint16飽和。ROM実測の飽和数を別記する。
  3. 9種の既存ROM・RMS比0.90〜1.10は変更しない。音量試験には起動から全PCMのクリップ検査を追加した。残差の切り分けは診断用コピーで行い、倍率やミキサー式を再調整して差を隠さない。再利用は既存CTestと本報告の測定手順に集約する。
- Verification: 以下のコマンドと実測表。
- Follow-up:
  1. mixedのpulse起動dividerについて実機根拠を確認し、外部本体とNestopiaのどちらを修正すべきか別作業で決定する。本体参照のみ・基準ROM/閾値維持の条件下では、ホスト尺度変更でこの位相差を解消しない。
  2. 切替時の周波数特性も一致させる場合は、測定済みの0.70Hz DC処理をホストの別仕様として検討する。今回の19Hz維持によるSMB残差は残る。
  3. コミットなし。環境の `.git` は読み取り専用で、利用者の「書けなければコミット不要」に従った。pushなし。

## 確認範囲と境界

指定の `replaynescore-volume-2.md`、`replaynescore-volume-code.md`、`replaynescore-volume-1.md`、既存音量仕様書、report-contractは全文を読んだ。CORE_SELECTION_PLANは音量差依頼節とidentityの関連箇所を確認した。
実装担当の報告全文、今回の実装・試験差分全文、Adapterの保存復帰箇所、Nestopiaのミキサー・DC処理周辺を親担当が確認した。外部本体の全ソースを通読したという意味ではない。
位相の診断と同tuple再混合は調査担当へ委譲した。親担当は報告全文、tuple-probe.cpp、prepare-probes.py、analyze-residual.py、analyze-smb-channels.pyの全文、出力JSONを読み、DC逆算を別スクリプトでも再計算した。

本体は `/home/sonohoka/Playground/ReplayNEScore` の `19abd413fa9984ca9300bd063d9d3e1ea24511ee` を参照のみ。開始時から本体に未追跡 `docs/reviews/runtime-speed-1.md` がある。
ReplayNESは `c265663` 基点、音量試験・ROM・前回調査文書等の既存未コミット変更を保持する。`sources.md` は編集・ステージしない。
ビルドとPCM・診断出力は `/tmp/replaynes-volume-3/`。画面描画の変更はなく、検証対象は音声のためWAVと数値を提示する。pushなし。

## 実行コマンド

```sh
cmake -S . -B /tmp/replaynes-volume-3/on -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DREPLAYNES_ENABLE_REPLAYNESCORE=ON \
  -DREPLAYNESCORE_SOURCE_DIR=$HOME/Playground/ReplayNEScore
cmake --build /tmp/replaynes-volume-3/on -j 3
cmake -S . -B /tmp/replaynes-volume-3/default -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/replaynes-volume-3/default -j 3
RN_VOLUME_ARTIFACT_DIR=/tmp/replaynes-volume-3/waves-final \
  ctest --test-dir /tmp/replaynes-volume-3/on --output-on-failure -j 3
RN_VOLUME_ARTIFACT_DIR=/tmp/replaynes-volume-3/waves-final-default \
  ctest --test-dir /tmp/replaynes-volume-3/default --output-on-failure -j 3
/tmp/replaynes-volume-3/on/tools/replaynes-cli/replaynes-cli dump-audio \
  'roms/Super Mario Bros. (World).NES' --core replaynescore \
  --frames 600 --press start@120+4 --out /tmp/replaynes-volume-3/smb-final.wav
# --core nestopia、--out /tmp/replaynes-volume-3/smb-nestopia.wav でも実行
python3 /tmp/replaynes-volume-3/measure.py
python3 /tmp/replaynes-volume-3/dc-analysis.py
```

実際の長時間コマンドは `/tmp/replaynes-volume-3/run.py` の期限付き子プロセス管理を通す。configure 120秒、build/SMB 600秒、CTest default 900秒/on 1200秒で、期限超過はプロセスグループを終了する。CTest各試験の既存TIMEOUTも有効。診断プローブは内部期限とフレーム上限を持つ。権限設定によりsandbox外の監視はできないため、子プロセス終了コードの回収で管理する。

## 最終SMB測定（volume-1と同じ区間）

600 frames / start@120+4、mono 48000Hz int16、両方479210 samples。最後の不完全な秒9は前回と同様に除外する。以下は平均を引かないRMS。

|秒|0|1|2|3|4|5|6|7|8|
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
|ReplayNESCore RMS|0.000|0.000|0.000|0.000|2196.334|2570.918|1286.821|2112.365|2141.640|
|Nestopia RMS|28.660|0.000|0.000|0.000|2442.877|3655.572|1955.303|3232.694|2987.561|
|比|0.000000|—|—|—|0.899077|0.703287|0.658119|0.653438|0.716852|

SMBのcore範囲は−12555〜14763、Nestopiaは−12552〜15100。全479210 samplesのクリップは両方0。
初期DC整合前のSMBは秒0 RMS764.576だったが、修正後は0。秒1以降431210 samplesは最大差0・差分数0で、曲の尺度・DC特性は変更していない。
Nestopiaの秒0はsample 0の−6279だけが非ゼロで、残り47999 samplesは0。継続的なBGM差ではなく起動時の1sample成分である。

## 既知ROM9種の最終測定

各120 frames、後半47921 samplesのRMS。全95842 samplesのクリップを別途検査する。

|ケース|Nestopia RMS|ReplayNESCore RMS|比|判定|
|---|---:|---:|---:|---|
|pulse1-15|3625.759|3647.680|1.006046|PASS|
|pulse1-7|1845.101|1853.258|1.004421|PASS|
|pulse2-15|3619.502|3644.015|1.006773|PASS|
|pulse2-7|1841.919|1852.672|1.005837|PASS|
|triangle|3849.016|3714.776|0.965124|PASS|
|noise|3125.288|3053.911|0.977161|PASS|
|dmc|3715.957|3860.324|1.038851|PASS|
|pulse-pair|4461.610|4495.833|1.007671|PASS|
|mixed|8041.976|6279.018|0.780780|FAIL|

**8/9種通過。mixedのみ下限違反。** 全9種・両コアの起動を含む全区間でクリップ0。無音・DCのみ・サンプル数の異常は0件。Python標準waveによる再読と独立RMS計算がCTestログと1e-6未満で一致し、生成ROMは初回実行と9本ともバイト一致した。

|対象・全区間|サンプル数|最小|最大|クリップ数|
|---|---:|---:|---:|---:|
|全混合 / replaynescore|95842|-20054|27415|0|
|全混合 / nestopia|95842|-24511|28091|0|
|SMB / replaynescore|479210|-12555|14763|0|
|SMB / nestopia|479210|-12552|15100|0|

全混合の正側ヘッドルームは32767−27415=5352。今回の全混合・SMBで飽和に依存しないことを確認した。任意のROM・任意の過渡に対する非飽和保証まで測定したものではない。

## 起動時のヘッドルーム

倍率だけを追加した初回版ではmixed全95842 samplesのうち29 samplesが正のrailへ達した。定常区間47921 samplesでは0件であり、検査範囲の拡張で発見した。
保持triangle=15の起動PCM8074に対し、ホストのdcInput=0が人工的な起動ステップを作り、その減衰過渡が混合音へ加算されるのが原因。
同じ入力で初期dcInputだけ8074へ合わせる診断はmin−20054/max27415/クリップ0となり、定常後半は最大差0・差分数0だった。

製品実装は8074をハードコードせず、初回pushFrame前に最初の実PCMでdcInputを初期化する。hasInputが保存されるため、hard power、初回出力前RESET・保存復帰、再生済みRESETを区別でき、追加の状態フィールドは不要。
起動過渡を除く設計とし、任意のリミッタ・倍率低下・フェードは追加しない。一般Pipelineのステップ応答も維持する。最終出力の飽和定義自体は残すが、対象ROMでクリップへ依存しないことを全区間で検証する。


## 残差の原因と採否

### 全混合: pulse起動dividerによる位相差

同一mixed ROMのミキサー入力DACを分離すると、triangle−pulse1の位相差はcore −2.602517rad、Nestopia −0.503781rad。
Nestopiaのpulseは初期timerが2048 cycles、本体は初期dividerが0で、このROMでは同周波数のtriangleとの打ち消し量が異なる。
`/tmp` の診断Nestopiaでpulse reset timerだけ2 cyclesへ変えると、位相差は−2.606917rad、混合RMSは8041.976→6144.274となり、coreとの比は1.021930になる。
同じcoreのDAC列をNestopia式へ再混合したRMSは6241.346で、正確式6279.018との差は約0.6%。式の違いや尺度だけでは約22%の不足を説明できない。

単一要因の介入で位相と混合RMSの両方を再現した。ただし、どちらの起動状態が実機に適切かは本測定では判定していない。本体参照のみという境界を守り、NestopiaやROMを変更して試験を通すことはしない。
診断Nestopiaの変更はSMB全479210 samplesに対して最大差0・差分数0で、全混合の原因をSMBへ一般化する仮説は棄却した。

### SMB: DCフィルタ特性とミキサー式の差

同一CPU slotの音源値を使い、式とDCをそれぞれ交換した診断結果。DC初期化前の出力を使用するが、以下は起動過渡の消えた秒4以降。

|秒|正確式＋既存19Hz|Nest式＋19Hz|正確式＋0.70Hz|Nest式＋0.70Hz|実Nestopia|
|---:|---:|---:|---:|---:|---:|
|4|2196.334|2261.927|2408.885|2464.330|2442.877|
|5|2570.918|2663.746|3633.211|3667.619|3655.572|
|6|1286.821|1347.161|1880.350|1975.348|1955.303|
|7|2112.365|2194.092|3146.782|3257.295|3232.694|
|8|2141.640|2223.846|2905.725|3007.908|2987.561|

倍率の逆写像と再倍率が全479210 samplesで一致し、DC再帰の逆算と元再帰への戻しも最大差0・差分数0を確認した。core直結プローブと実adapter CLIの出力も初期化前は全sample一致。式交換は比較用で、製品には採用しない。
DCだけ合わせるとRMS比0.962〜0.994、式も合わせると1.003〜1.010。前回の1.29〜1.90倍の過大は停止triangle修正後には再現しない。
チャンネルを分離した別測定でも、DMCの秒5は19HzでRMS19.050、0.70Hzで1180.716、Nestopiaは1181.863となり、遅いDAC変動が19HzのDC除去で落ちることを確認した。

既存のDC周波数特性を変更することは再生尺度変更とは別の仕様変更になる。本タスクでは19Hzを維持し、残差の原因と0.70Hz候補の実測を記録する。正確式と近似式の差も消さない。
noise分離値には3〜7%の残差があり、初期LFSR・窓平均の差などの寄与は未分離。総混合で式・DCを合わせた残差は約1%で、主要な音量不足とは区別する。
詳細なコード箇所、診断版作成コマンド、各チャンネルの数値は `docs/reviews/replaynescore-volume-3-residual.md`。


## 最終検証と完了条件の照合

- Configure/build: 両構成exit 0。最終再buildもexit 0。CMakeCacheでRelease / Unix Makefiles、ON＋指定本体パス / OFFを確認。
- 新コア最終CTest: **34/35通過**、411.07 sec、exit 8。失敗は `test_replaynescore_volume` のmixed RMS下限だけ。adapter（旧state拒否、初期DC無音、RESET・保存復帰を含む）とaudio oracleは通過。
- 既定最終CTest: **33/33通過**、110.98 sec、exit 0。追加した全区間クリップ検査を含む。
- 初回の新コアCTestは34/35、538.53秒、音量試験8/9だった。起動クリップを独立全区間検査で発見して初期DC仕様を修正し、最終構成で全件を再実行した。失敗をWILL_FAIL、除外、閾値変更で隠していない。
- SMB最終dump-audio: 600 frames / 479210 samples、exit 0。Pythonによる形式・秒別RMS・全区間クリップ・初期化前後比較を通過。
- `python3 /tmp/replaynes-volume-3/measure.py`: exit 0。9組WAVの独立再計算とログ一致、全区間クリップ0、生成ROM一致を確認（mixedの合格を意味しない）。
- `python3 /tmp/replaynes-volume-3/dc-analysis.py`: exit 0。coreプローブ/CLI一致、倍率・DCの往復で全479210 samplesの最大差0・差分数0、診断DC交換の非クリップを確認。
- `git diff --check`: exit 0。`sha256sum -c /tmp/replaynes-volume-3/sources-before.sha256`: sources.md OK。外部本体HEADは19abd41、tracked diffなし、既存未追跡文書1本のみで開始時と同じ。
- 起動したbuild/CTest/SMBの終了コードを回収し、診断担当のプローブもすべて終了済み。継続ジョブなし。

仕様との差異は、全混合の位相差により9/9・新コア全CTest合格に達していないこと。仕様の閾値は変更せず未達を残した。SMBは再測定と原因分離まで完了し、未分離の小さなnoise差とフィルタ仕様差を明示した。

