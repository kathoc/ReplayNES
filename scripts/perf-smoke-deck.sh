#!/bin/bash
# Frame pacing / latency measurement of the Linux frontend ON THE STEAM DECK (the counterpart of
# scripts/perf-smoke.sh for macOS). Runs replaynes-linux --perf-seconds over ssh in the Deck's
# current graphical session and prints its summary (sample -> on screen, judder, missed targets,
# audio underruns, CPU; see apps/linux/src/perf_stats.h).
#
#   scripts/perf-smoke-deck.sh [seconds] [rom-on-deck]      (default 30 s, generated test ROM)
#   ROM=smb ...           ~/Documents/ReplayNES/ROM/Super Mario Bros. (World).NES on the Deck
#   SESSION=gaming        run inside Gaming Mode's gamescope (X11 on :0; the default when
#                         gamescope-session is running). A window started over ssh is not shown by
#                         gamescope (Steam's UI keeps the focus, and presents of the hidden window
#                         still "complete"), so the script gives it the focus for the run: the
#                         window gets STEAM_GAME=769 and the root GAMESCOPECTRL_BASELAYER_WINDOW is
#                         set to it (reset to 0 afterwards). A game launched from Steam (non-Steam
#                         shortcut, docs/STEAM_DECK.md) is focused by Steam itself.
#   SESSION=desktop       Plasma (Wayland),
#   SESSION=nested        Desktop Mode, nested gamescope at 60 Hz (gamescope -r 60 -f --)
#   NESTED_RATE=45        run inside a nested gamescope at that refresh (any session; e.g. a display
#                         slower than the NES rate: 2 frames per present when needed)
#   BIN=dev               use ~/ReplayNES-dev/build-dev/replaynes-linux (DEV=1 build) inside the
#                         installed app's sandbox (default: the installed Flatpak)
#   WARMUP=8 INPUT=1 FLASH=0 LABEL=name EXTRA_ARGS="--windowed" DRIVER=wayland|x11
# Results append to ~/.var/app/io.github.replaynes.ReplayNES/data/perf/stats.jsonl on the Deck.
set -euo pipefail
HOST="${HOST:-deck@steamdeck.local}"
DUR="${1:-30}"
ROMARG="${2:-}"
WARMUP="${WARMUP:-8}"
APP_ID=io.github.replaynes.ReplayNES
case "${ROM:-}" in
  smb) ROMARG="/home/deck/Documents/ReplayNES/ROM/Super Mario Bros. (World).NES" ;;
esac
if [ -z "$ROMARG" ]; then ROMARG="/home/deck/Documents/ReplayNES/perf/replaynes-test.nes"; fi

ARGS="--perf-seconds $DUR --warmup $WARMUP --label '${LABEL:-run}'"
[ "${INPUT:-0}" = "1" ] && ARGS="$ARGS --inject-input"
[ -n "${FLASH:-}" ] && ARGS="$ARGS --flash $FLASH"
ARGS="$ARGS ${EXTRA_ARGS:-}"
CMD="flatpak run $APP_ID"
[ "${BIN:-}" = "dev" ] && CMD="flatpak run --filesystem=home ${FLATPAK_ARGS:-} --command=/home/deck/ReplayNES-dev/build-dev/replaynes-linux $APP_ID"

VARS="$(printf 'APP_ID=%q ROMARG=%q CMD=%q ARGS=%q SESSION=%q DRIVER=%q NESTED_RATE=%q' "$APP_ID" "$ROMARG" "$CMD" "$ARGS" "${SESSION:-}" "${DRIVER:-}" "${NESTED_RATE:-}")"
ssh "$HOST" "$VARS bash -s" <<'EOF'
set -e
export XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
if [ -z "$SESSION" ]; then
  if pgrep -x gamescope >/dev/null || pgrep -x gamescope-wl >/dev/null; then SESSION=gaming; else SESSION=desktop; fi
fi
D=~/.var/app/$APP_ID/data/perf
mkdir -p "$D" ~/Documents/ReplayNES/perf
[ -f ~/Documents/ReplayNES/perf/replaynes-test.nes ] ||
  flatpak run --command=replaynes-cli "$APP_ID" make-test-rom ~/Documents/ReplayNES/perf/replaynes-test.nes >/dev/null
rm -rf "$D/session"
PRE=""
case "$SESSION" in
  gaming)  export DISPLAY=:0 GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 XDG_CURRENT_DESKTOP=gamescope ;;
  desktop|nested)
    # The Plasma session's display variables (SteamOS 3 Desktop Mode is Plasma on X11 or Wayland).
    P=$(pgrep -x plasmashell | head -1)
    eval "$(tr '\0' '\n' </proc/$P/environ | grep -E '^(DISPLAY|XAUTHORITY|WAYLAND_DISPLAY|XDG_SESSION_TYPE)=' | sed 's/^/export /')"
    export XDG_CURRENT_DESKTOP=KDE
    [ "$SESSION" = nested ] && PRE="gamescope -W 1280 -H 800 -r 60 -f --" ;;
esac
[ -n "$DRIVER" ] && export SDL_VIDEO_DRIVER="$DRIVER"
WINPAT='^ReplayNES$'
if [ -n "$NESTED_RATE" ]; then
  PRE="gamescope -W 1280 -H 800 -r $NESTED_RATE --"
  WINPAT='^(ReplayNES|gamescope)$'
fi
echo "session: $SESSION"
if [ "$SESSION" = gaming ]; then
  # Give the (ssh-started) window gamescope's focus for the run; restored below.
  ( for i in $(seq 1 60); do
      W=$(xdotool search --name "$WINPAT" 2>/dev/null | head -1)
      if [ -n "$W" ]; then
        sleep 0.5
        xprop -id "$W" -f STEAM_GAME 32c -set STEAM_GAME 769
        xprop -root -f GAMESCOPECTRL_BASELAYER_WINDOW 32c -set GAMESCOPECTRL_BASELAYER_WINDOW "$W"
        break
      fi
      sleep 0.2
    done ) &
fi
eval "$PRE $CMD --rom \"\$ROMARG\" --session-root \"\$D/session\" --stats-log \"\$D/stats.jsonl\" --frame-log \"\$D/frames-\$SESSION.csv\" $ARGS" 2>&1 |
  grep -vE "^(Gtk|dbus|$)" | tail -25
if [ "$SESSION" = gaming ]; then
  xprop -root -f GAMESCOPECTRL_BASELAYER_WINDOW 32c -set GAMESCOPECTRL_BASELAYER_WINDOW 0
fi
true
EOF
