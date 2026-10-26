# VTR-style transport UX (2026-10-14)

Goal: a beginner who knows nothing about emulators can rewind like a video deck, retry from
there, and finally make a no-miss replay video. This document amends `UI_REDESIGN.md`; where
they conflict, this one wins. Applies to macOS, desktop (Linux/Steam Deck, Windows) and iOS.

## Concept: one tape, always running

- The UI never says "record". Playing *is* recording. No REC badge, no "● REC / PLAY" in the
  pause bar, no red playhead (playhead uses the neutral accent colour).
- The user-facing verbs are video-deck words: Play, Rewind, Fast-forward, Pause, Frame step,
  Slow, and **Make Video**.
- Takes/branches stay in the data model and the `.nesrec` file but are **not shown**: no take
  list, no "branch", no "Previous Try" entry in menus. (Existing take files load unchanged.)

## Behaviour

1. **Releasing Rewind (L2)**: the 3-2-1 resume countdown (`rnf_resume_countdown`) runs, then
   play resumes automatically. `pauseAfterRewind` stays available in Settings, default **off**
   for everyone.
2. **Overwrite notice**: when play resumes at a cursor before the end of the tape (i.e. the
   resume will start a new take), the countdown shows one extra line under the number:
   ja「ここからやり直し」 / en "Retrying from here".
3. **Undo (10 s)**: for 10 s after such an overwrite starts, a small chip
   ja「↶ 取り消す」 / en "↶ Undo" is shown at the right end of the bottom area (clickable /
   tappable; pad: shown in the hint row with its button, use the menu-confirm-free button
   chosen by the implementer and document it). On a pad the button (`hk.undo_take`, default L3)
   must be **held 0.6 s** (`kUndoHoldSeconds`); the chip then reads ja「↶ 長押しで取り消す」 /
   en "↶ Hold to Undo" and fills from the left while it is held. A tap does nothing: a soft stick
   clicks L3 by accident in play, and right after a retry that threw the new attempt away (reported
   2026-10-26 in Ninja Spirit as "rewind goes wrong"). A click or tap on the chip still undoes at
   once. Undo restores the previous continuation (switch
   back to the parent take at its previous end/position) and discards the new attempt.
   **If the user rewinds again within those 10 s, only the most recent attempt is kept**: the
   short attempt that was just abandoned is not kept as a separate take (drop it), so rapid
   retries do not pile up hidden takes. The undo target always remains the original
   continuation from before the retry burst.
4. **Replay state ("再生" / "Playback")**: when the user is watching the tape (REPLAY mode),
   pressing any game button switches straight to play (RECORD) from that point, with the
   countdown and overwrite notice as above. No explicit mode toggle is needed; the old
   Watch/Record toggle menu items are removed.
5. **Status labels** appear only for non-default states, at the bottom-left of the bar area
   (no top-left badge during play): 再生中 / Playback, 練習中（記録しません）/ Practice (not
   recorded), ◀◀ 巻き戻し / Rewind, ▶▶ 早送り / Fast-forward, スロー ½ / Slow ½, テープの終わり /
   End of tape, export progress.

## Countdown type-ahead

From the moment the resume countdown starts until play resumes, the NES buttons (D-pad, A, B,
Select, Start - whatever is bound to an NES pad button on a pad, the keyboard or touch) are game
input only, typed ahead for the first frame:

- They never act on the UI or the transport: no frame step (the paused D-pad ←/→ stepping is off
  while the game is unpaused, and a direction still held from stepping is handed back to the
  game when play resumes), no seek bar tap, no countdown cancel (neither countdown has a cancel tap; Y stops practice), no hint action.
- Hotkeys that are not NES buttons keep their meaning: L2 / R2 (rewind / fast-forward; holding
  one cancels the countdown back to paused, as before), L+R (menu: pauses, so it cancels too), L / R held + START (a pause) and L held + R (a pause; R held + L only toggles slow ½ and does not cancel),
  L3 (Undo, when held), the pause hotkey (cancels).
- **Held through**: a button held when the countdown ends is pressed from the very first frame
  (the normal sample of that frame).
- **Pressed and let go inside the countdown** (decision): it counts. It is latched and applied as
  a press on the first frame and released on the next (`rnf_resume_countdown_apply`). Taps from
  before the countdown (e.g. the seek bar's Play tap, or a button tapped to leave Playback) are
  not typed ahead: the first sample of the countdown is its baseline. A latched direction is
  dropped when the opposite one is held or also latched (never left + right / up + down).
- The first frame and every frame after it go through `rn_step` like any live frame, so they are
  recorded in the input log (deterministic: replay and export see exactly what was played).
- A cancel (pause, menu, rewind / fast-forward held) or a restart drops what was typed ahead; a
  modal dialog that holds the countdown at its start starts the type-ahead over as well.
- Shared core: `rnf_resume_countdown_input` (every counting tick: the sampled pads) and
  `rnf_resume_countdown_apply` (every live frame between `rn_input_sample_game` and `rn_step`);
  ctest `test_frontend_resume_countdown`. The practice countdown (after returning to A) has no cancel tap
  either (2026-10-17): its NES buttons are game input (held through = held from the first frame);
  practice stops with Y or the "■ Stop Practice" status button.

## Screens

- **While Rewind/FF is held**: show the bottom seek bar read-only (plus existing VTR effect),
  label at its left (◀◀ 巻き戻し). Replaces the top-left badge.
- **Paused / stopped after rewind (when pauseAfterRewind is on, or user pause)**: seek bar with
  at most 4 hints: `プレイ / Play` (resume; at end of tape `続きをプレイ / Continue`), frame
  step L/R, 巻き戻し/早送り L2/R2, `↑ 練習区間 / ↑ Practice sections`. The A/B / marker
  hints appear only after ↑ enters the marker lane. Button assignments are unchanged.
- The recorded-but-ahead-of-cursor part of the strip is drawn dimmed; unrecorded part as today.
- At end of tape while paused: a `▶ 動画にする / ▶ Make Video` chip (click/tap); pads reach it
  via the menu.

## Make Video dialog

Menu tile Share/MP4 becomes ja「動画にする」/ en "Make Video". One screen, ≤ 6 rows:

```
範囲   ● 全部  ○ ここまで  ○ 区間1..8          Range: All / Up to here / Section n
画質   ● 標準(YouTube)  ○ 高画質  ○ 小さく      Quality: Standard / High / Small
長さ   15:02 ・ 目安 91 MB                       Length · approx. size (estimate only)
詳細 › codec / size / PAR / overscan / flash / CRT / bitrate (current settings, unchanged)
[ 書き出す ] [ やめる ]
```

Quality presets map onto the existing bitrate choices (`rnf_export_*`); size is labelled as an
estimate and export never fails because the estimate was off. After export, the toast offers
Open / Share.

## Wording (ja / en)

| Old | New |
|---|---|
| ここから録り直す / Record from Here | ここからプレイ / Play from Here |
| 再生で見る / Watch | 再生 / Playback |
| 記録を最初からやり直す / Restart Recording | 最初からやり直す / Start Over |
| A/B リピート, A/B n | くり返し練習・区間 n / Repeat Practice · Section n |
| MP4に書き出す / Export to MP4 | 動画にする / Make Video |
| テイク / 分岐 / 前の試行へ / Takes / Previous Try | removed from UI |
| 録画中 / REC | removed |

Never use 「あそぶ」 or other childish wording. Keep 「プロジェクト / Project」.

## First-run coach lines

Three one-line coach toasts, each shown once ever (flag in settings), using the toast style:
1. 5 s after first game start: 「L2 を押し続けると巻き戻せます」/ "Hold L2 to rewind"
   (use the actual bound control name for keyboard/touch).
2. First stop after a rewind: 「ここからプレイするとやり直せます」/ "Play from here to retry".
3. First time at end of tape with ≥ 1 min recorded: 「☰ › 動画にする で動画にできます」/
   "☰ › Make Video turns this into a video".

## Implementation notes

- Shared logic (overwrite condition, undo window + burst collapse, coach flags, quality
  presets, wording) goes in the shared frontend core (`frontend/`) with C API + ctest; platform
  frontends only draw.
- iOS (local-only `ios` branch) picks this up by merging main; touch transport bar is later.
