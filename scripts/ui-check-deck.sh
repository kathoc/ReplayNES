#!/bin/bash
# Scripted UI check of the Linux frontend ON THE STEAM DECK: runs replaynes-linux --script over ssh
# in the Deck's current graphical session (Gaming Mode's gamescope by default, focused like
# scripts/perf-smoke-deck.sh does) and copies the screenshots it takes (the presented frames:
# game + UI, PNG) to build/ui-shots/ on this machine.
#
#   scripts/ui-check-deck.sh "wait 3; shot 01-library.png; menu; shot 02-menu.png; quit"
#   REPLAYNES_LANG=ja scripts/ui-check-deck.sh "..."    Japanese UI
#   ROM=smb ...           start with Super Mario Bros. from the Deck's ROM folder (temporary session)
#   BIN=dev REMOTE_DIR=ReplayNES-dev   the DEV=1 build in ~/$REMOTE_DIR/build-dev (default: installed)
#   SESSION=gaming|desktop   KEEP_SESSION=1 (reuse the scratch session folder: resume tests)
#   LIBRARY=scratch       a scratch library (~/.var/app/<id>/data/ui-check/library) whose ROM folder
#                         hard-links Super Mario Bros. and the generated test ROM (two games to switch
#                         between; projects made by the run stay out of the user's library)
# Script commands: apps/desktop/src/script.h. "shot NAME" writes into the remote shots folder;
# "pad a|b|x|y|up|down|left|right|l1|r1|l2|r2|menu|view|r3" injects a controller press (SDL events
# of pad 0, through the same path as a real press).
# The app runs with a scratch session folder (~/.var/app/<id>/data/ui-check/session), so the
# user's own session is never touched; the library is the real ~/Documents/ReplayNES unless
# LIBRARY=scratch.
set -euo pipefail
HOST="${HOST:-deck@steamdeck.local}"
APP_ID=io.github.replaynes.ReplayNES
SCRIPT_CMDS="${1:-wait 3; shot library.png; quit}"
RDIR="${REMOTE_DIR:-ReplayNES-dev}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/ui-shots"
mkdir -p "$OUT"
ROMARG=""
case "${ROM:-}" in
  smb) ROMARG="/home/deck/Documents/ReplayNES/ROM/Super Mario Bros. (World).NES" ;;
  "") ;;
  *) ROMARG="$ROM" ;;
esac
CMD="flatpak run --filesystem=home $APP_ID"
[ "${BIN:-}" = "dev" ] && CMD="flatpak run --filesystem=home --command=/home/deck/$RDIR/build-dev/replaynes-linux $APP_ID"

VARS="$(printf 'APP_ID=%q CMD=%q SCRIPT_CMDS=%q ROMARG=%q SESSION=%q RLANG=%q KEEP=%q LIBRARY=%q' "$APP_ID" "$CMD" "$SCRIPT_CMDS" "$ROMARG" \
  "${SESSION:-}" "${REPLAYNES_LANG:-}" "${KEEP_SESSION:-0}" "${LIBRARY:-}")"
ssh "$HOST" "$VARS bash -s" <<'EOF'
set -e
export XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
if [ -z "$SESSION" ]; then
  if pgrep -x gamescope >/dev/null || pgrep -x gamescope-wl >/dev/null; then SESSION=gaming; else SESSION=desktop; fi
fi
if pgrep -x replaynes-linux >/dev/null; then
  echo "another replaynes-linux is running:"; pgrep -ax replaynes-linux | cut -c1-160; exit 3
fi
D=~/.var/app/$APP_ID/data/ui-check
SHOTS=$D/shots
rm -rf "$SHOTS"; mkdir -p "$SHOTS"
[ "$KEEP" = "1" ] || rm -rf "$D/session"
# "shot NAME" -> "shot <shots dir>/NAME"
CMDS=$(printf '%s' "$SCRIPT_CMDS" | sed -E "s#shot ([^;/ ]+)#shot $SHOTS/\1#g")
case "$SESSION" in
  gaming)  export DISPLAY=:0 GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 XDG_CURRENT_DESKTOP=gamescope ;;
  desktop)
    P=$(pgrep -x plasmashell | head -1)
    eval "$(tr '\0' '\n' </proc/$P/environ | grep -E '^(DISPLAY|XAUTHORITY|WAYLAND_DISPLAY|XDG_SESSION_TYPE)=' | sed 's/^/export /')"
    export XDG_CURRENT_DESKTOP=KDE ;;
esac
[ -n "$RLANG" ] && export REPLAYNES_LANG="$RLANG"
echo "session: $SESSION"
if [ "$SESSION" = gaming ]; then
  ( for i in $(seq 1 60); do
      W=$(xdotool search --name '^ReplayNES$' 2>/dev/null | head -1)
      if [ -n "$W" ]; then
        sleep 0.5
        xprop -id "$W" -f STEAM_GAME 32c -set STEAM_GAME 769
        xprop -root -f GAMESCOPECTRL_BASELAYER_WINDOW 32c -set GAMESCOPECTRL_BASELAYER_WINDOW "$W"
        break
      fi
      sleep 0.2
    done ) &
fi
ARGS=(--session-root "$D/session" --script "$CMDS")
if [ "$LIBRARY" = scratch ]; then
  L=$D/library
  [ "$KEEP" = "1" ] || rm -rf "$L"
  mkdir -p "$L/ROM" ~/Documents/ReplayNES/perf
  [ -f ~/Documents/ReplayNES/perf/replaynes-test.nes ] ||
    flatpak run --command=replaynes-cli "$APP_ID" make-test-rom ~/Documents/ReplayNES/perf/replaynes-test.nes >/dev/null
  # Hard links (the library skips symbolic links); same file system as ~/Documents.
  ln -f ~/Documents/ReplayNES/perf/replaynes-test.nes "$L/ROM/ReplayNES Test ROM.nes"
  ln -f "$HOME/Documents/ReplayNES/ROM/Super Mario Bros. (World).NES" "$L/ROM/Super Mario Bros. (World).NES"
  ARGS+=(--library-root "$L")
fi
[ -n "$ROMARG" ] && ARGS+=(--rom "$ROMARG")
$CMD "${ARGS[@]}" 2>&1 | grep -vE "^(Gtk|dbus|$)" | tail -40 || true
if [ "$SESSION" = gaming ]; then
  xprop -root -f GAMESCOPECTRL_BASELAYER_WINDOW 32c -set GAMESCOPECTRL_BASELAYER_WINDOW 0
fi
ls "$SHOTS"
EOF
scp -q "$HOST:.var/app/$APP_ID/data/ui-check/shots/*.png" "$OUT/" 2>/dev/null || true
echo "screenshots: $OUT"
