# 03 DETERMINISM

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
UI追加より先にdeterminism harnessを作る。入力scriptをframe単位で与え、複数runのmachine state/video/audio hashを一定間隔で比較。途中savestateから再開したrunと先頭からのrunも比較。不一致時は最初にdivergeしたframeとcomponentを報告。最低10000frame規模を自動test可能にする。host clock/RNG/thread依存を洗い出して排除。
