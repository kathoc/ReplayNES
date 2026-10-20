# ReplayNEScore を Windows / macOS / Steam Deck / iOS で動かす

2026-10-19。利用者の要望: 新コア ReplayNEScore を Windows・Mac・Steam Deck・iOS の実機で動かして試したい。
macOS と iOS のビルドは SSH で Mac に接続して行う。

## 現状（調査結果）

| 端末 | ビルド経路 | 新コア |
|---|---|---|
| Linux デスクトップ | CMake（`-DREPLAYNES_ENABLE_REPLAYNESCORE=ON`） | 配線済み・検証済み |
| Windows | Mac 上で llvm-mingw（`scripts/build-windows.sh`）、Parallels の Windows 11 VM で実行 | 未配線（`EXTRA_CMAKE_ARGS` で CMake へ渡す口はある） |
| macOS | Xcode の build phase が `apps/macos/Scripts/build-engine.sh` で CMake を呼び、固定の3 lib を link | 未配線（UI 側の選択と Live 対応は実装済み） |
| Steam Deck | `scripts/build-linux-flatpak.sh` が tree を Deck へ rsync し flatpak-builder | 未配線（manifest は ReplayNES の tree と Nestopia だけを取る） |
| iOS | `ios` ブランチ（worktree `.claude/worktrees/ios`）。`apps/ios/scripts/build-engine.sh` | 未配線。main のコア選択も未取り込み、iOS 独自 UI にコア選択なし |

ReplayNEScore は git remote も LICENSE も無いローカルの兄弟リポジトリ（`../ReplayNEScore`）。

## 方針

### 新コアのソースの渡し方

| 案 | 効果 / 難度 | 判断 |
|---|---|---|
| 兄弟 `../ReplayNEScore` を各ビルド先へ rsync し、opt-in で有効化 | 既存の流れのまま / 低 | **採用** |
| ReplayNES に git submodule として追加 | 正式 / 中 | 見送り。remote が無く、作るには外部公開が要る |
| third_party へ複製して同梱 | 単純 / 低 | 見送り。コアの更新に追従できない |
| GitHub の private repo を作って取得 | CI でも使える / 中 | 見送り（外部への作成。必要になれば利用者に確認） |

### 既定ビルドは変えない

新コアは opt-in（環境変数 `REPLAYNESCORE_DIR` が指すディレクトリがあるときだけ有効）。指定しなければ
今の配布物と同じ Nestopia のみのビルドになる。リリース手順・CI・Flatpak の公開 manifest は変更しない。

### 端末ごと（効果と難度で優先順を付けた）

1. **Windows**（難度 低）: Mac へ兄弟を rsync し、`EXTRA_CMAKE_ARGS` でコアを有効化。VM で起動確認。
2. **macOS**（難度 中）: `build-engine.sh` が `REPLAYNESCORE_DIR` を見てコアを有効化。adapter とコアの約40個の
   静的 lib は `libtool -static` で1本（`libreplaynes_extra.a`）にまとめ、Xcode はそれを1つ link する。
   無効時は空に近い stub を作り、project.yml の link 指定は常に同じにする。
3. **Steam Deck**（難度 中）: 開発用 manifest を生成し、兄弟を `type: dir` の source として追加。
4. **iOS**（難度 高）: main のコア選択（未コミットの UI 作業を含む）を `ios` へ取り込み、iOS の UI に
   コア選択と Live 制限を移植。engine は macOS と同じまとめ方。

### 見送った案

- 各 OS の Xcode / CMake に約40個の lib を個別列挙する: コアの部品が増えるたびに壊れる。
- Windows を GitHub Actions でビルド: ReplayNEScore に remote が無く取得できない。

## 完了条件

- 4 端末それぞれで、新コアを含むビルドが成功し、設定でコアを ReplayNEScore にして NROM のゲームが動く。
- 既定ビルド（`REPLAYNESCORE_DIR` なし）が従来どおり成功する。
- 各端末で画面のスクリーンショットを残す（撮れない端末はその理由を書く）。

## 記録

### 2026-10-19 Windows（Mac 上で llvm-mingw）

- 初回は clang の `-Wsign-conversion` 5件で停止（GCC は警告しない）。`uint16_t`/`uint8_t` を `<<` すると
  `int` へ整数昇格し、unsigned と `|` する所で符号変換になる。`ppu_scroll.cpp:37`、`ppu_background_pixel.cpp:36,38,40`、
  `cpu.cpp:1583`。シフト前に `std::uint32_t` へ変換すれば値を変えずに消える（試作で AppleClang `-Werror` 警告なし・
  GCC 240/240・aarch64 build 成功を確認）。**ReplayNEScore の修正は Codex の担当（2026-10-20 利用者決定）なので、
  試作は取り消し、Codex への依頼として記録する。** それまで clang 系（Windows / macOS / iOS）で新コアを有効にする
  build は `-DREPLAYNES_WARNINGS_AS_ERRORS=OFF` が要る。
- Windows VM: 新コア有効 build の試験は打ち切りまで 23 件成功・失敗 0（24 番目 `test_longrun` で時間切れ）。
  `test_replaynescore_adapter` は書き出し先に build 機のパス（`RN_TEST_TMP`）を埋め込んでいて VM で失敗したため、
  実行時の `$RN_TEST_TMP` を見る `rntest::tempDir` に変更し、VM で成功。
- macOS: `REPLAYNESCORE_DIR` を指定したときだけ `build-engine.sh` が新コアを有効にし、adapter とコアの静的 lib を
  `libtool -static` で `libreplaynes_extra.a` 1本にまとめる（無いときは stub）。既定 build で XCTest 124 件失敗 0、
  新コア有効 build はアプリに adapter のシンボル 25 個を確認。

### 2026-10-19 速度（重要）

Donkey Kong（NROM）を Linux（Release -O3）の CLI で実行すると **約 1.2 秒/frame（約 0.85 fps、実時間の約 1/70）**。
タイトル画面（frame 120）は Nestopia と video hash 一致（`440f678a1121fd13`）で、絵は正しい。

gprof（15 frame）: 時間の大半はコア内部の状態の複製と検証（`CpuPort::export_checkpoint` 1,877万回、
`RegisterWindow` 構築 2,420万回、`OamPort::valid_state`、`NromBus::export_state/restore_state` …）。
adapter の呼び出し（`PhaseScheduler::run` 134万回 = 1 dot ごと）の自己時間は小さい。
1 dot ごとに呼ぶのをまとめても数割の改善にとどまり、約70倍の差はコアの設計（各段階で状態を写して検証）に由来する。
実用速度にするにはコア側の性能作業が要る。
