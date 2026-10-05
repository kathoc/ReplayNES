# 08 TAS ASSIST

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
制作補助としてpause、1-frame advance、任意N-frame advance、1/2・1/4 slow、timeline scrub、bookmark、soft reset、power cycleを実装。制作中のpause時間やslow速度はrecorded game timeに入れない。slowはemulation frame cadenceを落とすだけで入力frame番号を壊さない。reset/powerはsystem eventとして記録しreplay一致をtest。
