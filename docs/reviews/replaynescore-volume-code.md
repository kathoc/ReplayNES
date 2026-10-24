# 音量差のコード調査（2026-10-24）

## Findings

1. `~/Playground/ReplayNEScore/include/replaynes/apu_mixer.hpp:13,41` — 正規化した非線形DAC出力を32767倍して正のint16 PCMにする。
2. `third_party/nestopia/source/core/NstApu.hpp:320` — Nestopiaの同等尺度は `NLN_VOL(192) * OUTPUT_MUL(256) = 49152`。同じ正規化値でもReplayNEScoreは約2/3になる。
3. `third_party/nestopia/source/core/NstApu.cpp:2998` — pulse LUTは95.52定数、TND LUTは163.67と24329、入力は `3*triangle+2*noise+dmc`。ReplayNEScoreのpulse95.88、TND159.79と個別分母8227/12241/22638とは式も一致しない。
4. `third_party/nestopia/source/core/NstApu.cpp:1785,1951` — triangleの起動DACは15。非活動時も最後のDAC値を保持し、TNDのバイアスになる。
5. `~/Playground/ReplayNEScore/src/nrom_cpu_ppu.cpp:888` — length/linear counterが0ならtriangle入力を0にする。Nestopiaと異なり、noise/DMCの増分ゲインにも影響する。
6. `engine/src/core/ReplayNEScoreAdapter.cpp:118,136` — コアpcmをそのままpendingSlotsに格納し、Pipelineへ渡す。`tools/replaynes-cli/main.cpp:471,477` も返却PCMをそのままWAVへ連結する。
7. `engine/src/core/ReplayNEScoreAudio.h:90` — 面積平均後に `y=x-xprev+(65372/65536)*yprev` のDCブロッカ。定数の利得調整はない。
8. `third_party/nestopia/source/core/NstApu.cpp:1245` — DCブロッカは約 `1-3/32768` の極（約0.70Hz）。ReplayNESアダプタは約19.1Hzで異なる。
9. `third_party/nestopia/source/core/NstApu.cpp:3022` — Nestopiaは混合後に三角窓平均、ReplayNESアダプタは矩形窓平均。高域差は残るが全帯域の半減ではない。
10. `engine/src/core/NestopiaCore.cpp:111` — ホストは48kHz・既定volume・追加filter無効を設定。`NstApu.cpp:459` はAPU volumeをmute/通常の二択として扱い、DEFAULT_VOLUME=85を85%として後掛けする処理ではない。

## 独立した数値確認

コアの実際の `mix_level()` とホストの実際の `Pipeline::push()` を `/tmp/replaynes-volume-code-probe.cpp` から呼び出した。これはROM動作比較ではなく、式とアダプタの分離確認。

```sh
c++ -std=c++20 -O2 -I/home/sonohoka/Playground/ReplayNEScore/include /tmp/replaynes-volume-code-probe.cpp -o /tmp/replaynes-volume-code-probe
/tmp/replaynes-volume-code-probe
```

各チャンネルのみ最大、他DAC=0とした値。Nestopia値は現物の整数LUT式をPython整数除算で再現。

| 入力 | ReplayNEScore実関数 | Nestopia整数LUT | 比 |
|---|---:|---:|---:|
| pulse1=15 | 4895 | 7314 | 0.669264 |
| pulse2=15 | 4895 | 7314 | 0.669264 |
| triangle=15 | 8074 | 12557 | 0.642988 |
| noise=15 | 5716 | 8830 | 0.647339 |
| DMC=127 | 18817 | 27591 | 0.681998 |

実ROMの単独noise/DMCでは、Nestopiaの停止triangle=15も考慮が必要。Nestopia側の増分はnoise=6399、DMC=20761になり、ReplayNEScore側がtriangle=0のままなら増分比は0.893265/0.906363。単独チャンネルROMの比を単純に尺度比と解釈しない。

アダプタへCPUスロット率39375000/22Hzで、オフセット10000・振幅5000の正弦波を2秒入力し、後半1秒のDC除去RMS比を測定。

| 周波数 | 出力/入力RMS |
|---|---:|
| 55 Hz | 0.94013414 |
| 110 Hz | 0.98477960 |
| 220 Hz | 0.99709689 |
| 440 Hz | 1.00016736 |
| 880 Hz | 1.00052650 |

結果はアダプタの定数0.5倍説を否定する。低域減衰と整数丸め差は存在する。

## Likely cause

- コアのPCM尺度32767対49152が広帯域の主要差。さらにTND式と停止triangleの扱いでチャンネル比率が一致しない。
- ただし上の測定だけではSMBの秒別0.46〜0.65を完全には説明していない。実ROMチャンネル別測定、SMB再測定との突き合わせは親担当の検証に委ねる。

## Suggested next step

既知ROMのチャンネル別RMSと上記予測を比較し、コア側の尺度・ミキサー・triangle保持について根拠付き依頼を作る。ホストへ一律2倍を足す修正は採用しない。

## Confidence

high（コード上の尺度・式・経路差とアダプタ正弦波応答）。SMB差の全量説明は未完。下記追加測定でホストDCの寄与も確認した。

確認範囲: apu_mixer.hpp と ReplayNEScoreAudio.h は全文、他の列挙コードは該当関数・周辺のみ。ユーザー資料やsources.mdは読まず変更せず。ReplayNEScore本体は参照のみ。


## 追加測定: 実ROMのdigital値・raw PCM・アダプタ

実行コード `/tmp/replaynes-volume-2/raw-probe.cpp` は、アダプタと同じ起動設定で `NromCpuPpu` を生成し、各CPUスロットの `last_cpu_audio()` を読む。各frameを最大262×341ドットに制限し、実行は最大300frame・内部90秒。今回9種は120frame実行、最初60frameを捨てた。別コア実行との時刻合わせをしない、同じtupleの再混合で原因を分離した。

```sh
c++ -std=c++20 -O2 -I/home/sonohoka/Playground/ReplayNEScore/include /tmp/replaynes-volume-2/raw-probe.cpp -Wl,--start-group /tmp/replaynes-volume-2/on/engine/experimental-core/*.a -Wl,--end-group -o /tmp/replaynes-volume-2/raw-probe
/tmp/replaynes-volume-2/raw-probe
/tmp/replaynes-volume-2/raw-probe /tmp/replaynes-volume-2/pulse1-15.nes 120
```

最後のコマンドは下表各ROM名に変えて9回実行、全exit 0。ROM生成元は `tests/support/AudioVolumeRom.h`、生成コード `/tmp/replaynes-volume-2/generate.cpp`。結果全文は `/tmp/replaynes-volume-2/raw-probe-results.txt`。RMSはこの表では平均を引いたAC RMS。

| ROM | core raw | core adapter | adapter/raw | Nest式raw（同tuple） | Nest式adapter（同tuple） | core/Nest式adapter |
|---|---:|---:|---:|---:|---:|---:|
| pulse1-15 | 2447.50 | 2431.59 | 0.993499 | 3657.00 | 3635.95 | 0.668763 |
| pulse1-7 | 1245.50 | 1235.34 | 0.991843 | 1861.00 | 1853.46 | 0.666505 |
| pulse2-15 | 2447.50 | 2429.18 | 0.992515 | 3657.00 | 3631.72 | 0.668879 |
| pulse2-7 | 1245.50 | 1234.94 | 0.991521 | 1861.00 | 1850.83 | 0.667236 |
| triangle | 2478.58 | 2476.32 | 0.999088 | 3854.80 | 3850.44 | 0.643126 |
| noise | 2858.00 | 2797.78 | 0.978929 | 4415.00 | 4321.83 | 0.647360 |
| dmc | 3466.66 | 3465.77 | 0.999743 | 5039.30 | 5038.26 | 0.687890 |
| pulse-pair | 3013.96 | 2995.68 | 0.993935 | 4503.81 | 4475.74 | 0.669315 |
| mixed | 4220.43 | 4185.87 | 0.991811 | 6295.99 | 5910.15 | 0.708251 |

注意: mixedのNest式adapterは正のint16上限を超えたため435961slotで飽和する補助経路。最終列0.708250を尺度差の証拠には使わない。mixedのNest式raw最大は40105、core raw最大は26733で、raw同士のRMS比約0.6703とnative adapter/rawは有効。SMBプローブではこの補助経路の制約を固定DCオフセットで解消した。

digital値の確認:

- pulse1/2 volume15/7は0と指定volumeだけで各約50%。規定のduty・定常volumeはこのROMでは成立。
- triangleは0..15を各約111633〜111760slot保持。半分の振幅しか出ないsequencerではない。
- noiseは0/15、約50%。DMCは0..66の偶数値。実波形は定常RMSで確認できる。
- `nest_triangle15_adapter` は0を15へ置く探索経路。停止triangleの単独noise/DMCだけ解釈できる。active triangleの0も置換するためtriangle/mixed/SMBの正しい保持モデルとしては使用しない。
- 同tuple単独DAC sweep比（ゼロ除外）: pulse1/2=0.669033〜0.670175、triangle=0.642036〜0.642988、noise=0.646694〜0.647339、DMC=0.681998〜0.700763。

## 追加測定: SMBの尺度・式・DCを分離

`/tmp/replaynes-volume-2/raw-probe-smb.cpp` は上記と同じ起動・スロット採取にStart=120..123frameを追加し、最大600frame・内部360秒に制限。600frame完走、exit 0。同tupleのNestopia整数LUT再混合では、int16入力上限の人工的飽和を避けるため16384を引いた固定DCオフセットを置く。RMS比較はオフセットの起動過渡が消える秒4以降。

```sh
c++ -std=c++20 -O2 -I/home/sonohoka/Playground/ReplayNEScore/include /tmp/replaynes-volume-2/raw-probe-smb.cpp -Wl,--start-group /tmp/replaynes-volume-2/on/engine/experimental-core/*.a -Wl,--end-group -o /tmp/replaynes-volume-2/raw-probe-smb
/tmp/replaynes-volume-2/raw-probe-smb 'roms/Super Mario Bros. (World).NES' 600 > /tmp/replaynes-volume-2/raw-probe-smb-results.txt
python3 /tmp/replaynes-volume-2/remix-dc.py > /tmp/replaynes-volume-2/remix-dc-results.txt
```

再混合PCMは `/tmp/replaynes-volume-2/smb-remixed-corefilter.raw`（48kHz mono int16、479210 samples）。`remix-dc.py` はcore native WAVと再混合PCMの両方に対して、既存DC再帰を逆にたどる。

- `delta = y[n] - trunc(65372*y[n-1]/65536)` を累積して面積平均後の入力xを復元。
- 復元xに既存DCを独立に再適用し、元yとの一致を確認。
- Nestopiaの整数DC処理と同じ `acc += delta*32768 - next*3; next=floor(acc/32768)` へ交換。
- 元出力・再混合とも479210samples、差分サンプル数0、最大絶対差0。復元x範囲はnative=0..22442、再混合=−16384..17462。どちらもクリップなし。

以下は平均を引かない秒ごとのRMS（最初の実測と同じ定義）。

| 秒 | native（19Hz） | 式のみNest化（19Hz） | DCのみNest化（0.7Hz） | 式とDCをNest化 | 実Nestopia | 式とDCをNest化/実Nestopia |
|---|---:|---:|---:|---:|---:|---:|
| 4 | 1589.924 | 2459.871 | 3037.385 | 4644.693 | 2442.877 | 1.901321 |
| 5 | 1878.575 | 2933.050 | 3743.515 | 5742.241 | 3655.572 | 1.570819 |
| 6 | 932.140 | 1471.681 | 1639.646 | 2574.551 | 1955.303 | 1.316702 |
| 7 | 1498.782 | 2344.658 | 2672.097 | 4159.067 | 3232.694 | 1.286564 |
| 8 | 1542.034 | 2411.665 | 2549.257 | 3966.340 | 2987.561 | 1.327618 |

結論の範囲:

1. 定常単独波形の差はコア内の尺度32767対49152とDAC式に帰属できる。ホストが一律0.5倍しているわけではない。
2. SMBではホストDC極の差もRMSを大きく変える。式だけの補正は秒4をほぼ揃えるが秒5〜8は不足。式とDCの両方を揃えると逆に実Nestopiaの1.29〜1.90倍になる。したがってSMBの半減全量を尺度だけ、またはホストDCだけに帰属する主張は成立しない。
3. 停止triangleを0へ落とす動作、DMCを含むDC基準、音の遷移とDCブロッカの相互作用の分離は残る。原因を特定していない段階でホストへ増幅やDC変更を採用しない。コア側の確定差と未解明のSMB残差を分けて別ジョブへ引き継ぐ。

## 保存WAVの追加確認（親担当）

全混合の実API出力RMS比は0.520503で、定常ROMでも約半分を再現した。
`python3 /tmp/replaynes-volume-2/fundamental.py` で、後半47921 samplesを
`f=(39375000/22)/(16*254)` Hzの複素正弦波へ射影した（絶対sample番号で位相を計算）。

|ROM|Nestopia 基音振幅|core 基音振幅|Nestopia 位相rad|core 位相rad|
|---|---:|---:|---:|---:|
|pulse1-15|4656.08|3113.07|1.54764|−2.53197|
|triangle|5397.07|3471.40|1.09951|1.19898|
|mixed|7622.46|1386.28|1.29207|2.62840|

単独ROM間でのtriangleとpulse1の位相差は、Nestopiaで約−0.448rad、coreで約−2.552rad（折り返し後）。
混合時の基音低下には同周波数チャンネル間の位相・打ち消しも調査対象となる。
ただし単独ROMとmixedではレジスタ書込み順が異なるため、これだけでmixed内部のチャンネル位相差を確定したとは扱わない。
次の本体側調査では両コアの同一mixed ROMでチャンネル別DAC列を採取し、停止DACだけでなく起動位相・シーケンサも照合する。
