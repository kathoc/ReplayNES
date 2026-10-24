# ReplayNESCore 音声ドリフト修正の受け入れ報告

- Result: done（PASS）
- Changed:
  - `engine/src/core/ReplayNEScoreAudio.h`
  - `engine/src/core/ReplayNEScoreAdapter.cpp`
  - `engine/src/core/ReplayNEScoreAdapter.h`
  - `tests/test_replaynescore_audio.cpp`
  - `tests/test_replaynescore_adapter.cpp`
  - `docs/plans/2026-10-24-replaynescore-continuous-audio.md`
  - `docs/CORE_SELECTION_PLAN.md`
  - `docs/reviews/replaynescore-regression-note.md`
  - `docs/reviews/replaynescore-audio-drift-2.md`
- Key decisions:
  1. 描画オフの29780 2/3 CPU cycle/frameと固定フレーム時間29780.5の差が累積することが原因。実測N slotの境界を `floor(i*655171*48000/N)` に写し、固定幅39375000の出力区間へ整数積分する。各フレームの出力数は時間軸と一致し、端数phase・area・DC履歴は保持する。出力繰越による遅延蓄積は採用しない。
  2. soft resetの7 slotは未写像PCMとして保持・保存し、次のフレーム終了時に写す。payload schema / stateFormatVersionは3、compat / build IDはv16、audio state versionは2、DC profileは1。レビューで見つかったpending数の整合性不足も修正し、実PPU dot数とraster位置から求めたフレーム内CPU slot数に照合する。旧stateは拒否する。
  3. 既存計画の22案を実態に合わせて更新し、独立区間積分oracleと描画オフ620フレームの回帰試験へ確認手順を集約した。別スキルは追加しない。指定調査記録・報告契約・計画書は全文、外部coreはrender状態の定義とdot更新箇所を確認した。外部core、ROM、sources.mdは変更していない。描画変更はないため画像検証は対象外。
- Verification:
  - `cmake -S . -B /tmp/replaynes-drift-on -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release -DREPLAYNES_ENABLE_REPLAYNESCORE=ON -DREPLAYNESCORE_SOURCE_DIR=$HOME/Playground/ReplayNEScore`、`cmake --build /tmp/replaynes-drift-on -j 4`: 成功。
  - 旧実装で `/tmp/replaynes-drift-on/tests/test_replaynescore_adapter '600 rendering-disabled'`: exit 1。frame=15で `ReplayNEScore continuous audio count disagrees with frame timeline`、1 assertion failure。追加試験が従来実装の不具合を検出した。
  - 修正後 `ctest --test-dir /tmp/replaynes-drift-on --output-on-failure -j 2`: 34/34通過、373.24秒。adapterは249.02秒。描画オフ620フレーム、各フレーム音声数・累積数・非無音、600フレーム後の復元、610フレーム目のsoft reset直後の保存復元を検証。
  - 最終ビルド後 `/tmp/replaynes-drift-on/tests/test_replaynescore_adapter 'rejects pending'`: 1/1通過。全ctest実行中に追加したこの試験は別途実行。pendingの7 slot挿入とRESET後7 slot削除を拒否し、拒否前後の保存バイト列が一致。
  - `/tmp/replaynes-drift-on/tests/test_replaynescore_audio`: 5/5通過。独立した絶対区間交差oracleでPCM・phase・area・DC・cursorを比較し、復元一致・不正入力とoverflowの原子的拒否を確認。
  - `/tmp/replaynes-drift-on/tests/test_replaynescore_adapter 'restores DMC sample DMA'`: 1/1通過。
  - `cmake -S . -B /tmp/replaynes-drift-default -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release`、`cmake --build /tmp/replaynes-drift-default -j 4`、`ctest --test-dir /tmp/replaynes-drift-default --output-on-failure -j 2`: 成功、32/32通過、184.14秒。
  - `/tmp/replaynes-drift-on/tools/replaynes-cli/replaynes-cli dump-audio 'roms/Super Mario Bros. (World).NES' --core replaynescore --frames 600 --out /tmp/replaynes-drift-smb.wav`: exit 0、600 frames / 479210 samples。指定コマンドそのままでは必須の `--out` がなくusage/exit 1だったため、CLIの既存仕様に従って出力先を補った。
  - Python標準`wave`で生成WAVを読取: mono / 48000 Hz / 16 bit / 479210 samples、時間軸の期待数と一致。入力なしSMB実行のPCM範囲は[0,0]。非無音性はパルス回帰ROMで別途確認。PCM SHA-256: `7f898082c3d50efcf4645384204d4a762d8e17775e65da3f7143dcae50f3d763`。
  - `git diff --check`: 成功。`git -C $HOME/Playground/ReplayNEScore status --short`: 出力なし。ビルド・音声出力・検証ログは全て`/tmp/replaynes-drift-*`。
- Follow-up: コミット未作成。対象9ファイルを明示した`git add`と`git commit`を試行したが、環境が`.git`を読み取り専用にしており`Unable to create .git/index.lock: Read-only file system`で失敗。権限昇格不可のため変更は作業ツリーに残した。書き込み可能な環境で同じ9ファイルだけをコミットする。sources.mdの既存変更と利用者の調査記録・ROMは対象外。pushは実施していない。
