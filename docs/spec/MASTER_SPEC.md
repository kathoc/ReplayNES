# NES No-Miss Recorder MASTER SPEC

## Goal
macOSでNES/Famicomを低遅延プレイし、動画ではなく決定論的なエミュレーション履歴を記録する。ミス時は任意の過去へ戻り、そこから再録画する。最終takeを再生すると失敗部分のない連続プレイとなり、音声付きMP4へオフライン出力できる。

## Core principle
論理時間は整数 frameIndex。各フレーム境界でP1/P2入力とsystem eventを確定してcoreを正確に1 frame進める。
再生identity = ROM SHA-256 + core ID/build + serialization version + region/timing + machine config + initial state + ordered frame input/event stream。
テトリス等のPRNGもCPU/RAM/PPU/APU/mapperを含む完全状態と同一入力系列から再現する。host clock、host RNG、thread schedulingをゲーム結果へ混入させない。

## Recording
正本は「coreへ実際に渡した最終入力bitfield」。毎フレームまたは変化点としてP1/P2入力、soft reset、power cycle等を記録する。連射設定そのものではなく、連射変換後の入力を記録する。

## Checkpoints
完全savestateを一定間隔とbookmark時に保存。seek時は目的frame以前の最寄stateをloadし、input logを高速再実行して目的frameへ到達。stateは高速化用であり履歴の正本ではない。stateにはframe、core compatibility ID、checksumを持たせる。

## Branching
rewind後に再録画したら新branchを作る。旧試行は即削除せずundo/recovery用に保持。内部はimmutable segmentのDAGを許容し、active takeはroot→leafの一本の経路。通常UIではDAGを複雑に見せない。

## Persistence
`.nesrec` package:
manifest.json
timeline/index.json
timeline/segments/*
states/*
metadata/bookmarks.json
journal/*
ROM本体は原則含めずpath/referenceとSHA-256を保持。同hash ROMを再指定可能。
manifestにはformatVersion、core ID/build、state serialization versionを保存。atomic write、journal、checksum、crash recoveryを実装する。

## Core compatibility
プロジェクト作成時のcore互換IDを固定する。アプリ更新でcore/state形式が変わる場合、既存projectを無条件変換しない。旧coreを同梱/選択できる設計または明示的migrationを検討し、再現性を最優先する。

## Controls
外部controller: GameController.framework。keyboard対応。remap可。hotkeyとゲーム入力を分離。controller disconnect時はpause。
pause / frame advance / slow 1/2, 1/4 / rewind / scrub / bookmark。
制作時のpauseやslowは最終takeの時間に含めず、最終再生は正規速度。

## Turbo
frame単位のperiod/dutyでA/B等を連射可能。Input Pipeline: physical input -> mapping -> turbo/SOCD policy -> final NES bitfield -> recorder/core。

## Reset
Soft ResetとPower Cycleを別system eventとしてframe単位記録。リセットを攻略に使うゲームを再現可能にする。

## Low latency
プレイ時にencodeしない。emulation、render、audio、UIの責務を分離。Metal表示、低めのaudio buffer、入力sampleから次frameへの経路短縮。latencyは計測する。「ゼロ遅延」は目標にしない。

## Audio
coreのAPU PCMをリアルタイム再生。pause/rewind/scrubは原則mute。slow時も初期版はmute可。exportでは正規速度PCMを使用。

## MP4
active takeをheadless/offlineで最初から再実行しvideo frameとPCMを生成。AVAssetWriterでH.264/HEVC + AAC。timestampはemulated timeから生成しwall clock非依存。UI frame dropはexportへ影響させない。nearest-neighbor整数拡大、overscan、pixel aspect設定を用意。

## Determinism tests
同じROM/state/inputを複数回再生し、一定間隔でmachine-state hash、framebuffer hash、audio hashを比較。先頭からの再生と途中savestateからの再生も一致させる。市販ROMをrepoに入れず合法的test ROM/fixtureを使う。

## UI
game viewport、record/play状態、frame/time、pause/play、frame-step、slow、rewind、timeline scrubber、bookmark、reset、controller status、take/branch、export。高度なbranch管理は別panel。

## Non-goals v1
ROM配布、ROM改変、netplay、チート、Lua automation、高度なTAS input editor、ライブ動画切り貼り方式。
