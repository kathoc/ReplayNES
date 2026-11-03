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
- Levels (user 2026-11-01: adjustable, low by default): Off / Low / Standard / High = the picture
  seen through about 24 / 12 / 6 texels (Low at 10 was too blurred, user 2026-11-01) (backdropTexels); Settings > Display > Picture Shape >
  Blurred Sides is a choice now (desktop `backdrop=0..3`, an older `1` is Low; Apple `backdropLevel`).
- With the CRT on (the heavy GPU load) the copy is made for every other picture: 30 fps of blur
  (user: 30 fps is fine under load).
- Vulkan (Linux / Steam Deck): VkBackdrop (vkCmdBlitImage copy and mip chain, backdrop.frag with
  the picture's blit.vert). D3D11 (Windows) and Metal (macOS, iOS): the same plan and the same blur
  (GenerateMips / generateMipmaps).

## Not adopted

- A separable Gaussian in offscreen passes: more passes and targets for the same look at this size.
- Sampling the picture itself at a large radius without mips: grainy and flickers as the picture moves.

## Done when

- Unit test of the plan (shown / not shown, covering, cut, rotations).
- A screenshot on each platform with the setting on and off; on a Steam Deck the frame time with it on.

## Levels, 2026-11-03

User: the low level was still too blurred; wanted なし / くっきり / 弱 / 中 / 強 with the new 強 blurring less than
the former 弱, and a level with no blur at all. Levels (Settings > Display > Picture Shape > Blurred Sides, saved as
`backdrop2`): 0 none, 1 sharp (the picture enlarged and dimmed, read at mip level 0 without the 3x3 tent:
BackdropPlan::spread 0), 2 low (160 texels across, the default), 3 medium (96), 4 high (48; the former low was
24). The older key `backdrop` (0 off, 1-3) reads as none or the default; macOS likewise (`backdropLevel2`).
Dimming and the lower contrast are the same at every level. Checked in the Android emulator (1942): the band
above the picture's detail (mean absolute neighbour difference) 0.211 sharp, 0.024 low, 0.004 medium, 0.001
high, 0 none, the same mean brightness.
