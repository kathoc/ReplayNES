# UI/UX redesign (2026-10-08) — single source of truth for all frontends

Goal: intuitive for beginners and experts, no manual needed, minimal text, modern look, no scrolling.
Applies to macOS (SwiftUI), Linux/Steam Deck and Windows (shared ImGui frontend `apps/desktop`).

## Input model

| Input | During play | In menus |
|---|---|---|
| **L+R together** (both pressed within 100 ms; either order) | open Quick Menu (pauses) | close Quick Menu (resume) |
| R alone | pause / resume — shows only the seek bar (no menu) | next page (L1/R1 page switching) |
| L alone | slow ½ toggle | previous page |
| L2 / R2 hold | rewind / fast-forward | rewind / fast-forward |
| D-pad / stick | game | move focus |
| A (south on Xbox layout = confirm) / B (back) | game | confirm / back (B on the top level = resume) |
| Y / X | game | contextual (rename / clear …), shown in the hint bar |
| Keyboard | — | Esc = open/close menu, Enter = confirm, Backspace = back, arrows = move |

- Chord detection lives in the shared core (`frontend/`, C API) so all platforms behave identically: L or R alone fire on release-or-timeout (100 ms) if the other shoulder wasn't pressed; L+R fires once both are down. L/R alone lose ≤100 ms of responsiveness — acceptable for pause/slow.
- Bindings stay remappable (the chord is an action "hk.menu" bound to a two-element combo).

## Always-visible menu affordance

- A small pill **"☰ Menu  L+R"** (en) / **"☰ メニュー  L+R"** (ja) is always on screen in a corner (top-right of the game area, not over the picture when letterboxed; in FILL it sits over the picture with low contrast and fades to 30 % after 3 s of play, back to 100 % on mouse move / touch / pause).
- It is a button: click / tap opens the Quick Menu (rescue when no controller). The glyph changes to the connected family (Steam Deck/Xbox: L+R, PlayStation: L1+R1, Nintendo: L+R, keyboard only: Esc).
- In full-screen direct-to-display mode the pill must not defeat the low-latency path: draw it inside the game's own render pass (Metal/Vulkan/D3D11), not as an OS overlay.

## Quick Menu (top level — 6 tiles, one row, nothing else)

| Tile | Icon (Tabler/SF Symbol) | Contents (next level, ≤ 6 items each) |
|---|---|---|
| 再開 / Resume | play | (closes menu) — default focus |
| やり直す / Retry | history | ここから録り直す · 前の試行へ · テイク一覧 · ブックマーク |
| 練習 / Practice | target | 8 A/B slots as 4×2 cards (thumbnail of A, name, length); A = practice, Y = rename, X = clear; empty slot = "＋" (set A here) |
| 共有 / Share | share | MP4に書き出す · 配信出力 (Syphon/Spout) |
| 設定 / Settings | settings | 画面 · 操作 · 音 · システム (L1/R1 switch between these four) |
| ゲーム / Game | device-gamepad | ゲーム選択 · 保存 · 別名で保存 · リセット |

### Settings pages (≤ 6 rows each, 2 columns; deeper detail one level further)

- **画面 / Display:** サイズ (等倍 | FILL) · ブラウン管 (on/off) · 8:7 (on/off) · 点滅を抑える (切 弱 中 強) · 端を隠す (on/off) · ブラウン管の詳細 ›
- **操作 / Controls:** コントローラー › (diagram page) · キーボード › · 連射の速さ (slider) · 一時停止中の十字キー (on/off) · 画面キーボード (自動 | 内蔵 | Steam; Linux only)
- **音 / Sound:** 音量 (slider) · スロー中の音 (on/off if supported) · 低遅延 (on/off where applicable)
- **システム / System:** 言語 (自動 | 日本語 | English) · アップデート › · Steamに追加 (Linux) · バージョン情報 ›

Rules: no screen ever scrolls; if a page needs more than 6 rows, split it into a "… の詳細 ›" sub-page. Max depth 3 (Quick Menu › 設定 › 画面 › ブラウン管の詳細).

## Visual language

- Game frame stays visible behind menus, dimmed to ~35 % (not blurred — keep it cheap on all GPUs).
- Panels: dark translucent (#141416 at 92 %), corner radius 16 px (tiles 12 px), 24 px outer margin, 10 px gaps.
- Focus: 2 px brand-red (#FF3B3B) ring + slightly raised tile tint; 120 ms ease-out on focus move / open / close.
- Typography: one family, three sizes (title 22, label 15, hint 12). Labels 2–5 characters (ja) / 1–2 words (en). Never paragraphs: the only explanatory text is a single line at the bottom describing the focused item.
- Hint bar bottom-right: button glyphs for the connected controller family + ≤ 3 verbs.
- Breadcrumb top-left on sub-levels (icon + names), e.g. ⚙ 設定 › 画面.
- Icons: macOS SF Symbols; desktop: an icon font subset (Tabler Icons MIT, or Material Symbols Apache-2.0) bundled and recorded in THIRD_PARTY_NOTICES.md.
- Light/dark: menus are always dark (they sit over game video).

## Library (start screen)

- The app always starts here (no automatic resume). Top: a large "続きから / Continue" hero card (last session's thumbnail + game name + time) — A resumes the session of the resume record (a temporary session or a project, journal recovered after a crash / force quit), else opens the latest project. Starting another game while a temporary session waits there asks first (save / don't save / cancel).
- Top bar (between the name and the Menu pill): filter chips すべて / お気に入り / 最近 (All / Favorites / Recent: the last 24 games played, newest first, with when and how long), the sort (名前 / 最近遊んだ / メーカー / 発売年 / ジャンル — Name / Last Played / Maker / Year / Genre; cycles) and the search (on-screen keyboard). The controller reaches the row with up (order: row, hero, cards).
- Below: a grid of large game cards (thumbnail from the latest project/screenshot, else a generated tile with the game name), 4 per row at 1280×800, showing the game's title from the built-in game database (`frontend/data/nesdb.tsv`, shared core) in the UI language and "maker · year" small; a star badge on favourites. The focused game's "maker · year · genre" is the one-line description. A = play, Y = favourite on / off, X = projects of the focused game, View / Select (⧉; keyboard Tab / S) = next sort, Menu pill → Settings.
- Order: ja = gojūon order of the kana reading (dakuten / handakuten and small kana folded, ー read as the preceding vowel; Latin official titles such as FRONTLINE read フロントライン), en = title ignoring a leading "The"; ROMs not in the database keep their file name and come after the kana titles. Favourites / history / sort / filter persist in `library.json` (settings folder, written by the core).
- Empty library: a single card explaining where to put ROMs (folder path + "Open folder").

## Seek bar while paused (R)

- Only the filmstrip seek bar + time + the Menu pill; L2/R2 scrub; A = resume; the A/B lane stays.
- A (and B on Linux / Windows) resume as a **tap**: pressed while paused they still reach the game, and resume on release only if nothing else was pressed or stepped while held. So a button can be held across D-pad frame steps (hold B to run, step with →) without resuming; a quick tap still resumes. (Considered: resuming only with a focused "Resume" affordance on the seek bar — rejected, the seek bar has no focus and R already resumes; consuming A outright — rejected, it broke held-button frame advance.)

## First run

- No tutorial screens. The Menu pill is the only onboarding; on the very first launch it pulses twice.
