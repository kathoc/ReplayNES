# ReplayNESのコア選択と開発用実行

2026-10-18。利用者は既存ReplayNESの変更を許可し、NestopiaとReplayNEScoreを選択・切替する要件を確認した。
旧変更禁止を更新。ROM配布、他core実装の流用、既存記録の無断変換は行わない。

## 目的と完了条件

ReplayNESに新規ゲーム用core選択を実装。既存Nestopiaを既定として維持し、ReplayNEScoreを実接続する。
新coreはpulse/triangle PCMと永続snapshotを持つが、recording・soft resetが未実装なので、能力を明示したLive modeへ限定。
未実装機能を無音や架空stateで成功扱いにしない。未対応MMIOでは具体的なerrorで停止する。
既存recordingを開くときは保存済compat identityを優先し、設定で別coreへ置き換えない。

- factory/API/CLI/GUIが同じstable core IDを使用。未知値/未ビルドcoreを拒否、Nestopiaへ黙って代替しない。
- 新規選択を保存し次の新規ゲームへ適用。実行中sessionは変えず、ROM再開始で別sessionへ切替。
- Nestopia従来機能・compat ID・保存formatを維持。Mockは試験用。
- ReplayNEScoreはmapper0/現在の実装範囲で実CPU/PPU/入力を動かし、実raster境界でframe進行。
- Live modeでは録画/巻戻し/保存/Practice/オフラインexport/soft resetを明示拒否。PCMはunavailable、無音配列を生成しない。
- 単体・統合・既存回帰、CLI両core、GUI操作とスクリーンショットを検証。実機/OS未検証は明記する。

## 候補比較

|案|効果/難度|判断|
|---|---|---|
|Nestopia置換|接続容易/低|不採用、両core要件違反|
|compile時一択|工数小/低|不採用、実行時選択不可|
|core共通factory拡張|現構成に適合/中|採用|
|未知IDをNestopiaへfallback|継続しやすい/低|不採用、identityを壊す|
|native plugin ABI導入|将来拡張/高|後続、現2coreには不要|
|core subprocess分離|ABI独立/高|後続、初回には過大|
|完全APU/codec完成まで選択延期|必須契約維持/高|不採用、選択実装が遅れる|
|無音PCMを捏造|既存契約に見える/低|不採用|
|保存なしLive mode|現coreを正直に接続/中|採用|
|PracticeをLiveの代用|改修小/低|不採用、初期snapshot必須|
|input履歴をstate代用|codec不要/中|不採用、保存機能を今回広げない|
|capability API|未対応を事前拒否/中|採用|
|既存録画を選択coreで開く|選択尊重/低|不採用、再現性を壊す|
|manifest完全一致core解決|録画維持/中|採用|
|録画中hot swap|操作容易/高|不採用、state互換なし|
|新規sessionで切替|状態分離/中|採用|
|Nestopia既定|既存動作維持/低|採用|
|未ビルドcoreを非表示|UI簡潔/低|未対応理由を見せる方を採用|
|raw dot/capture接続|実画面を検証/中|採用、固定89342換算禁止|
|全OS共通UIのみ変更|工数小/低|不採用、macOSは独立UIを変更し未実機検証を明記|

## 責務とAPI合意

engine CoreKind: Nestopia=0, Mock=1を保持、ReplayNEScore=2を追加。
C API RN_CORE_REPLAYNESCORE=2、RN_MODE_LIVE=3（既存enum値を保持）。
共通能力: audio, state, soft_reset, recording。Nestopia/Mockは全部、ReplayNEScoreはaudio/state（live実行/映像/入力/powerは基礎能力）。
C API追加: rn_core_name(kind), rn_core_available(kind), rn_core_capabilities(kind), rn_session_core_kind(session)。
能力bit: RN_CORE_CAP_AUDIO=1, RN_CORE_CAP_STATE=2, RN_CORE_CAP_SOFT_RESET=4, RN_CORE_CAP_RECORDING=8。
未知core nameはnullptr、available=0、caps=0。新規createではunknown InvalidArg、未ビルドUnsupportedFormat。
CLI --core nestopia|replaynescore|mock、旧--mock維持、競合は明示error。既存project openはmanifest優先。

Core選択を次回新規ゲーム用設定としてdesktop/macOSへ追加、未搭載は理由表示。
Live sessionはprojectを作らず自動保存しない。退出時に存在しない保存を要求しない。
ただし既存sessionの未保存録画を捨てる経路は従来保護を維持。

ReplayNEScore buildはoptional CMake flagと明示source pathで接続。通常buildは既存依存だけでも成立し、
今回の検証buildでは有効化。新core adapterにC++20を局所適用。独立coreの試験はhostへ大量登録しない。
frameIndexはadapter論理frame数、power後も単調。ROMloadは0、実rasterで各frameを区切りodd skipを尊重。
動画は共通output契約に合わせ、未確証palette/raster衝突の範囲は実装担当が報告し仕様へ追記する。
音声は各frameの規定sample数を返し、core save/loadを提供する。softResetはUnsupportedFormat。
失敗frameは成功数に含めず、入力・停止errorを表示する。無制限実行はしない。

## 調査と検証入口

`reviews/host-core-switch-audit.md`、`reviews/host-core-ui-audit.md`全文を主担当が確認。
ICore必須契約はfull capability coreに適用し、Live coreの欠けた能力を明文化して拡張する。
既存CTest、追加core/session/CLI tests、実GUI scriptと画像を併用。既存specを更新し両repoに記録を残す。

## 開発adapterの画素観測policy（実装前の追記）

既存ownerのRenderDotObservationはpalette offsetまでなので、prepare時点（pre-edge）のpaletteと
PPUMASKから色codeを解決する観測fieldを追加する。6bit palette index、greyscale、emphasis3bitを保持。
CPU同slot palette/MASK writeに対するpre-edge優先は今回の明示modelで、実機衝突一致は未検証。
従来のstatic palette fixture一致と新policyの同slot試験を区別して検証する。
RGBは既存hostの共通変換を使用する方針で調査。未対応emphasisを無視して描かず、必要なら明示停止。
videoCodesはburst位相の同一性をまだ保証できないためnullptrとし、host CRTの生信号対応済みとしない。

## RGB表示profileの確定

hostに共通のcode→RGB関数は確認できなかったため、固定参照adapterへ自作uniform NROMを投入し、
全512code（emphasis8通り×palette64色）の表示RGBを測定した。各entryで全61440画素のcodeとRGB単色を検証。
生成script `tools/reference/probe_display_palette.py`、結果 `artifacts/nrom-game/display-palette-reference.json`、
host table `engine/src/core/ReplayNEScoreDisplayPalette.h`。profile `nestopia-reference-display-lut-v1`。
これは測定した表示変換表であり、他coreの実装コード流用や独立した実機RGB測定ではない。
emphasisも表で対応し、grayscaleはcode生成前に適用する。既存Nestopiaの色処理は変更しない。

## 初回ビルドで露見した既存header依存

host `video/FlashFilter.h` がsize_tを宣言に使いながら必要headerを含めず、単独翻訳単位でbuild失敗。
<cstddef>を明示includeし、宣言をstd::size_tへ修正する。描画アルゴリズムは変えない。
回帰は既存flash-filter exact試験で確認する。原因は未宣言型のinclude依存であり閾値変更ではない。

## 新規build directoryの生成競合

最初の並列Release buildで `frontend/tools/embed_data.py` が出力先generated directoryを作らず、
別generatorの実行順に依存してFileNotFoundErrorになった。各generatorが自身の出力先を用意する
責務に修正し、embed_dataに親directoryのmkdirを追加。空の深い出力先への単独実行と再buildで確認。

## 所持ROMのhost経由再現

CLI `screenshot --core replaynescore --at 120` で同じ所持NROMを投入。
master tick2134488 / PC=$FA98 / CPU write $4000 data=$00で停止、exit10。
独立runnerの既知停止と一致し、未知MMIOを黙って進めていない。
ログ `artifacts/nrom-game/host-owned-rom-stop.log`。タイトル・開始・移動の達成とは扱わない。

## 統合検証（2026-10-18）

- 標準OFF Release build成功、CTest32件の未解消失敗0。新menu icon未登録の初回失敗を既存iconへ修正しdesktop再試験成功。
- ON Release build成功、CTest33件の初回31件成功。修正前の描画fixture（visible中MASK enable）とmenu iconが失敗。
  fixtureを正しいvblank初期化と3frame captureへ修正。最終build後、adapter/core selection/frontend menu/desktop4件再試験すべて成功（10.85秒）。
  初回全件 `artifacts/core-selection/on-initial-ctest.log`、修正後 `on-final-retest.log`。未解消失敗0。
- adapter Debugの実入力/停止契約、実PPU隣接2色描画が成功。画像 `artifacts/nrom-game/replaynescore-adapter-pattern.png` を主担当も目視。
- 所持ROMはhost CLIとUIで同じ$4000停止。タイトル成功の証拠はまだない。
- 主担当が本番UI::build経由のoffscreen画像 core-japanese/core-replaynescore/core-rom-stop を目視。
  日本語表示、選択値、Live制限と停止dialogが読める。Vulkan/D3D/macOS実機の検証とは区別する。
- Release測定buildのみ既存ownerのGCC optional maybe-uninitialized警告のためWerror OFF。
  Debug strict/UBSan検証は維持し、READMEの開発core手順はDebugを案内する。

独立coreのDebug/Werror/UBSan全228試験も成功（171.99秒）。
ログ `artifacts/nrom-game/ctest-core-selection-owner.log`。新video_codeと診断API追加後の回帰結果。
共有UIのキー操作はON/OFFとも成功、Desktop61ケース成功、l10n761件/問題0。
報告 `reviews/host-core-ui-implementation.md`、画像7枚 `artifacts/host-core-ui/`。
これらの報告・artifactの正本は兄弟ReplayNEScoreのdocsに置く。ReplayNES側のこの計画は写し。

仕様との照合: core選択・Live実接続・identity保持・拒否契約・CLI・共有UI検証を完了。
本番Vulkan/D3D/macOS実機は環境制約で未検証。所持NROMのタイトル/操作、PCM、永続codec、
raw CRT burst位相は未完了として残し、次のpulse control実装へ引き継ぐ。

## 追加要件：スプライト制限解除とフラッシュ軽減

2026-10-18。利用者の追加要望を仕様へ採用。今回は仕様更新であり、以下の新規対応を実装済みとは扱わない。
所持NROM一本の起動・タイトル・入力の優先順位は維持し、この2機能を完了条件へ追加する。

### 選定

|案|効果・難度|判断|
|---|---|---|
|スプライト制限を常時解除|欠けを減らす／中〜高|不採用、実機互換と利用者の選択を失う|
|解除を切替可能にし、既定は実機制限|互換動作と表示改善を選べる／中〜高|採用|
|フラッシュ軽減をコア内部へ個別実装|コアごとの調整が可能／高|不採用、重複と録画再現性への影響を増やす|
|既存hostの表示filterを両コアへ適用|共通設定と既存試験を利用／中|採用、各描画経路の検証が必要|

### スプライト制限の解除

- 設定名は「スプライト制限を解除」、OFF/ONを変更・保存可能にする。既定OFF。
  OFFは実機相当の1走査線8枚制限、ONはその表示制限を解除する。
  OAM自体の64枚上限やROM側の描画ロジックを増やす機能ではない。
- NestopiaとReplayNEScoreの両方を対応対象とする。未実装のコアでは利用不可と理由を表示し、
  ONを受理したように見せない。対応能力をAPIで判定できるようにする。
- 新規session開始時に設定を固定する。実行中の変更は次に開始するゲームへ適用する。
  既存録画では保存済みの設定を優先し、未記録の従来録画はOFFとして扱う。
- 解除ON時もsprite priority、透明色、背景との前後関係を定義して試験する。
  sprite zero hit、overflow flag、PPU busアクセスとCPU timingを実機相当経路から不用意に変更しない。
  表示のみの拡張として分離できるかを実装前に測定・設計し、差異が残る場合は明記する。
- 表示だけの機能だと推測して既存compat IDへ混在させない。設定は録画の再現条件に含め、
  状態・映像hashへの影響を検証してcompat identity／state検証へ反映する。
  ON録画を未対応buildで開く場合は明示拒否し、OFFへ自動変換しない。

### 画面全体の激しいフラッシュの軽減

- 光の点滅による刺激を減らす目的で、全画面または広い領域の急激で反復する輝度変化・赤色点滅を軽減する。
  設定名は既存の「フラッシュ軽減」、OFF／弱／標準／強。既定は標準、変更値を保存する。
  実行中にも変更可能とし、コア切替後も選択を保持する。
- 発作防止を保証する機能とは表記せず、光刺激の軽減機能として説明する。
  「てんかん防止済み」「安全保証」といった未検証の効果を表示しない。
- 既存host `FlashFilter` と `docs/FLASH_REDUCTION.md` を基礎として両コアに適用する。
  CPU/PPU/APU、入力履歴、保存状態、検証用の未加工映像hashは変更しない表示処理とする。
- コアのRGB出力から実際の表示まで適用し、CRT有効時・raw video codes未提供時・power演出でも
  軽減処理が抜けないことを検証する。小範囲のsprite点滅、通常のスクロールや場面転換の不要な抑制を避ける。
- コア切替、新規ROM、seek/load等の不連続時はfilter履歴を既存契約に従って初期化する。
  初期frameや設定変更直後を含む限界を記録し、未検証の医学的効果を主張しない。
- 動画exportが利用可能なコアでは適用有無を選択可能にし、既存export設定を維持する。
  ReplayNEScoreのLive modeにexport機能を追加済みとは扱わない。

### 完了条件・現状

1. 両設定の保存・復元、既定値、次session適用／即時適用の違い、未対応コアの表示をUI操作と自動試験で確認する。
2. 同一走査線に9枚以上を配置した合成ROMでOFF/ON画像、priority、sprite zero hit／overflowとtimingを比較する。
   OFFは従来の状態・映像と一致することを確認し、録画の再open・state復元でも設定を維持する。
3. 全画面明暗／赤色点滅と通常映像を用い、各軽減強度の出力、OFF時の画素一致、core状態・入力・音声の不変を検証する。
   実際の表示経路で画像・時間方向の測定を残す。静止画だけでフラッシュ軽減を証明しない。
4. 制限解除の比較画像とフラッシュ軽減前後の画像、差分画素数・最大差分・時間方向の指標を報告する。
   画像を見る検証で激しい点滅を自動再生しない。数値試験の成功を医学的安全性の証明にしない。

現状: host Nestopia adapterは `EnableUnlimSprites(false)` 固定で解除設定未実装。
フラッシュ軽減の4段階設定とfilterはhostに既存。ただし今回の要件に対する両コア・全描画経路の
完了判定は未実施。仕様追加と実装・検証の完了を区別する。

## 2026-10-18 pulse control接続の継続

独立コアの `docs/NROM_APU_Pulse_Control_Plan.md` に従い、adapter constructorでpulse
controlを明示有効化する。4000/4004とquarter-envelope/length haltを扱い、他音源registerや
PCMは未対応のまま。adapter試験はcontrol write後のframe成功と4001拒否を分けて検証する。
既存Nestopia選択・録画identity・UIは変更しない。所持ROMの次停止をCLIでも照合する。

実行可能MMIOが変わるため、新コアのcompat/build IDをpulse-control-v2/live-v2へ更新する。
stable core種別IDは維持し、Nestopiaのidentityは変えない。保存codecは引き続き未実装。

pulse統合後のRelease buildとadapter/core選択2試験成功（2.85秒）。実ROMのhost CLIは
2134536/FA9B/4001=7F停止で独立runnerと一致。合成stripeは前回と差分0画素・最大RGB差0。
独立core Debug/Werror/UBSan全230試験成功（168.16秒）。UI再操作とゲーム画像は未確認。
今回の2件を前段階の全ON33/OFF32回帰の再実行とは扱わない。

## 2026-10-18 sweep/timer register接続

独立コアの4000..4007受理範囲に合わせ、adapter試験を全8registerの成功と4008拒否へ更新する。
実行可能MMIOとsnapshot stateが変わるため、compat IDを
`replaynescore/nrom-live/pulse-sweep-v3`、build IDを`experimental-nrom-live-v3`へ進める。
stable core種別、Nestopia identity、既存録画のmanifest優先は維持する。

## 2026-10-18 triangle接続

独立コアの意味付き受理範囲に合わせ、adapterでtriangle能力を明示有効化し、4008/400A/400Bの
通過と空きregister 4009の拒否を試験する。実行可能MMIOとsnapshot stateが変わるためcompat IDを
`replaynescore/nrom-live/triangle-v4`、build IDを`experimental-nrom-live-v4`へ進める。
stable core種別、Nestopia identity、既存録画のmanifest優先は維持する。

初回adapter試験はconstructorのtriangle能力引数が欠けて4008で停止した。コアの拒否条件は緩めず、
adapterから能力を明示して修正した。Release再build後、adapter/core selection 2件は成功（3.78秒）。

## 2026-10-18 PPUCTRL dot257順序とG2

描画中PPUCTRLのframe同期とdot257 horizontal copy同slot順序を実装したため、開発コアのcompat IDを
`replaynescore/nrom-live/ppuctrl-dot257-v5`、build IDを`experimental-nrom-live-v5`へ進める。
既存録画のmanifest identity、stable core種別、Nestopia identityは変更しない。

## 2026-10-18 pulse/triangle PCMとG3

独立コアのCPU-slot音声観測をframe内で整数box平均し、固定小数点DC blockerを通して48 kHz mono
signed int16を返す。ReplayNEScoreの能力bitはaudioだけを有効化し、Live sessionも直近frame PCMを
公開する。noise/DMC、analog filter一致、永続snapshotは未実装。compat IDを
`replaynescore/nrom-live/pcm-pulse-triangle-v6`、build IDを`experimental-nrom-live-v6`へ進める。

CLIへ`dump-audio`を追加し、NestopiaとReplayNEScoreの同条件WAVを採取可能にした。Start+Right
600frameの自コアWAV二本はbyte単位一致、479,210 sampleで非無音。参照もsample数は一致するが、
方式差を含むため波形比較は診断のみ。adapter/core selection 2試験成功（6.48秒）、独立コア全234試験成功。

## 2026-10-18 machine stateとG4

独立コアのrequired MACH schema 1.0 codecをadapter stateへ接続し、frame/tick/video/audio slot/DC履歴を
含む251,317 bytesの自己完結stateを実装した。ReplayNEScoreの能力bitはaudio/stateとし、recordingと
soft resetは無効のまま。compat IDを`replaynescore/nrom-live/state-v7`、build IDを
`experimental-nrom-live-v7`、state format versionを1へ進める。

所持ROMをframe 500のプレイ中に保存し、同一ROMの別instanceへ復元後100frameを同入力で継続。
各frameのcanonical state、RAM、video、PCMが完全一致した。adapter/core selection 2試験成功（9.56秒）、
独立コア全236試験成功。Sessionの録画・巻戻し・project保存UIはこのstate能力だけでは有効化しない。

## 2026-10-18 ReplayNESCore 0.2.0 / APU status read

ReplayNESCore commit `b1a3792`で`$4015` status readを追加した。pulse 2本・triangleのlength active、
frame IRQ、bit 5保持とIRQ clearを実装し、noise/DMC bitは未実装のまま0とする。実行可能MMIOと
再現結果が変わるため、compat IDを`replaynescore/nrom-live/apu-status-v8`、build IDを
`replaynescore-0.2.0+adapter-v8`へ進める。MACH schemaとadapter state envelopeは変わらないため、
state format versionは1を維持する。

adapter試験へpulse lengthをreloadして`$4015` bit 0をRAMへ保存するROMを追加する。core selection、
保存復元、PCMの既存試験も同時に通し、既存Nestopia identityと録画manifestの優先規則は変更しない。

Release統合buildで`test_core_selection`と`test_replaynescore_adapter`が2/2成功（8.63秒）。新しい
status ROM、既存pad/PCM/state/描画、Live制約を同じbinaryで確認した。

## 2026-10-18 Claude Codeによる差分レビューと再検証

- **Debug + 新core有効でリンク失敗**（`test_frontend_pacing`ほか）。原因: adapterが`stateenv::read/write`
  （`replaynes_engine`）を呼ぶのに、adapter→engineの依存がCMakeに無い。Debugではadapter.oがinline関数を
  weak定義するため、engine走査後にadapterが引き込まれ未解決になる。Releaseはinline化で偶然通っていた。
  修正: `engine/CMakeLists.txt`でadapterに`replaynes_engine`をlink（静的libの循環としてCMakeが両方を再掲）。
- **既定build（Nestopiaのみ）で`test_core_selection`失敗**。未ビルド時の能力0は仕様どおりで、試験が
  能力3を無条件に期待していた。`REPLAYNES_HAS_REPLAYNESCORE`で期待値を分け、未ビルド時はsession作成が
  失敗する（代替しない）ことも追加で検査。
- 検証: Release既定build 32/32成功。Debug新core有効build、新規3試験（core_selection/adapter/practice_return）
  単独実行で成功。Debugでの`test_determinism`/`test_flash_filter_exact`の300秒timeoutは既定buildでも同じで
  Debug速度起因（CIはRelease）。`practice_return`の`ms < 2.0`失敗は2build並列の負荷時のみ。
- 未完: Release新core有効buildは、作業中の独立コア`src/nrom_cpu_ppu.cpp:90`の`-Werror=maybe-uninitialized`
  で停止（21:01更新、検証時点でCodexが編集中）。独立コア確定後に再実行が必要。
- 未対応の指摘: 音声能力の記述が不一致（adapterはPCMを生成しCapAudioを持つが、デスクトップ/macOSはLiveで
  音声を流さず、表示とREADMEは「音声なし」）。既定buildでもSystem設定に「ReplayNEScore」が出て、選ぶと
  新規ゲームが毎回「このビルドに含まれない」で失敗する（未ビルド時は項目を隠す案）。

## 2026-10-18 ReplayNESCore 0.3.0 / APU noise

ReplayNESCore commit `73ab03c`で`$400C/$400E/$400F`、envelope、15-bit LFSR、noise length/status bit 3、TND mixer、MACH schema 2.0を追加した。adapterはnoise ownerを有効にし、合成ROMで非ゼロPCM、`$4015` bit 3、別instanceへの保存復元継続を検査する。互換IDを`replaynescore/nrom-live/noise-v9`、build IDを`replaynescore-0.3.0+adapter-v9`へ進める。adapter envelopeの構造は変えず、state format versionは1を維持する。

Release統合buildで`test_core_selection`と`test_replaynescore_adapter`が2/2成功（9.69秒）。既存のpulse/pad/描画/state試験に加え、noiseのstatus、PCM、別instance復元を確認した。

## 2026-10-18 0.3.0の音声をUIへ届ける（Claude Code）

ReplayNESCore 0.3.0（core `73ab03c`、adapter `noise-v9`）はengine側へ`e563ea3`で反映済みだったが、
デスクトップ/macOSのUIがLive modeの音声を捨てていた（`mode != RN_MODE_LIVE`で`push`しない）。
adapterはpulse/triangle/noiseの実PCMを生成しCapAudioを持つため、音声を捨てる理由はもう無い。

- 採用: Live modeでも`rn_audio`の内容をそのまま流す。音声能力の無いcoreはSessionが0 sampleを返すので、
  mode判定は不要（判定を残すと能力表と表示が食い違う、が前回指摘の原因）。
- 表示: 「音声なし」を外した3文字列（status label、設定の状態、macOSのQuick Menu）。xcstringsのkeyと日本語も更新。
- 設定の「使用するコア」は`RNF_MENU_FEATURE_CORE`で、第2coreを含むbuildだけに出す。保存済み設定が
  nestopia以外なら、未ビルドでも直せるよう表示を残す。macOSも同じ条件。
- 見送り: capability bitでUI側の音声を分岐する案（Sessionがすでに0 sampleを返すので重複）。
- 試験: `test_desktop_frontend`のLive試験とoffscreen UIの音声sinkを「0 sampleであること」から
  「sampleが届くこと」へ変更。menu試験は機能flag 0でCore行が無いことを確認。

検証: Release既定 32/32、Release新core有効 33/33（core側は`REPLAYNES_WARNINGS_AS_ERRORS=OFF`、下記）。
offscreen UIを新core有効/無効の両buildで実行しPASS、設定画面・Live画面・日本語画面を目視確認。

残課題:
- 新core有効のReleaseはcore側既定（警告をerror扱い）だと`ReplayNEScore/src/nrom_cpu_ppu.cpp:679`の
  `current->`で`-Werror=maybe-uninitialized`停止。`require(current.has_value())`が戻らないことをGCCが
  知らない誤検知と見られる。Codexの0.3.0検証もOFFで通していた。core側で直す。

- `engine/src/core/ReplayNEScoreDisplayPalette.h`が未追跡のまま、commit済みadapterがincludeしている。
  次のcommitに含めないとclean checkoutで新core有効buildが通らない。
- macOSの`build-engine.sh`はadapter/core libをlinkしないため、macOSは常にNestopiaのみ。
- macOS（Mac実機、Xcode 27.1、作業ツリーを`~/tmp/replaynes-core-check`へ複製）: XCTest 124件中0失敗・1skip。
  l10n検査でCodex追加の未使用key `Core availability`を検出し削除、再検査0件。macOS UIの目視は未実施。

## 2026-10-19 Release + 警告error扱いでの停止を解消（Claude Code）

- 再現: GCC 11.4、`-O3 -Werror`で`ReplayNEScore/src/nrom_cpu_ppu.cpp`が`-Wmaybe-uninitialized`停止。
  独立コア単体でも`-fPIC`の有無に関係なく再現（ReplayNES統合固有ではない）。Codexの検証はReleaseを
  常に`REPLAYNES_WARNINGS_AS_ERRORS=OFF`で回していたため未検出だった。
- 原因: 665行の`std::optional current`を`require(current.has_value() && …)`の後で`current->`で読む。
  `require`は失敗時にthrowするので実際は空にならないが、GCC 11はoptional payloadの初期化を追えずに警告する。
  `current.value()`への置換では消えなかった。
- 修正: raster位置を全経路で初期化される値`raster`へ写し、以降はそれだけを読む（空なら既定値だが、直後の
  `require`が従来と同じmessageで失敗するので挙動は不変）。変更はtimed MASK書き込みの1か所。
- 検証: ReplayNES新core有効Release（警告error ON）build成功・33/33。独立コアRelease 240/240。
  delayed MASK成功経路は`mask_delay=12`の各試験とsnapshot試験が通る。拒否経路の専用試験は無い。
- 別件で発見: 独立コア単体のRelease + 警告errorは、試験2本（`tests/nrom_cpu_ppu_mmio.cpp:35`、
  `tests/nrom_cpu_ppu_apu_pulse.cpp:102`）の`vector::insert`でGCC 11の`-Wstringop-overread`誤検知が出て止まる。
  ReplayNESはcoreの試験をbuildしないので統合には影響しない。Codex側で対応。
- コミット: 独立コアの上記修正は作業ツリーからCodexが`94a9b62`として取り込み済み（内容同一）。
  ReplayNES側は`ccb48ac`（未追跡だったpalette header）と`db5bdd9`（`FlashFilter.h`の`<cstddef>`）。
  後者はclean checkoutで判明: GCC 11.4では既定buildでも`FlashFilter.cpp`が`size_t`未宣言で失敗していた。
- clean checkout検証: ReplayNES `ccb48ac`＋`FlashFilter.h`修正と独立コア`c0e6cfa`を別の場所へ取り出し、
  新core有効Release（警告error ON）build成功・33/33。
