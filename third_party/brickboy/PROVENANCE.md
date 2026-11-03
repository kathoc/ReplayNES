# Brickboy touch controller (via NESTERM) - provenance

The iPhone on-screen controller (`Sources/Play/Brickboy/`) is a Swift translation of the touch
controller NESTERM vendors from Brickboy. It was written by reading the JavaScript and CSS and
translating them function by function; nothing was re-invented from screenshots or descriptions.

| | |
|---|---|
| Upstream | `kathoc/brickboy` @ `9f26b5a2f9d598caff72b4192e0e6bc15ac990e5`, `src/ui/input.ts` |
| Via | NESTERM `web/brickboy/input.mjs` (generated from the above, SHA-256 of the upstream file `1e47a930f6ba89264745fc1d02c5f2837e6deb8b32743c82111638ac24b2ba09`), with NESTERM's modifications of 2026-10-05 (roll-tap reading of the A/B pad: `ROLL_RETURN`, `ROLL_ENGAGE`, `ROLL_WINDOW_MS`, `rollTap`) |
| Also read | NESTERM `web/brickboy/buttons.mjs`, `web/input-router.mjs`, `web/controller.mjs`, `web/styles.css`, `web/index.html`, `web/app.mjs`, `scripts/controller-check.mjs`, `docs/controller-port-spec.md` (NESTERM checkout of 2026-10-05) |
| Copyright | 2026 Katsuya Hokamura |
| License | Apache License 2.0 (`LICENSE` in this folder) |

Files and what they derive from:

| ReplayNES file | Derived from | License |
|---|---|---|
| `Sources/Play/Brickboy/TouchGamepad.swift` | `input.mjs` (`TouchGamepad`, constants, helpers), `buttons.mjs` | Apache-2.0 |
| `Sources/Play/Brickboy/TouchLayout.swift` | the controller geometry of `styles.css` (+ ReplayNES's own extras row, `TouchButton`, `TouchBindings`) | Apache-2.0 AND GPL-2.0-or-later |
| `Sources/Play/Brickboy/TouchControlsView.swift` | the markup of `input.mjs`'s constructor and the controller rules of `styles.css` (+ UIKit glue) | Apache-2.0 AND GPL-2.0-or-later |
| `Tests/BrickboyGoldenTests.swift`, `Tests/Fixtures/golden-*.json`, `docs/nesterm-golden/` | traces recorded from the unmodified `input.mjs` | Apache-2.0 |
| `Tests/TouchControlsTests.swift` (`NESTERMControllerCheckTests`) | the expectations of `scripts/controller-check.mjs` | Apache-2.0 AND GPL-2.0-or-later |

Modifications for ReplayNES (2026): translated to Swift / UIKit; browser plumbing replaced (see
`docs/TOUCH_PORT.md` for every mapping and deviation); the web build's diagnostics (debug overlay,
gesture recording, CSS rotation option, keyboard path, autofire ring) not ported.

Combination: ReplayNES is GPL-2.0-or-later; Apache-2.0 code can be combined with it under the
GPL version 3 terms.
