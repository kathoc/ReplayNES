# 00 RESEARCH

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
本実装前の技術調査を行え。Mesen系、Nestopia UE系、FCEUX系、libretro経由のNES core、その他有力候補を比較する。macOS arm64組込み、1-frame step、入力注入、完全savestate、determinism、reset/power、raw video/audio、license、Swift/C/C++ bridge、core build固定と旧project再生の観点で評価し docs/ARCHITECTURE_DECISION.md を作る。推奨core、module境界、thread model、最大リスク5件、先に作るPoCを示す。この段階では本体を作り込まない。
