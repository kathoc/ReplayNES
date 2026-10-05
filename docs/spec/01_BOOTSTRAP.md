# 01 BOOTSTRAP

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
SwiftUI中心のmacOS app骨格を作る。AppUI/Emulation/CoreAdapter/Input/Recording/Persistence/Export/Testsを分離。CoreAdapterにloadROM,powerOn,softReset,stepFrame(input),videoFrame,audioFrames,serializeState,deserializeStateを定義。まずMockCoreで60Hz相当frame counter、pause、1-frame stepを実装。UI threadをblockしない。完了条件: clean build、test、pauseでframe不変、stepで正確に+1。
