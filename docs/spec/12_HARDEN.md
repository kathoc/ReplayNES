# 12 HARDEN

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
長時間運用を堅牢化。数時間相当のrecord/replay、数百回rewind/branch、save/reopen、crash recovery、corrupt state/log、disk full、controller disconnect、ROM移動、core mismatchをtest。core mismatch時は無警告で開かない。migration/legacy-core方針を文書化。memory/disk使用量をprofileしcheckpoint interval/segment compactを必要な範囲だけ最適化。
