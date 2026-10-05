# 10 EXPORT MP4

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
active takeのoffline rendererを作る。fresh coreで初期状態からinput/event logを正規速度論理時間で再実行し、video framesとPCMを生成。AVAssetWriterでH.264/HEVC + AAC MP4へ。timestampはframe/sample count由来。nearest-neighbor、1280x960等preset、overscan/pixel aspect設定を用意。export前後でproject/timelineを変更しない。export runのhashが通常replayと一致するtestを作る。
