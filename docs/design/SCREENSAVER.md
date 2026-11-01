# Screensaver (desktop frontend)

User decisions 2026-11-01:

- After a time without any input (Settings > System > More > Screensaver: Off / 1 / 3 / 5 / 10 /
  30 min, default 5) on the library, or while a game is paused (a menu or the seek bar shown, not
  practising), the app shows game demos: a random ROM of the library, powered on and left alone (the
  board's own attract mode), switched every 2 minutes. Never while a game runs.
- Nothing is recorded: the demo is an in-memory session in RN_MODE_LIVE, installed into the
  emulation directly (AppModel::install is not used: no resume record, no play history, no library
  card picture, no project).
- The screen is dimmed (a translucent black over the picture); the UI shows nothing else but a faint
  line saying how to return. The demo is muted.
- Any input (a key, a mouse button or move, a controller button or stick) ends it; that input does
  nothing else. The app is where it was: the library, or the paused session at its point.

## Leaving a paused session

A paused session is left the way quitting the app leaves it (AppModel::requestQuit: flushForResume,
the resume record, the session closed), and reopened the way "Continue" reopens it (continueLast:
the project at the recorded point, "Resumed where you left off"). Both are the app's tested paths
for exactly this; a session whose work cannot be saved is not left (no screensaver then).

## Done when

- Unit tests: the idle rule (which states start it, which do not, the minutes), a demo session is
  live and leaves no resume record or history entry, the chosen ROMs vary.
- tools/ui-offscreen: a screenshot of the dimmed demo.
- On a Steam Deck: the library and a paused game both go to the demos after the time, and come back.
