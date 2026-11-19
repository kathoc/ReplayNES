# ReplayArcade macOS 作業報告

判定: **FAIL**。実装は作業ツリーに置いたが、アプリのビルドと実行を完了できず、仕様の完了条件は満たしていない。

## 確認範囲と方針

`docs/plans/2026-11-19-replayarcade-macos.md` の全文を読み、同ファイルに採用案・見送り案・検証方針を追記した。iPhone の製品別設定とビルド手順、Mac の製品プロファイルと関連するアプリコードは担当者が必要箇所を参照し、結果を受け取った。ReplayNES の Mac 設定は共通化して製品固有部分を分け、アプリの製品差は RNProduct に従わせる。別アプリのソース複製は保守上の乖離を避けるため採用しなかった。

## 変更と確認

- `apps/macos/common.yml`, `apps/macos/project.yml`: 既存 XcodeGen 設定を共通部分と ReplayNES 固有部分へ分割。担当者が分割前後の `xcodegen dump` を構造比較し、アセットの列挙順と等価な Syphon パス表記以外は一致。両製品の `xcodegen generate` は成功。
- `products/ReplayArcade/packaging/macos/project.yml`, `.gitignore`, `Assets.xcassets/Contents.json`, `Assets.xcassets/AppIcon.appiconset/Contents.json` と同ディレクトリの PNG 10 枚: Arcade のバンドル情報、arcrec、リンク設定、起動フックおよび iPhone の原画から作成した Mac アイコン。生成 Info.plist の `plutil -extract CFBundleDocumentTypes json` は arcrec の package 型を返し、`plutil -extract SUFeedURL` はキーが存在しないと報告。実際の完成アプリの Info.plist は未確認。
- `products/ReplayArcade/scripts/build-macos.sh`: macOS 14、arm64、Release のエンジンと Xcode アプリを構築し、任意で起動する手順。`sh -n` 成功。クリーン出力から実行し、エンジンの CMake 構築と統合アーカイブ作成まで成功。`file` は ar archive、`lipo -info` は arm64、CMakeCache は Release / arm64 / 14.0 を示した。Xcode 部分は下記の権限制約で失敗。
- `apps/macos/Sources/App/AppModel.swift`: ROM・プロジェクトの種類、保存先拡張子、コア選択を製品別にした。
- `apps/macos/Sources/App/InputManager.swift`: bindings.json の Application Support 保存先を製品のデータディレクトリに変更。両製品の設定衝突を避けるため仕様上必要な一行の変更。
- `apps/macos/Sources/App/ReplayNESApp.swift`, `Views/ContentView.swift`, `Views/LibraryView.swift`: 製品名、ライブラリ表示、NES テストカートリッジと更新機能を製品別にした。
- `apps/macos/Sources/App/QuickMenu/QuickMenuPages.swift`, `apps/macos/Sources/Core/QuickMenuModel.swift`: Arcade では NES コア選択を表示せず、製品名と更新項目を切り替えた。後者には並行作業の差分も含まれる。
- 上記の Swift 変更ファイルについて `swiftc -frontend -parse` は終了コード 0。これは構文検査であり、型検査や動作検証ではない。`git diff --check` は成功。
- `docs/plans/2026-11-19-replayarcade-macos.md` に実装判断を追記し、本報告を作成した。

## ビルド・テストの結果と未検証

- `products/ReplayArcade/scripts/build-macos.sh`: CMake のアーカイブ作成後、Xcode の Sparkle SwiftPM 依存解決で `sandbox-exec: sandbox_apply: Operation not permitted`、終了コード非ゼロ。完成した ReplayArcade.app は得られていない。サンドボックス外での実行は現在の権限設定で許可されていない。
- `scripts/test-macos.sh`: 同じ SwiftPM サンドボックス権限エラーで XCTest 開始前に失敗。テストケースの成否は不明。ReplayNES の完成ビルドも未検証。
- そのため、起動、About とウィンドウ表示、ROM の映像・音・入力、録画・巻き戻し、arcrec 保存・再読込、署名と完成バンドルは未検証。アプリを起動できないためスクリーンショットも撮影できていない。ROM の実行確認も行っていない。
- エンジン・フロントエンドのソースは変更していないため、仕様でそれらの変更時に求める `cmake --build build && ctest` は実行していない。

## 作業ツリーについて

開始時から FBNeo サブモジュールに変更があり、`docs/reviews/replayarcade-macos-1.md` は未追跡だった。作業中に iOS の複数ファイル、`QuickMenuController.swift`、`QuickMenuModel.swift` に並行作業による差分が現れた。今回の担当者は iOS を編集しておらず、これらの差分は保持した。コミット・プッシュ・タグ作成はしていない。
