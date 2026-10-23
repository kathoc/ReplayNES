# ReplayNESCore continuous 48 kHz audio plan

## 目的

ReplayNESCore adapterのframeごとのbox平均を、NTSC CPU clock `39375000/22 Hz`から48 kHzへ連続する整数面積resamplerへ置き換える。frame境界、block分割、save/loadで位相を切らず、`audioSamplesForFrame(frame)`が要求する個数を過不足なく返す。現行DC blockerは係数の実機根拠が未確証なので、新しいanalog特性とは呼ばず、version付きの既存互換profileとして状態と演算順を固定する。

## 要件

- CPU slot PCMを一定保持された区分値として扱い、入力1 slot幅を`22*48000=1056000`、出力1 sample幅を`39375000`の共通整数時間単位で積分する。
- stateはprofile version、phase、area、直近入力、入力有無、出力cursor、DC input/outputを含む。
- 出力平均は符号対称の最近接丸め、DC blockerは既存どおり`y=x-xPrev+trunc(65372*yPrev/65536)`、その後int16 saturateとする。
- frame境界でstateをresetしない。frame APIは`audioSamplesBefore(frame+1)-audioSamplesBefore(frame)`個だけ返し、生成数の不足・過剰をerrorにする。
- soft resetはresampler/filter historyとcursorを保持し、公開済みframe audioだけclearする。hard power/loadROMは既定stateへ戻す。
- adapter payloadへ全stateを保存し、不正profile、phase、area、cursor、入力値、filter historyを候補instance上で原子的に拒否する。
- envelope schema、compat ID、build IDはこの単位では変更しない。payload内部は現行schema 1のまま拡張し、主担当の同期時に一括更新する。

## 検討案

|優先|案|効果|難度|判断|
|---:|---|---|---:|---|
|1|共通整数時間単位の面積accumulator|連続・bit決定的|中|採用|
|2|有理phaseだけ持ち各出力点を最近傍sample|軽量|低|不採用。area平均要件を満たさない|
|3|frameごとのbox平均を維持|変更最小|低|不採用。位相が毎frame切れる|
|4|浮動小数phase/area|実装容易|低|不採用。platform決定性が弱い|
|5|64-bit signed area|最大値に十分、保存容易|低|採用|
|6|128-bit area|overflow余裕|中|見送り。1出力区間で64-bitに収まる|
|7|入力PCMをslot区間の先頭値として保持|core観測と直結|低|採用|
|8|入力をslot境界間で線形補間|高域が滑らか|中|不採用。core契約に根拠がない|
|9|出力生成をframe終了後に一括処理|既存構造に近い|中|不採用。block分割stateを隠す|
|10|CPU slot受信ごとに逐次処理|任意分割と同一|中|採用|
|11|出力を無制限queueへ保持|切出し容易|低|不採用。異常を隠しメモリ上限がない|
|12|frame内bufferへ生成し終了時に個数を厳密照合|API一致、異常検出|低|採用|
|13|不足時に最後のsampleを複製|音切れ回避|低|不採用。不足を隠す|
|14|過剰時に次frameへ繰越queue|連続性維持|中|不採用。frame時刻不一致を隠す|
|15|現DC係数を無名のまま維持|変更最小|低|不採用。state identityがない|
|16|現DC係数をprofile v1として固定|互換挙動を明示|低|採用|
|17|新high/low-pass段を同時追加|実機らしい可能性|高|不採用。係数根拠がない|
|18|soft resetでfilterをclear|単純|低|不採用。連続出力を切る|
|19|hard powerだけ全historyをclear|fresh instance一致|低|採用|
|20|独立整数oracleをtest内に実装|実装写しの危険|中|採用。ただし時間区間列を直接算出し、component内部helperを使わない|
|21|frame 798/799列を短区間だけ確認|高速|低|採用。ただし長時間cursor試験も追加|
|22|state破損をCRCだけに任せる|試験が少ない|低|不採用。意味範囲を検証する|

## 採用する状態と遷移

`phase`は現在の出力区間に積分済みの時間幅で`0 <= phase < 39375000`、`area`はその符号付き面積とする。入力slotごとに幅1056000を分割して現在の出力区間へ加算し、phaseが39375000へ達するたびに平均を丸め、profile v1 filterへ渡し、output cursorを1増やす。入力値はstate検証と継続契約のため保存する。slot処理完了時には入力区間を持ち越さない。

## 完了条件

- 独立整数oracleとimpulse、step、DC、交互値が一致する。
- 1 slotずつと複数block投入の出力・全stateが一致する。
- save/load前後、soft reset前後で連続し、hard powerはfresh instanceと一致する。
- frame APIが少なくとも798/799系列を`audioSamplesForFrame`どおり返し、不足・過剰を成功扱いしない。
- payload破損はload先を変更せず拒否する。
- adapter対象testと全ReplayNES testを通す。

