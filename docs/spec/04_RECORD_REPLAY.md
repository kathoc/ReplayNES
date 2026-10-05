# 04 RECORD REPLAY

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
FrameInputLogを実装。P1/P2の最終bitfieldとsystem eventをframeIndexに対応させる。変化点/RLE圧縮は単純で検証可能な方式にする。recordしたsessionを新規core instanceで先頭からreplayしhash一致をtest。連射やphysical mapping情報を正本にしない。coreへ渡した最終入力を正本にする。
