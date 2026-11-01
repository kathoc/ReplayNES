# The Steam menu pauses the game (Linux, gamescope)

User decision 2026-11-01: pressing the Steam button (the Steam menu) pauses the game, and on the
way back the pause menu is still open.

## Why the focus pause did not do it

RN_PRODUCT_FOCUS_PAUSE pauses on SDL_EVENT_WINDOW_FOCUS_LOST. In gamescope (Steam Deck game mode)
the Steam menu and the quick access menu are drawn by gamescope over the game: the game's window
keeps its X input focus and SDL sends no focus event (checked in the app's input log on a Deck).
What changes is the root window property `GAMESCOPE_FOCUSED_APP`: the app id gamescope shows in
front, 769 for Steam's own UI (read on a Deck: the library shown, `GAMESCOPE_FOCUSED_APP = 769`).

## Adopted

- PlatformHooks::systemOverlay: "the system's own UI is in front of the window". Linux: a thread
  polls the root property every 250 ms over its own X connection (libX11 loaded at run time, as SDL
  does; in the Flatpak runtime) and the hook returns its last answer, so the frame loop never waits
  on X. Shown = `GAMESCOPE_FOCUSED_APP` is 769. Only in gamescope (gamingMode()); elsewhere no hook.
- App: when it becomes shown, the same as a focus loss (unless Play in the Background is on): the
  game pauses, the controllers are released and kept from the game and the menus while it is shown;
  and with a game open (not the screensaver demo) the pause menu opens (UI::setMenu(true): paused,
  "quick" page). When it goes, nothing happens: the game stays paused with the menu open.
- Only after the window was in front once: while the app starts, Steam's UI is in front until
  gamescope shows the window (seen on a Deck: a game opened from the command line paused at once).
- The screensaver does not start while it is shown (as while the window is not focused).
- `--script "overlay on|off"` stands in for the hook (tools/ui-offscreen, the tests).

## Not adopted

- Comparing GAMESCOPE_FOCUSED_WINDOW with the app's own X window: SDL may run on Wayland, and a
  window launched outside Steam carries Steam's app id (the measuring runs over ssh), so it would not
  be simpler or more exact than the app id of Steam's UI.
- Reading the property on the frame thread: an X round trip each poll on the thread that paces the
  frames.
- The quick access menu (the "..." button) is Steam's UI too and pauses as well: not told apart.

## Done when

- A scripted run: a game playing, `overlay on`, `overlay off`: paused, the pause menu open
  (a screenshot), the controllers did not reach the game meanwhile.
- On a Steam Deck: the Steam button pauses the game, closing the Steam menu shows the pause menu.

## Fix (2026-11-01: "the pause menu does not open")

Launched by Steam a game runs on gamescope's second Xwayland (DISPLAY=:1); GAMESCOPE_FOCUSED_APP is
on the first one's root (:0, Steam's) only. The checks had run the app over ssh with DISPLAY=:0, so
the property was there; under Steam the watcher read :1, never saw it and never reported the menu
(the Steam log of the user's sessions: no "system overlay" line at all, and a screensaver started
while Steam had the input). The watcher now falls back to :0 when its own display lacks the property;
the Flatpak may reach that socket (--filesystem=/tmp/.X11-unix:ro).
