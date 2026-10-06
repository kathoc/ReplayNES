#!/bin/bash
# Frame pacing / latency measurement of the Linux frontend ON THE STEAM DECK (the counterpart of
# scripts/perf-smoke.sh for macOS). Runs replaynes-linux --perf-seconds over ssh in the Deck's
# current graphical session and prints its summary (sample -> on screen, judder, missed targets,
# audio underruns, CPU; see apps/linux/src/perf_stats.h).
#
#   scripts/perf-smoke-deck.sh [seconds] [rom-on-deck]      (default 30 s, generated test ROM)
#   ROM=smb ...           ~/Documents/ReplayNES/ROM/Super Mario Bros. (World).NES on the Deck
#   SESSION=gaming        run inside Gaming Mode's gamescope (X11 on :0; the default when
#                         gamescope-session is running), SESSION=desktop for Plasma (Wayland),
#   SESSION=nested        Desktop Mode, nested gamescope at 60 Hz (gamescope -r 60 -f --)
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

ssh "$HOST" "bash -s" <<EOF
set -e
export XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
SESSION="${SESSION:-}"
if [ -z "\$SESSION" ]; then pgrep -x gamescope-wl >/dev/null || pgrep -x gamescope >/dev/null && SESSION=gaming || SESSION=desktop; fi
D=~/.var/app/$APP_ID/data/perf
mkdir -p \$D ~/Documents/ReplayNES/perf
[ -f ~/Documents/ReplayNES/perf/replaynes-test.nes ] || flatpak run --command=replaynes-cli $APP_ID make-test-rom ~/Documents/ReplayNES/perf/replaynes-test.nes >/dev/null
rm -rf \$D/session
PRE=""
case "\$SESSION" in
  gaming)  export DISPLAY=:0 GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 XDG_CURRENT_DESKTOP=gamescope ;;
  desktop) export WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 XDG_CURRENT_DESKTOP=KDE ;;
  nested)  export WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 XDG_CURRENT_DESKTOP=KDE; PRE="gamescope -W 1280 -H 800 -r 60 -f --" ;;
esac
[ -n "${DRIVER:-}" ] && export SDL_VIDEO_DRIVER=${DRIVER:-}
echo "session: \$SESSION"
\$PRE $CMD --rom "$ROMARG" --session-root \$D/session --stats-log \$D/stats.jsonl --frame-log \$D/frames-\$SESSION.csv $ARGS 2>&1 \
  | grep -vE "^(Gtk|dbus|$)" | tail -25
EOF
