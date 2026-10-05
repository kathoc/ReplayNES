# 02 CORE

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
ADRで決めたNES coreを統合。ROM SHA-256、power-on、1-frame step、P1/P2入力、framebuffer、PCM、soft reset、state serialize/deserializeをwrapper越しに実装。third-party licenseを記録。完了条件: state保存→100frame→復元→同入力100frameで最終hash一致。resetも同frameで再現して一致。
