# Sources and inspection ledger

## 2026-10-18 Core selection

User authorization: 「ReplayNESは変更してOK。ただしコアを変更できるようにして。」
Clarification: 「はい、両コアを選択・切替」 (Nestopia and ReplayNEScore).
This supersedes the previous read-only constraint from the ReplayNEScore project.

The primary agent read engine/src/core/ICore.h and ICore.cpp, engine/CMakeLists.txt,
and docs/COMPATIBILITY.md in full. Session create/step/checkpoint sections were inspected selectively.
Exact excerpt: `// ICore: the only interface the rest of the engine uses to talk to an emulation core.`
Compatibility excerpt: `A take is reproducible only on the exact same emulation core behaviour.`
The audit agents inspected host selection/UI call sites and documented their precise scope in
../ReplayNEScore/docs/reviews/host-core-switch-audit.md and host-core-ui-audit.md.
The primary agent read both reports in full. Third-party emulator implementation code was not inspected
or reused for the new core adapter.

Specification before implementation: docs/CORE_SELECTION_PLAN.md, mirrored from
../ReplayNEScore/docs/ReplayNES_Core_Selection_Plan.md.

主担当は docs/COMPATIBILITY.md 全文、READMEのbuild節、CLI cmdScreenshot全体と
AppModel desktop/macOS変更差分を読んだ。
実在引用: "A take is reproducible only on the exact same emulation core behaviour."
既存仕様のcore identity保持を維持し、Live-only coreの制約とoptional build手順を追記。

2026-10-18: 利用者のスプライト制限解除／フラッシュ軽減の要望を仕様へ追加。
主担当がfrontend/src/menu.cppのdisplay.flash周辺、settings.hのflash既定値、NestopiaCore.cppの
EnableUnlimSprites周辺とdocs/FLASH_REDUCTION.mdのscope/default/CRT/export関連箇所を読んだ。
実在引用: "Dims hard full-screen flashes (photosensitivity)"
CORE_SELECTION_PLAN.mdは兄弟ReplayNEScore/docsの正本と同期。今回は仕様のみで実装変更なし。

## 2026-10-18 pulse control opt-in

主担当がReplayNEScoreAdapter cpp/headerとadapter試験全文を確認し、pulse制御を有効化。
実在引用: `replaynes::dma::Cadence::get,true,replaynes::apu::Phase::clock,true,true);`。
開発中core compat/build IDをv2へ更新、Nestopiaは変更していない。
adapter/core selection試験2/2成功。所持ROM CLI停止ログ全文を確認し、
`ReplayNEScore stopped at master tick 2134536 PC=$FA9B CPU write $4001 data=$7F` を記録。
独立core側も同じ停止で、タイトル/PCM完成の根拠とはしない。
