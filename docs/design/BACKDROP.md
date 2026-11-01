# Blurred sides (the backdrop)

User requests 2026-11-01: where the game picture leaves the screen empty (pillarbox / letterbox), show
the game picture enlarged, blurred and slightly dimmed; on every platform; can be turned on and off.

## Adopted

- Setting: Settings > Display > Picture Shape > Blurred Sides (`display.backdrop`, Settings::backdrop,
  on by default); DisplayPostProcess::backdrop takes it to the renderer. The Display page keeps its
  six rows (a page never scrolls), so it is on the Picture Shape page, which says so in its line.
- The plan is shared (apps/desktop/src/render/backdrop.h): shown when more than a pixel of the screen is
  empty around the picture (the CRT tube rectangle when the CRT shows); the rectangle covering the
  screen with the picture's aspect, centered; cut to the screen (cropBackdrop: a viewport within the
  device's limits, the source cut alike, for the four rotations); brightness 0.6.
- Contrast 40 % lower (user 2026-11-01: a hard flash should not flash the whole screen as hard):
  the blurred colour mixed 60 / 40 with the picture's mean colour (the mip chain's last level),
  then dimmed (kBackdropContrast).
- The blur: a copy of each new picture with its full mip chain, read at the level whose long side is
  about 12 texels (24 at first; user 2026-11-01: more clearly blurred), a 3 x 3 tent of trilinear samples there. Display only (no emulation state, no
  recording, no export).
- Vulkan (Linux / Steam Deck): VkBackdrop (vkCmdBlitImage copy and mip chain, backdrop.frag with
  the picture's blit.vert). D3D11 (Windows) and Metal (macOS, iOS): the same plan and the same blur
  (GenerateMips / generateMipmaps).

## Not adopted

- A separable Gaussian in offscreen passes: more passes and targets for the same look at this size.
- Sampling the picture itself at a large radius without mips: grainy and flickers as the picture moves.

## Done when

- Unit test of the plan (shown / not shown, covering, cut, rotations).
- A screenshot on each platform with the setting on and off; on a Steam Deck the frame time with it on.
