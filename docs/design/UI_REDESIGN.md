# UI/UX redesign (2026-10-08) — single source of truth for all frontends

Goal: intuitive for beginners and experts, no manual needed, minimal text, modern look, no scrolling.
Applies to macOS (SwiftUI), Linux/Steam Deck and Windows (shared ImGui frontend `apps/desktop`).

## Input model

| Input | During play | Paused (seek bar) | In menus |
|---|---|---|---|
| **L+R together** (both pressed within 100 ms; either order) | open Quick Menu (pauses) | open Quick Menu | close Quick Menu (resume) |
| R alone (fires on **release**) | pause — shows only the seek bar (no menu) | step forward 1 frame (held ≥ 400 ms: repeats) | next page (L1/R1 page switching) |
| L alone (fires on **release**) | slow ½ toggle | step back 1 frame (held ≥ 400 ms: repeats) | previous page |
| L2 / R2 hold | rewind / fast-forward (VTR look, see below); practicing: **L2 + R2 together = back to A now** | rewind / fast-forward | rewind / fast-forward |
| D-pad / stick | game | ← → step 1 frame; ↑ to the markers | move focus |
| Confirm (**east** by default) / cancel (**south**) | game | confirm tapped = drop a marker; **cancel tapped = resume** | confirm / back (back on the top level = resume) |
| Y / X | game | Y tapped = practice the selected slot's section; X tapped = next A/B slot (a focused marker: X deletes it) | contextual (rename / clear …), shown in the hint bar |
| Keyboard | Space = pause | Space = resume | Esc = open/close menu, Enter = confirm, Backspace = back, arrows = move |

- Chord detection lives in the shared core (`frontend/`, C API `rnf_chord_*`) so all platforms behave identically. **ALONE_UP is the trigger**: L or R alone act only on their release, so a single press never interferes with the L+R chord (ALONE_DOWN only means "held alone": a member bound to a game button is held from there, a held marker moves). While paused the core's repeat mode (`rnf_chord_set_repeat`) fires ALONE_REPEAT at press + 400 ms and every 50 ms after (frame steps); the release then reports the repeats and does not step again. A chorded member never repeats.
- Bindings stay remappable (the chord is an action "hk.menu" bound to a two-element combo). In play L / R keep their own bindings (default pause / slow); while paused the two chord members always step (back / forward).
- **Confirm / cancel** (`rnf_ui_confirm_element` / `rnf_ui_cancel_element`): east = confirm, south = cancel on every controller family by default (Nintendo style); Settings › Controls › 決定ボタン / Confirm Button swaps them, shown as glyph pairs of the connected family ("Ⓐ決定 Ⓑ戻る" on Nintendo / Deck-style "Ⓑ決定 Ⓐ戻る", "○決定 ×戻る" on PlayStation). Every hint bar, the on-screen keyboard (Type = confirm, Delete = cancel), dialogs, prompts and the paused seek bar follow it. Game input never changes.
- **Hold speed** (`rnf_hold_speed`): rewind (L2), fast-forward (R2) and a marker moved with L / R held use one curve, frames per 60 Hz tick: 2 for the first 0.5 s, 3 until 1.5 s, then 4 — the same rate and acceleration in opposite directions.

## Always-visible menu affordance

- A small pill **"☰ Menu  L+R"** (en) / **"☰ メニュー  L+R"** (ja) is always on screen in a corner (top-right of the game area, not over the picture when letterboxed; in FILL it sits over the picture with low contrast and fades to 30 % after 3 s of play, back to 100 % on mouse move / touch / pause).
- It is a button: click / tap opens the Quick Menu (rescue when no controller). The glyph changes to the connected family (Steam Deck/Xbox: L+R, PlayStation: L1+R1, Nintendo: L+R, keyboard only: Esc).
- In full-screen direct-to-display mode the pill must not defeat the low-latency path: draw it inside the game's own render pass (Metal/Vulkan/D3D11), not as an OS overlay.

## Quick Menu (top level — 6 tiles, one row, nothing else)

| Tile | Icon (Tabler/SF Symbol) | Contents (next level, ≤ 6 items each) |
|---|---|---|
| 再開 / Resume | play | (closes menu) — default focus |
| やり直す / Retry | history | ここから録り直す · 前の試行へ · テイク一覧 · ブックマーク · 再生で見る · 記録を最初からやり直す (6 = the limit) |
| 練習 / Practice | target | 8 A/B slots as 4×2 cards (thumbnail of A, name, length); A = practice, Y = rename, X = clear; empty slot = "＋" (set A here) |
| 共有 / Share | share | MP4に書き出す · 配信出力 (Syphon/Spout) |
| 設定 / Settings | settings | 画面 · 操作 · 音 · システム (L1/R1 switch between these four) |
| ゲーム / Game | device-gamepad | ゲーム選択 · 保存 · 別名で保存 · プロジェクトを開く · リセット |

**記録を最初からやり直す / Restart Recording** (Retry, 6th item) opens the same Reset Project confirmation as ゲーム › リセット › プロジェクトをリセット… (keep the A/B sections or not; a saved project is backed up to the Trash first). It is where a player looks for "start the run over"; the Game page keeps its リセット (soft reset / power cycle / reset project) because it still fits (5 items) and soft reset / power cycle belong there. Retry is now full: anything new for it goes one level down.

### Settings pages (≤ 6 rows each, 2 columns; deeper detail one level further)

- **画面 / Display:** サイズ (等倍 | FILL) · ブラウン管 (on/off) · 点滅を抑える (切 弱 中 強) · ビデオ演出 / VTR Effect (on/off, default on) · 画面の形 / Picture Shape › (8:7 · 端を隠す) · ブラウン管の詳細 ›. (2026-10-09: VTR Effect joined this page; 8:7 and Hide Edges - set once, both about the picture's shape - moved one level down so the page keeps six rows and the depth stays 3.)
- **操作 / Controls:** コントローラー › (diagram page) · キーボード › · 決定ボタン (east | south, as glyph pairs) · 一時停止中の十字キー (on/off) · 画面キーボード (自動 | 内蔵 | Steam; Linux only) · 詳細 › (巻き戻し後に一時停止 (off by default since 0.5.1) · 連射の速さ · 連射の押す長さ · 逆方向の同時押し · スティックのしきい値 · 初期設定に戻す)
- **音 / Sound:** 音量 (slider) · スロー中の音 (on/off if supported) · 低遅延 (on/off where applicable)
- **システム / System:** 言語 (自動 | 日本語 | English) · アップデート › · Steamに追加 (Linux) · バージョン情報 ›

### Remapping with the controller alone

- The HOME / guide button (Steam, Xbox, PS, HOME) is the system's (Steam overlay, Game Bar): not on the diagram, never captured or assignable, ignored by the input routing (`rnf_input_element_ignored`); layout 6 drops old bindings to it.
- **コントローラー:** the diagram of the pad (core geometry `rnf_diagram_*`; outline `rnf_diagram_decor_*`: a rounded pill body without grips for Xbox / PlayStation / Nintendo / generic, a wide rounded rectangle with the screen hinted and the trackpads for the Steam Deck). Every element is focusable with the D-pad / left stick (focus ring, nearest element in that direction). Confirm opens the action picker; X = next pad, Y = defaults, cancel = back.
- **Action picker** (`controls.assign`, a LIST page of the menu model): "None" + the pad's own player's actions, the other player's, the hotkeys (`rnf_input_assign_choices`), 6 rows per sheet, L / R switch sheets, no scrolling; the current action is checked and focused when it opens. Confirm assigns (the button does exactly that action from now on) and returns to the diagram; cancel returns without a change.
- **キーボード:** rows of actions (6 per sheet, L / R); confirm = "press the new key" (keys only), the controller's cancel / Esc cancels; X clears.

Rules: no screen ever scrolls; if a page needs more than 6 rows, split it into a "… の詳細 ›" sub-page. Max depth 3 (Quick Menu › 設定 › 画面 › ブラウン管の詳細).

## Visual language

- Game frame stays visible behind menus, dimmed to ~35 % (not blurred — keep it cheap on all GPUs).
- Panels: dark translucent (#141416 at 92 %), corner radius 16 px (tiles 12 px), 24 px outer margin, 10 px gaps.
- Focus: 2 px brand-red (#FF3B3B) ring + slightly raised tile tint; 120 ms ease-out on focus move / open / close.
- Typography: one family, three sizes (title 22, label 15, hint 12). Labels 2–5 characters (ja) / 1–2 words (en). Never paragraphs: the only explanatory text is a single line at the bottom describing the focused item.
- Hint bar bottom-right: button glyphs for the connected controller family + ≤ 3 verbs.
- Breadcrumb top-left on sub-levels (icon + names), e.g. ⚙ 設定 › 画面. Every segment but the last is a link for the mouse / touch (`rnf_menu_crumb_select`): back to that level with its focus; 設定 goes to the Settings top page (画面, what the tile opens). Controllers never focus it (back already goes up).
- Icons: macOS SF Symbols; desktop: an icon font subset (Tabler Icons MIT, or Material Symbols Apache-2.0) bundled and recorded in THIRD_PARTY_NOTICES.md.
- Light/dark: menus are always dark (they sit over game video).

## Dialogs and notices

No system alerts while the user can be on the controller: every question is an **in-window dialog** in the Quick Menu's look (macOS `Dialogs.swift` + `Views/DialogOverlay.swift`, pure navigation `DialogNav` (tested); desktop `ui_dialogs.cpp` via `DialogHost`).

- Layout: over everything in the window (game, menu, library), the rest dimmed; panel #141416 / radius 16; title (22) + at most a few lines of message, an optional on/off row (e.g. "Keep A/B repeat sections") or text field (rename), then one row of button tiles (left to right) with the 2 px brand-red focus ring, the destructive one in brand red; the hint bar shows confirm + the cancel button's name. Nothing scrolls; long details are clipped.
- Input: D-pad / left stick ← → move between the buttons, ↑ ↓ to / from the on/off row; **confirm** (east by default, per 決定ボタン) presses the focused button (on the row: flips it), **cancel** (south) / Esc / Delete press the dialog's cancel button wherever the focus is; Return / Space = confirm, arrows move; the mouse hovers to focus and clicks. A text field takes the keyboard (Return = the focused button, Esc = cancel); the controller's confirm / cancel still work.
- While a dialog is up it has the focus: the Menu pill (both the SwiftUI and the Metal one), the L+R chord, Esc-to-menu and the menu commands do nothing; the game is paused behind it (resumed afterwards only if the dialog paused it and the same session is still there). Dialogs queue; flows continue in callbacks (nothing blocks the main thread).
- Default focus is the safe choice: Save in save questions, **Cancel** in the Reset Project confirmation.
- **Toast instead of a dialog** when there is nothing to decide (visible ≥ 4 s, 8 s for the longer ones below, never takes input): "Unsaved work was restored (up to the last autosave)" after a crash / force quit, "the last project can't be found" at launch, "couldn't resume where you left off" (after the open error's own dialog), "Saved" / "Project reset", and (desktop) the result of an export left running in the background.
- Converted (macOS): save / don't save when switching or closing a game (temporary session and project), the unsaved previous temporary session, Reset Project (with Keep A/B), open errors (ROM not found / mismatch → Locate ROM…, damaged checkpoints / A/B sections → discard and open, other core, newer format), any other error (OK), rename (bookmark / A/B section). The export sheet takes the controller's confirm / cancel (Export… / Cancel, Close when done or failed; while it runs the pad keeps playing the game) and shows a settings problem inline.
- Still system UI (keyboard / mouse paths only): file panels (Open / Save As / Locate ROM / export destination), Sparkle's own update windows, and the one quit-time question (persisting failed, or quitting without session persistence: `applicationShouldTerminate` is synchronous and the window may already be closed). The desktop frontends draw their own file chooser.

## Library (start screen)

- The app always starts here (no automatic resume). Top: a large "続きから / Continue" hero card (last session's thumbnail + game name + time) — A resumes the session of the resume record (a temporary session or a project, journal recovered after a crash / force quit), else opens the latest project. Starting another game while a temporary session waits there asks first (save / don't save / cancel).
- Top bar (between the name and the Menu pill): filter chips すべて / お気に入り / 最近 (All / Favorites / Recent: the last 24 games played, newest first, with when and how long), the sort (名前 / 最近遊んだ / メーカー / 発売年 / ジャンル — Name / Last Played / Maker / Year / Genre; cycles) and the search (on-screen keyboard). The controller reaches the row with up (order: row, hero, cards).
- Below: a grid of large game cards (thumbnail from the latest project/screenshot, else a generated tile with the game name), 4 per row at 1280×800, showing the game's title from the built-in game database (`frontend/data/nesdb.tsv`, shared core) in the UI language and "maker · year" small; a star badge on favourites. The focused game's "maker · year · genre" is the one-line description. A = play, Y = favourite on / off, X = projects of the focused game, View / Select (⧉; keyboard Tab / S) = next sort, Menu pill → Settings.
- Order: ja = gojūon order of the kana reading (dakuten / handakuten and small kana folded, ー read as the preceding vowel; Latin official titles such as FRONTLINE read フロントライン), en = title ignoring a leading "The"; ROMs not in the database keep their file name and come after the kana titles. Favourites / history / sort / filter persist in `library.json` (settings folder, written by the core).
- Empty library: a single card explaining where to put ROMs (folder path + "Open folder").

## Seek bar while paused (R)

- Only the filmstrip seek bar + time + the Menu pill; L / R (released) and D-pad ← → step one frame, L2/R2 rewind / fast-forward; the A/B lane stays.
- The cancel button resumes as a **tap** (default south): pressed while paused it still reaches the game, and resumes on release only if nothing else was pressed or stepped while held. So a button can be held across frame steps (hold B to run, step with →) without resuming; a quick tap still resumes. Confirm (drop a marker), ↑ (to the markers), Y (practice) and X (next A/B slot) are taps the same way. Space and the pill / menu resume too; the hint bar shows it.
- Tap rather than a dedicated button: (considered: resuming only with a focused "Resume" affordance — rejected, the bar has no focus by default; consuming the buttons outright — rejected, it broke held-button frame advance.)

### A/B markers (controller)

The markers are the controller's view of the **selected A/B slot** ("A/B n" in the hint bar; X tapped on the bar picks the next slot; also the slot last used on the Practice page) on the active take. State machine: `rnf_markers_*` in the shared core (pure, tested); both frontends only apply its results.

- Focus on the bar: confirm drops a marker at the playhead — none → the slot becomes "A only" there; one → the two markers become the range (left = A, right = B: `rn_practice_set_range`); two → nothing but a subtle hint ("X deletes one"); a marker already at the playhead → hint.
- ↑ (when ≥ 1 marker) focuses the marker nearest the playhead; ← → pick the other; ↓ or cancel back to the bar. While a marker has the focus, the pad belongs to the seek bar (nothing reaches the game).
- Confirm on a marker = edit: the picture follows it (seek preview); ← → ±1 frame (repeats while held); L / R held move it continuously with `rnf_hold_speed` (the rewind / fast-forward curve). It never sits on the other marker (it skips over it; the two swap roles A ⇄ B automatically) and stays within 0 … take length. Confirm commits (writes the range / "A only"), focus back to the bar, the playhead stays at the marker. Cancel reverts the marker and the playhead. Opening the menu / resuming while editing reverts too.
- X on a focused marker deletes it: one of two → the slot becomes "A only" at the other marker (its section length is cleared, the least surprising: the remaining flag stays where it is); the last one → the slot is cleared.
- Visuals: small flags on the lane above the filmstrip with a pole through it, in the slot's color (A's flag left of its pole, B's right; letters only once both exist); focus ring on the focused flag, brand red + thicker ring while editing. The hint bar lists the buttons of the state: bar (cancel Resume · L R Step · confirm Marker · ↑ Markers · Y Practice (when the slot has a section) · X A/B n), marker (←→ A ⇄ B · confirm Move · X Delete · cancel Back), editing (←→ ±1 · L R Move · confirm Done · cancel Undo).
- Practicing: no markers (the bar shows the practice run). The mouse / touch timeline (drag a range, handles) is unchanged.
- **Y = practice** (2026-10-09): Y tapped on the bar (or with a marker focused, not while editing) starts practice of the selected slot from its A when the slot has a section on this take - A and B, or A only (then it plays without a loop, like the Practice page). The decision (`rnf_markers_face`: Y practice / X next slot / X delete) is the shared core's. Slot cycling moved from Y to X: X on the bar = next slot, X on a focused marker = delete it (unchanged).

## Practice: return to A (2026-10-09)

One sequence for every arrival at A - reaching B, **L2 + R2** pressed together while practicing (from anywhere in the section), starting practice (menu, seek bar Y): brief hold (at B only, 0.5 s) → **1 s VTR rewind** → A's picture → **3, 2, 1** → play. State machine `rnf_practice_loop` (shared core, tested on a fake clock); the frontends only apply its actions.

- **Rewind sweep:** always exactly `RNF_PRACTICE_REWIND_SECONDS` = 1 s, whatever the section length: the pictures run backwards from B (or where L2 + R2 was pressed) to A evenly over that second. They come from the run's **reel** (`rnf_reel`): during the run every picture is offered with its position; it keeps ≤ 120 full frames evenly spread from A (stride k keeps positions with (p − 1) mod k = 0, so the frame after A is always there; when full every other one is dropped and the stride doubles; dropping only reorders slot indices, no pixels move). Display only: never touches emulation or determinism; a practice rewind (L2) truncates it; freed when practice ends.
- **A's picture:** after going to A the run emulates the frame after A with no input and takes it back (`rnf_practice_preview_a`: one step + `rn_rewind(1)`; the state, counter and A's continuity are exactly as before), so the countdown shows A, not a stale picture.
- **Countdown:** a large centered numeral (3, 2, 1; one per second) on a soft dark disc with a thin ring running down the second; subtle scale-in (1.14 → 1) / fade-in, hold, fade-out (`rnf_practice_countdown_visual`, the same curves everywhere). Drawn inside the game's own pass (Metal texture on macOS, ImGui's draw list in the same Vulkan / D3D11 pass), so full screen stays direct-to-display. Setting: **Countdown on/off** (default on) on the **Practice page** - the Options button (⧉ / View / −; keyboard Tab, macOS S) toggles it, the hint bar shows "Countdown: On / Off" (clickable). (Controls › More is full at six rows; the Practice page is where practice is set up.) Off: play resumes right after the sweep.
- **Input during hold / sweep / countdown:** nothing is emulated; the game input is sampled and dropped every tick, so a tap there is not latched into the first frame afterwards; a button held through the countdown counts from the first frame (hold → to run off at once). L2 does not rewind the run during the return (or while the L2 + R2 chord is still held); the chord during a return is ignored. The L+R menu works as always; the **cancel button tapped** during the countdown (pressed and released within it, nothing else pressed) ends practice (back to the take, paused) - a press held from before the countdown never counts.
- **Pauses and dialogs:** time that does not tick (paused, the menu, a dialog) is not counted - the countdown resumes where it was. A modal UI holds the practice run (`modalHold`): it never plays on behind a dialog. The L2 + R2 chord only acts while playing (paused it latches and does nothing).
- **Audio:** stays muted through the sweep and the countdown (a short fade tail when the chord interrupts play, as at B). No tape whir.

## VTR effect (2026-10-09)

The tape-rewind look (`rnf_vtr`, shared core): two soft horizontal tracking-noise bands drifting smoothly (upwards while rewinding, downwards for fast-forward), low-contrast light / dark noise streaks inside them (changing at 30 Hz), a slight line wobble and the head-switching skew of the last lines, mild chroma bleed (the colour smears ~2 px right), desaturation and slightly lifted blacks.

- **Where:** the practice return sweep (full), in-game **L2 rewind** (subtle: 60 %), **R2 fast-forward: a lighter "FF" variant** - thin bands moving down, slight wobble, no colour change (50 % of that) - so both directions read as "tape is moving" while the picture stays readable for finding a spot. Fades in over 0.12 s, out over 0.25 s.
- **How:** a CPU pass on the 256×240 picture (≈ 0.3 ms on an M-series Mac), after the flash filter and before the renderer: the same look on Metal, Vulkan and D3D11, through the CRT (which then takes the RGB picture instead of the PPU codes, like flash-filtered frames) and in stream output. It runs only while active (rewinding, returning, fading); otherwise nothing is touched - zero cost in normal play.
- **Photosensitivity:** never a large-area flash: luminance-neutral parts dominate (desaturation, chroma, wobble); brightness changes are small (mean luminance moves < 5 %, < 1 % between frames); the noise flickers only inside the bands (< 8 % of the picture changes by > 10 % between frames, WCAG's flash area is 25 %); the bands move smoothly (no random jumps). Reduce Flashing tones it down further (Low 85 %, Standard 60 %, High 40 %). Unit-tested on black, white, grey, SMB-like and checkerboard pictures. **Settings › Display › VTR Effect** (default on) turns it off everywhere.

## First run

- No tutorial screens. The Menu pill is the only onboarding; on the very first launch it pulses twice.
