# Product hooks and the input UI for other cores (2026-10-24)

Follows `2026-10-24-product-profile.md`, `2026-10-24-generic-core-boundary.md` and
`2026-10-24-media-frontend.md`. The engine runs any registered core (`RN_CORE_EXTERNAL`) with its
own media and input layout. This step lets a product built from these sources (ReplayArcade: an
arcade core, zip sets, two players with up to six buttons, coins, analog sticks) plug its core in
and makes the Linux app's library and controls work for it. **ReplayNES must not change.**

## S5a: product hooks

Profile variables (`cmake/products/replaynes.cmake` keeps today's behaviour):

| Variable | ReplayNES | Meaning |
|---|---|---|
| `RN_PRODUCT_STARTUP_TARGET` | empty | CMake target linked into the Linux app that defines `extern "C" void rn_product_startup(void)`; `main` calls it before the app starts (the product registers its core there) |
| `RN_PRODUCT_DEFAULT_CORE` | `nestopia` | the default of the `core` setting; `external` selects `RN_CORE_EXTERNAL` |
| `RN_PRODUCT_GAMEDB` | `frontend/data/nesdb.tsv` | the game database compiled into the frontend (same TSV format; aliases are matched against file names, so an arcade table lists set names as aliases) |
| `RN_PRODUCT_NES` | `ON` | NES-only features: the Test Cartridge, the NES core choice in Settings, identifying ROMs by NES PRG/CHR hashes |

Frontend: a content identity provider,
`typedef int (*rnf_content_id_fn)(const char* path, char* out, size_t cap, void* ctx);`
`void rnf_set_content_id_provider(rnf_content_id_fn fn, void* ctx);` (NULL = SHA-256 of the file).
When set, the ROM hash cache, the library's project matching and the catalog (favourites, recent)
use its id instead of the file's SHA-256 (the engine writes the core's `contentId()` into projects
as `rom.sha256` with `rom.identity = "content"`). With `RN_PRODUCT_NES` off, games are identified by
file name only.

The `core` setting accepts `external`; the core menu is shown only when more than one core is
available. With `RN_PRODUCT_NES` off the Test Cartridge entries are not shown.

## S5b: controls for the session's input layout

- Action catalog from the layout (`rn_session_input_layout_json`): one action per button (label: the
  layout's label, else the frontend's name for the id: "Up", "Coin", "Start", "Button 1" ...), one
  turbo action per button (`<turbo id>`; label "<button> (Turbo)"), one per axis. Groups:
  `RNF_GROUP_PLAYER1..4` by the `p<n>.` prefix, `RNF_GROUP_SYSTEM` for ids without a player prefix,
  hotkeys as today. The NES layout gives exactly today's catalog, labels and order.
- Bindings are stored per layout name (`bindings.json` for "nes", unchanged; `bindings-<layout>.json`
  otherwise). Defaults for a non-NES layout (applied when no file exists), by position: `b1` east,
  `b2` south, `b3` north, `b4` west, `b5` / `b6` the shoulders when the hotkey scheme leaves them
  free, otherwise unbound; `coin` View/Back, `start` Menu; directions on the d-pad and the left stick;
  axes: `axis.x` / `axis.y` the left stick (`gcN:lstick.x`, `gcN:lstick.-y`), `axis.rx` / `axis.ry` the
  right stick; player N on controller N.
- Settings › Controls gets a Turbo page: the global period and press length (today's two rows) and
  per button "global" or its own period / press length (`rn_input_set_button_turbo`), saved with the
  bindings. The turbo rate label is computed with the session's frame rate.
- Labels like "1P" / "2P" come from the group (P1..P4).
- The resume countdown, the paused d-pad step and anything else that reads NES bits use the
  layout's direction ids of player 1.

## Done when

- `build-make`: all ctest tests pass (golden, media, input layout, desktop frontend).
- A smoke profile with `RN_PRODUCT_NES OFF`, `RN_PRODUCT_DEFAULT_CORE external`, a startup target that
  registers a test core with a non-NES layout, and a small TSV builds; its `test_desktop_frontend`
  covers: catalog and defaults from the layout, per-layout bindings files, the Turbo page model,
  content ids from the provider in library matching, no Test Cartridge.
- The Linux app compiles in the Freedesktop SDK; ReplayNES's `--version` and behaviour unchanged.
