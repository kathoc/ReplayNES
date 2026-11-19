# ReplayArcade macOS 作業報告

判定: FAIL（未実装）

## 確認範囲

- `docs/plans/2026-11-19-replayarcade-macos.md` 全文、iPhone の製品別 XcodeGen 設定とビルドスクリプト全文、Mac の `RNProduct.swift` 全文を読んだ。Mac のプロジェクト定義は必要箇所を窓で読み、Mac と iPhone の AppModel はセッション作成付近を確認した。
- `git status --short --branch`: `trunk`、既存の FBNeo サブモジュールに変更あり。サブモジュール内の変更は保持した。
- `hostname`: `macsrv.local`。`xcodegen` は `/Users/fugudaro/.local/bin/xcodegen` にある。
- Mac のプロジェクトは Sparkle、Syphon、エンジン、アプリ、テストを含み、AppModel は NES コアと拡張子を直接指定している。製品別のエンジン登録とプロジェクト形式への変更が必要。

## 実装計画と判断

1. 既存 Mac の XcodeGen 設定を共通部分と製品固有部分に分割し、ReplayNES の生成結果の等価性を比較する。設定の複製は将来の乖離を招くため見送る。
2. iOS と同じ CMake ターゲットを macOS arm64、Release、14.0 向けにまとめ、製品ヘッダーとアーカイブを使う ReplayArcade 設定とビルドスクリプトを追加する。
3. 起動フック、コア選択、保存形式、ファイルを開く処理、タイトル、更新 UI とデータ保存先を製品プロファイルに接続し、ReplayNES の分岐を維持する。
4. 両製品のビルドと Mac テスト、Info.plist、画面および ROM とプロジェクトの保存・再読込を検証する。画面はスクリーンショットを記録する。

追加の自動化は、既存の XcodeGen と CMake のビルド経路を利用する範囲に留める。独立した新規フレームワークは、この一回の移植に対して保守費用が大きい。

## 変更と未検証

実装・ビルド・テスト・スクリーンショット・ROM の実行は行っていない。現環境の権限設定では `.git` が読み取り専用であり、依頼された trunk へのコミットは作れない。プッシュやタグ作成は行っていない。作業報告のみ作成した。コミットが可能な環境で実装から再開する必要がある。

コミット可否の確認として `git add docs/reviews/replayarcade-macos-1.md` を実行したところ、`fatal: Unable to create .../.git/index.lock: Operation not permitted`（終了コード 128）となった。報告書も未コミットである。
