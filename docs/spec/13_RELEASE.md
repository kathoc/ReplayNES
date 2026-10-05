# 13 RELEASE

# Prompt Pack
Codex CLI / Claude Code等で段階的に実装するためのプロンプトです。00から順に一つずつ実行してください。一度に全プロンプトを渡さないでください。

各prompt共通ルール:
- 最初にrepo、README、MASTER_SPEC、既存testを読む。
- 作業前に変更範囲/変更しない範囲/test方法を短く提示。
- 今回のscope外を先回りして大量実装しない。
- build/testを必ず実行。
- errorを隠すfallback禁止。
- host時刻、非決定RNG、raceをemulation結果へ混入させない。
- ROM/著作権物をrepoへ追加しない。
- 完了時に変更内容、test結果、残課題を報告。

## 今回のタスク
配布準備。Apple Silicon優先のRelease build、署名/notarization手順、third-party notices、license遵守、privacy/network不要ならその明記、sample projectは著作権ROMを含めずtest ROMで作る。READMEに基本操作、巻き戻し、翌日再開、MP4 export、core互換性注意を記載。既知の制限を隠さない。
