# ReplayNESCore 音量差の調査・受け入れ報告

- Result: partial（FAIL）。定常波形の尺度・チャンネル比の原因は本体のミキサーで確認した。SMBの約半分という差の全量は未解明。本体修正は別ジョブに限定する指示に従って修正依頼を追記し、音量変更は行っていない。
- Changed:
  - `tests/support/AudioVolumeRom.h`
  - `tests/test_replaynescore_volume.cpp`
  - `tests/CMakeLists.txt`
  - `docs/plans/2026-10-24-replaynescore-volume.md`
  - `docs/CORE_SELECTION_PLAN.md`
  - `docs/reviews/replaynescore-volume-code.md`
  - `docs/reviews/replaynescore-volume-test.md`
  - `docs/reviews/replaynescore-volume-2.md`
- Key decisions:
  1. **本体側の確定差を修正依頼へ渡す。** `apu_mixer.hpp` のint16尺度32767に対しNestopiaは49152。pulse定数・TND式も異なる。停止triangleのDACをゼロにする本体の動作は、最終値を保持するNestopiaと異なりnoise/DMCの非線形ゲインを変える。ホストの一律増幅は採用せず、本体・adapterのstate/compat/build IDはいずれも変更していない。
  2. **SMBの全量を本体だけの責任とは断定しない。** 定常波形ではadapterはほぼ等倍だが、SMBではDC処理の違いも大きい。同一チャンネル値で式だけを揃えても不足し、式とDCを両方揃えると過大になる。DC変動、停止triangle、チャンネル遷移の相互作用の分離は別ジョブへ残す。原因を特定できていないホスト変更はしない。
  3. **既知波形9種を回帰試験にした。** 許容比0.90〜1.10を固定し、現在の不具合に合わせて緩めない。既定構成でも基準ROMの非無音・非DC・非クリップ・サンプル数を検証する。再利用は既存CTestへ集約し、新規スキル基盤は追加しない。
- Verification:
  - 以下のコマンド・実測表に記載。新コア構成の既存34件と既定構成33件は通過。追加音量試験は9種完走、RMS比8件の失敗。最終集計は新コア構成34/35通過で受け入れ未達。
- Follow-up:
  1. `docs/CORE_SELECTION_PLAN.md` の「2026-10-24 音量差の修正依頼」に従い、ReplayNEScoreリポジトリの別ジョブで尺度・ミキサープロファイル・停止DAC・ヘッドルームを設計して修正する。
  2. SMBの残差をチャンネル遷移とDC基準まで追い、必要なホスト変更の有無を改めて判断する。本体変更後は両構成の全CTestと同じSMB比較を再実行する。
  3. コミットなし（この環境では `.git` が読み取り専用）。pushは実施していない。

## 確認範囲と固定条件

指定の `replaynescore-volume-1.md` と `report-contract.md` は全文を読んだ。追加した試験・ROM生成コードも全文をレビューした。
`apu_mixer.hpp` は全文、`nrom_cpu_ppu.cpp:880–897`、Nestopiaのミキサー・triangle・音声設定、adapter/CLIの音声経路は対象関数の周辺を確認した。
広範囲の経路調査とrawプローブはサブエージェントに委譲し、その報告全文と測定ログを確認した。全関連ソースを通読したという意味ではない。

- ReplayNES: `c2656631c7ad1dc549a4b19c377daabff0e12292` を基点。
- 参照専用ReplayNEScore: `dc47d121cd39b8758294983fd6bd505f45e72ec0`。
- Release / Unix Makefiles。ビルド、生成ROM、WAV、測定ログは `/tmp/replaynes-volume-2/`。
- `sources.md` は開始時から既存変更があり、内容・ステージとも変更しない。開始時SHA-256との一致を確認する。
- 表示に関わる変更はないため、スクリーンショットは対象外。音声はWAVと数値で検証した。

## チャンネル別の原因切り分け

実 `mix_level()` を各DAC値で走査し、ローカルNestopiaの整数LUT式と比較した。他チャンネルを0に固定した最大値は以下。

|入力|ReplayNEScore PCM|Nestopia LUT|比|
|---|---:|---:|---:|
|pulse1=15|4895|7314|0.669264|
|pulse2=15|4895|7314|0.669264|
|triangle=15|8074|12557|0.642988|
|noise=15|5716|8830|0.647339|
|DMC=127|18817|27591|0.681998|

尺度比32767/49152に加え、pulseは95.88対95.52、TNDは159.79と個別分母8227/12241/22638の式対163.67・24329・`3*t+2*n+d`の近似という差がある。
入力全域の比もpulse約0.669〜0.670、triangle約0.642〜0.643、noise約0.647、DMC約0.682〜0.701で、一律倍率だけでは同じミキサーにならない。

実ROMの同一CPU slot列を使ったAC RMS（平均を引いて算出）は次のとおり。原PCMはunipolarなので、DC込みのRMSと比較しない。

|既知ROM|core直出力AC RMS|adapter後AC RMS|adapter/直出力|
|---|---:|---:|---:|
|pulse1 volume15|2447.50|2431.59|0.99350|
|pulse2 volume15|2447.50|2429.18|0.99252|
|triangle|2478.58|2476.32|0.99909|
|noise|2858.00|2797.78|0.97893|
|DMC|3466.66|3465.77|0.99974|
|pulse pair|3013.96|2995.68|0.99394|
|全混合|4220.43|4185.87|0.99181|

pulse1の同じslot値をNestopia式へ再混合し、同じadapterへ通すと3635.95となる。差はadapterより手前にある。
triangleのslot値は0〜15をほぼ均等に保持し、pulseは指定volumeと0を約50%ずつ保持するため、今回の定常ROMのシーケンサ振幅が半分なのではない。
noise/DMCの実Nestopia比較では停止triangle=15がTNDバイアスとなるので、上の「他DAC=0」比とは異なる。noiseの同一slotをtriangle=15でNestopia再混合した値3131.83は、実NestopiaのRMS3125.29に近い。

アダプタへの独立正弦波入力（振幅5000、DCオフセット10000）の出力/入力AC RMS比は55Hz=0.9401、110Hz=0.9848、220Hz=0.9971、440Hz=1.0002。
一律半減はない。ただし、これを根拠に音の開始・停止を含むSMBのDC処理の影響まで無視することはできない。

rawプローブの全9種、コード行番号、実行コマンドは `replaynescore-volume-code.md`。全混合をNestopia尺度で正のint16へ詰めた補助測定は飽和したため、そのadapter後の比は証拠から除外した。rawの混合値40105は、単純に尺度を49152へ上げるだけではint16のヘッドルームが足りないことも示す。

## 既知ROM回帰試験

ROMは外部アセンブラ・市販ROM不要の32KiB NROM。pulse1/2のvolume15・7、triangle、noise、DMC、pulse pair、全混合の9種。
各120 frames、先頭60 framesを捨てて47921 samplesを測定する。比は平均を引かないRMS。DMCはループする17 bytesで、DAC範囲は実測0〜66。
`RN_VOLUME_ARTIFACT_DIR` に出すWAVをPython標準`wave`で再読し、別計算でも比を確認する。

|ケース|Nestopia RMS|ReplayNESCore RMS|比|判定|
|---|---:|---:|---:|---|
|pulse1-15|3625.759|2431.591|0.670643|FAIL|
|pulse1-7|1845.101|1235.337|0.669523|FAIL|
|pulse2-15|3619.502|2429.182|0.671137|FAIL|
|pulse2-7|1841.919|1234.942|0.670465|FAIL|
|triangle|3849.016|2476.379|0.643380|FAIL|
|noise|3125.288|2797.779|0.895207|FAIL|
|dmc|3715.957|3465.768|0.932672|PASS|
|pulse-pair|4461.610|2997.098|0.671753|FAIL|
|mixed|8041.976|4185.873|0.520503|FAIL|

9種完走、8件の下限違反。無音・DCのみ・クリップ・サンプル数の失敗は0件。独立WAV再計算は試験出力と小数6桁で一致。全混合でも約半分を再現するため、残差はSMB固有とは限らない。

保存WAVの同周波数成分にも差がある。pulse1とtriangleの基音を混合した際の振幅低下について、起動位相・打ち消しの追加調査を依頼に含めた。
単独ROMとmixedではレジスタ書込み順が異なるため、単独ROM同士の位相差だけで原因確定とはしていない。数値と計算法は `replaynescore-volume-code.md` 末尾。

## SMB再測定（元症状の再現）

```sh
/tmp/replaynes-volume-2/on/tools/replaynes-cli/replaynes-cli dump-audio \
  'roms/Super Mario Bros. (World).NES' --core replaynescore \
  --frames 600 --press start@120+4 --out /tmp/replaynes-volume-2/smb-replaynescore.wav
# --core nestopia、--out .../smb-nestopia.wav でも同じ条件で実行
```

両方exit 0、mono / 48000Hz / int16 / 479210 samples。秒0〜8は完全な1秒区間。最後の不完全な秒9は元の表と同様に除外。ピークは11322 / 15100。

|秒|ReplayNESCore RMS|Nestopia RMS|比|
|---:|---:|---:|---:|
|0|0.000|28.660|0.000000|
|1|0.000|0.000|—（両方無音）|
|2|0.000|0.000|—（両方無音）|
|3|0.000|0.000|—（両方無音）|
|4|1589.924|2442.877|0.650841|
|5|1878.575|3655.572|0.513894|
|6|932.140|1955.303|0.476724|
|7|1498.782|3232.694|0.463632|
|8|1542.034|2987.561|0.516151|

同じCPU slotのチャンネル値で尺度・式だけをNestopiaへ交換し、さらにDCの影響も別に測った。実装は変更せず診断用の再混合・オフライン処理だけである。

|秒|式のみNestopia化|DCのみNestopia化|式とDCをNestopia化|実Nestopia|
|---:|---:|---:|---:|---:|
|4|2459.871|3037.385|4644.693|2442.877|
|5|2933.050|3743.515|5742.241|3655.572|
|6|1471.681|1639.646|2574.551|1955.303|
|7|2344.658|2672.097|4159.067|3232.694|
|8|2411.665|2549.257|3966.340|2987.561|

DC交換は元の19Hz再帰を逆算して平均後PCMを復元し、Nestopiaの整数DC再帰（約0.70Hz）へ通した。
元の再帰へ戻す独立検査ではnative/remix各479210 samplesとも最大差0・差分数0。クリップなし。
再混合側はint16飽和を避ける固定オフセット−16384を入れ、起動過渡が消えた秒4以降だけ解釈する。

式のみでは秒5〜8が不足、DCのみでは過大と不足が混在し、両方を交換すると1.29〜1.90倍になる。SMBの全量を尺度だけ、またはDCだけの問題とは言えない。
停止triangleのゼロ化などのDCステップとフィルタの相互作用は候補であり、ここでは証明していない。別ジョブでの調査を必要とする。

## 実行コマンドと受け入れ判定

```sh
cmake -S . -B /tmp/replaynes-volume-2/on -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DREPLAYNES_ENABLE_REPLAYNESCORE=ON \
  -DREPLAYNESCORE_SOURCE_DIR=$HOME/Playground/ReplayNEScore
cmake --build /tmp/replaynes-volume-2/on -j 3
cmake -S . -B /tmp/replaynes-volume-2/default -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/replaynes-volume-2/default -j 3
RN_VOLUME_ARTIFACT_DIR=/tmp/replaynes-volume-2/waves-on \
  ctest --test-dir /tmp/replaynes-volume-2/on --output-on-failure -j 2
RN_VOLUME_ARTIFACT_DIR=/tmp/replaynes-volume-2/waves-default \
  ctest --test-dir /tmp/replaynes-volume-2/default --output-on-failure -j 2
```

- Configure / build: 両構成exit 0。CMakeCacheでRelease / Unix Makefiles、ON/OFFと外部本体パスを確認。
- 既定CTest: **33/33通過**、188.92秒、exit 0。
- 新コアCTest初回: 既存**34件通過**、追加音量試験は6ケース測定後に300秒timeout、全体449.70秒、exit 8。
  既存adapter試験も259.05秒で通過。新試験は9本×120 framesを回すため、TIMEOUTを900秒へ設定して単独再実行した。RMS許容比は変更していない。
- 実行は `/tmp/replaynes-volume-2/run.py` の期限付き子プロセス管理で有限化し、期限超過時はプロセスグループを終了する。rawプローブにもframe上限と内部deadlineがある。

- 再実行: `RN_VOLUME_ARTIFACT_DIR=/tmp/replaynes-volume-2/waves-on ctest --test-dir /tmp/replaynes-volume-2/on -R '^test_replaynescore_volume$' --output-on-failure`: **9種完走、音量比8件失敗**、450.01秒、exit 8。timeoutではなくアサーションによる失敗。
- `python3 /tmp/replaynes-volume-2/finalize.py`: 9組WAVの形式・95842 samples・後半47921 samples・非クリップを確認し、試験ログと別計算のRMS/比が1e-6未満で一致。
- `python3 /tmp/replaynes-volume-2/rms.py`: SMB秒別表と既知波形9種の独立計算が上表と一致。
- 既定構成の生成ROMとrawプローブの入力ROM: 全9本バイト一致。
- `git diff --check`: exit 0。`sha256sum -c /tmp/replaynes-volume-2/sources-before.sha256`: `sources.md: OK`。
- 外部本体のHEADは開始時と同一、`git diff --stat` は出力なし。作業中に未追跡の `docs/reviews/runtime-speed-1.md` が別作業で現れたが、本タスクでは作成・変更していない。
- 管理したbuild / CTest / SMB / rawプローブはすべて終了結果を取得。継続実行するジョブは残していない。

音量の受け入れは未達。試験は不具合を検出する状態で残す。本体は参照のみという作業境界に従って停止し、未修正を成功とは扱わない。
