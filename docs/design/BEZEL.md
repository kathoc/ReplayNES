# Bezel (desktop frontend)

A product may ship a bezel: a full-screen picture with a transparent window the game picture is
shown in (ReplayArcade: an arcade cabinet's screen surround, 1586 x 992, about 16:10 like the Steam
Deck's screen). ReplayNES ships none and behaves exactly as before.

User decision 2026-11-01: shown by default for horizontal games on Steam Deck / PC; never for
vertical (rotated) games; a setting turns it off. (The iPhone app gets it separately, in Swift.)

## Product inputs (CMake, the product profile)

- `RN_PRODUCT_BEZEL_PNG`: path of the PNG (RGBA; alpha 0 in the window, the frame's own shading
  around the window opaque). Unset: no bezel feature at all (no setting row either).
- `RN_PRODUCT_BEZEL_WINDOW`: the window in the PNG's pixels, `"x y width height"`
  (ReplayArcade: `"321 98 944 739"`).

The PNG is compiled into the executable (the way the icon font is: a generated `.inc` of bytes), so
the Flatpak, the Windows build and the tests need no extra files at run time. Decoding: the
existing PNG reader if there is one, else stb_image (third_party) - check what the tree has first.

## Layout (computeGameRect)

With the bezel shown:

1. The bezel is fitted into the window (screen) keeping its aspect ratio, centred; the bars
   outside it (other aspect ratios than the bezel's) are black.
2. Its window, scaled the same way, is the area the game picture is laid out in, exactly as the
   game is laid out in the whole screen today (aspect ratio, 8:7 / PAR, integer scale when on).
   The game is drawn first, the bezel over it (ImGui's background draw list, the first thing of
   the UI pass), so the frame's inner shading covers the picture's edge and the transparent window
   shows the game.
3. Everything else (menus, seek bar, pills, indicator, notices) is laid out over the whole screen
   as today.

Not shown (layout exactly as today) when: the product has no bezel; the setting is off; the game
is vertical (its displayed picture is taller than wide, after rotation); no session (library).

## Renderer

A new `Renderer` call for one large static texture (the bezel; the thumbnail atlas is 128 x 120
tiles and too small):

    virtual bool staticTexture(uint64_t key, const uint32_t* rgba, uint32_t width, uint32_t height,
                               uint64_t* texId) = 0;  // uploaded once, kept until shutdown

Implemented in every renderer: Vulkan (Linux), D3D11 (Windows), the software renderer of
`tools/ui-offscreen`, and any test double. Pixel order as the thumbnails' (check the existing
upload path: BGRA vs RGBA).

## Setting

`Settings::bezel` (bool, default on), saved as `bezel=1`. A TOGGLE row "Bezel" ("ベゼル",
description "A cabinet frame around horizontal games" / "横画面のゲームの周りに筐体の枠を表示") on
Settings > Display, only when the product has a bezel (a new `RNF_MENU_FEATURE_BEZEL`). Every menu
page keeps at most 6 rows (tests/test_frontend_menu.cpp checks it with all features on): if the
Display page would exceed that, put the row on the page where the picture-shape rows are, and say
where in the change notes. Strings go into apps/macos/Resources/Localizable.xcstrings (append, keep
the order; `scripts/check-l10n.py` must report 0 problems).

## Done when

- Unit tests: computeGameRect with a bezel (16:10 screen, 16:9 screen, 4:3 game, rotated game ->
  no bezel, integer scale inside the window), the setting round trip, the menu row only with the
  feature.
- `tools/ui-offscreen` writes a screenshot of a horizontal game with the bezel (and one without).
- ReplayNES (no `RN_PRODUCT_BEZEL_PNG`) builds and its tests pass unchanged.
