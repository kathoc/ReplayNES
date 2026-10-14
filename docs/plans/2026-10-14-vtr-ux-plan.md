# VTR transport UX - implementation plan (2026-10-14)

Spec: `docs/design/VTR_UX.md` (wins over `docs/design/UI_REDESIGN.md` where they conflict).

## Done: engine + shared core (commit "VTR transport UX core")

- Engine: `rn_take_discard`, `rn_take_truncate` (no file-format change). Test: `tests/test_retry_takes.cpp`.
- Core `frontend/src/tape.cpp`, header section "VTR transport UX" in `frontend/include/replaynes/frontend.h`:
  - `rnf_tape_overwrites(mode, frame, length)`: resuming RECORD before the end = overwrite.
  - `rnf_retry_*`: one `rnf_retry` per emulation controller. Live play steps go through
    `rnf_retry_step` (instead of `rn_step`; not frame steps / fast-forward / practice return);
    `rnf_retry_reset` on new / opened / reset / closed session; `rnf_retry_can_undo` drives the
    Undo chip; `rnf_retry_apply_undo` performs Undo (then pause there, refresh take state).
  - `rnf_tape_status_label` (bottom-left label), `rnf_tape_time_label`.
  - `rnf_coach_*` (three first-run toasts; persist `rnf_coach_shown` bits).
  - `rnf_video_range`, `rnf_video_quality*`, `rnf_video_summary` (Make Video dialog).
- Menu model (`frontend/src/menu.cpp`): Retry = `retry.rerecord` (Play from Here), `retry.bookmarks`,
  `retry.playback` (Playback toggle), `retry.restart` (Start Over). The `takes` page and
  `retry.undo` / `retry.takes` are gone. Share > `share.export` is labelled Make Video.
- Input: `hk.undo_take` is now "Undo Retry"; default pad 1 binding `gc0:leftThumb` (L3). Layout 7:
  `rnf_input_undo_migration` (call it for saved configs with layout < 7).
- Strings (ja/en) in `apps/macos/Resources/Localizable.xcstrings` (keys used below).

## Platform work (macOS: `apps/macos`; desktop: `apps/desktop` + `apps/linux` + `apps/windows`)

Both frontends, same behaviour:

1. **Live play** through `rnf_retry_step`. Remove the "Recording a new take from here…" notice. When
   the active take id changes after a step (or `branched`), refresh take-dependent state (thumbnail
   cache rebase / structure) as for a branch today.
2. **Rewind release** (L2 hold that started during play): `pauseAfterRewind` on -> paused (as now);
   off (default) -> the resume countdown (`rnf_resume_countdown_start`), then play. Same for an R2
   fast-forward started during play that does not end at the tape end. A hold started while paused
   still ends paused.
3. **Overwrite notice**: whenever a resume countdown starts and `rnf_tape_overwrites(RECORD, frame,
   length)`, draw one line "Retrying from here" (key `Retrying from here`) under the numeral, inside the
   same pass as the countdown (macOS: `CountdownOverlay` / `GameRenderer.drawCountdown`; desktop: the
   ImGui draw list). Countdown setting off: show it as a short toast instead.
4. **Undo chip** "↶ Undo" (key `↶ Undo`) at the right end of the bottom area while
   `rnf_retry_can_undo(now, active take, mode)`; clickable; with a controller connected it shows the
   pad button bound to `hk.undo_take` (default L3) as in the hint bar. `hk.undo_take` (pad / key) and the
   click call `rnf_retry_apply_undo`, then pause there (seek bar shown) and notice `Back to before the
   retry`. Replace the old `undoTake` (rn_undo_take_switch) wiring.
   Wire layout 7: `rnf_input_undo_migration` in the saved-binding migration ladder.
5. **Playback (REPLAY)**: any game button pressed while watching (playing or paused on the seek bar;
   not in a menu / dialog) switches to RECORD at that frame and starts the resume countdown (with the
   overwrite notice when before the end). Remove the Watch/Record toggle and "Previous Attempt"
   entries from app / window menus (macOS app menu). `retry.playback` toggles Playback as before.
6. **End of tape**: when Playback / fast-forward reach the end: paused, status "End of tape", no
   "press Record…" notice. Paused at the end of the tape the resume hint reads `Continue (tape)`, and
   resuming there continues play (REPLAY -> RECORD; no more "restart from the beginning").
7. **No REC anywhere**: remove the REC / "● REC / PLAY" badges and the top-left status badge during
   play; the playhead uses the neutral accent colour (not red).
8. **Status label** (`rnf_tape_status_label`, plus `Making video… %lld%%` while an export runs) at the
   bottom left of the bar area, only when non-empty.
9. **Rewind / FF held**: show the bottom seek bar read-only with the status label at its left (the
   VTR effect stays).
10. **Paused seek bar**: at most 4 hints - cancel `Play` (`Continue (tape)` at the end of the tape),
    L/R frame step, L2/R2 rewind / fast-forward (`Rewind` / `Fast-forward`), `↑ Practice sections`.
    The marker / A-B hints (Y practice, X slot, confirm marker…) appear only after ↑ enters the marker
    lane. Button assignments unchanged. The recorded part after the playhead is drawn dimmed.
11. **End-of-tape chip**: paused at the end of the tape (length > 0, not practicing): `▶ Make Video`
    chip (click / tap opens the Make Video dialog); pads reach it via the menu.
12. **Coach toasts** (`rnf_coach`, toast style, persisted bits: macOS UserDefaults `coachShown`,
    desktop settings key `coachShown=`). `rewind_control` = the name of the control bound to
    `hk.rewind` for the device in use (pad: L2 glyph of the family; keyboard only: the key name).
13. **Make Video dialog** (Share > Make Video, the end-of-tape chip): one screen, <= 6 rows -
    Range (`Range`: `All (range)` / `Up to here` / `Section %lld` for each A/B slot visible on the active
    take with A and B), Quality (`Quality (video)`: `rnf_video_quality_label` presets), Length · size
    (`rnf_video_summary` with `rnf_export_predict_size` of the current range / canvas / codec / quality),
    `Details ›` (the existing settings: codec / size / PAR / overscan / flash / CRT; bit rate = Quality),
    buttons `Export` / `Cancel (make video)`. Pad navigable like the other dialogs. After export: toast
    `Video saved` with Open / Share actions (desktop: Open = the file, Share = its folder).
14. **Wording**: Retry page uses the new keys (`Play from Here`, `Playback (menu)`, `Start Over`), Share
    uses `Make Video`; no UI path shows takes / "Previous Try" / "Takes" / REC. Never 「あそぶ」.
15. **Tests**: update tests that use removed items; add tests for the new flows where the frontend
    has a windowless test harness (desktop `apps/desktop/tests/test_desktop_frontend.cpp`; macOS XCTest
    for pure logic).

Strings: every user-visible string must be a catalog key with ja + en. Add missing ones only with
`python3 <scratchpad>/l10n_set.py apps/macos/Resources/Localizable.xcstrings <entries.json>` (never
hand-edit; ja must be natural Japanese). Unused catalog keys are removed at the end by the lead.
