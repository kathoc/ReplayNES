# Testing the macOS app in a clean macOS VM

A macOS 27 guest in Parallels Desktop (Apple Virtualization backend) for checking the built
ReplayNES.app on a pristine system: first launch / Gatekeeper, empty preferences and library,
Japanese vs English UI, no Xcode or developer tools. Scripts: `scripts/macos-vm/`. State outside the
repo: the VM in `~/Parallels/`, SSH host keys in `~/.local/state/replaynes-vm/`. ROMs and VM images
never go into the repo. (An earlier version of these scripts used tart; it is gone.)

The VM is generic: nothing project-specific lives in it, so it can be reused for other projects.
`REPLAYNES_VM` (default `MacOS`) picks the VM; `SNAPSHOT` (default `provisioned`) the snapshot reset.sh uses.

## Setup (once, by hand)

1. Parallels Desktop (licensed), `prlctl` in PATH (`/usr/local/bin`).
2. Create a macOS VM named `MacOS` (Parallels: New > Install macOS from the IPSW), click through
   Setup Assistant with a local account `admin` / `admin`, install Parallels Tools when asked.
3. In the guest: System Settings > General > Sharing > Remote Login on; add the host's
   `~/.ssh/id_ed25519.pub` to the guest's `~/.ssh/authorized_keys`.
4. Stop the VM and take the snapshot: `prlctl snapshot MacOS --name clean`.
5. `SNAPSHOT=clean scripts/macos-vm/reset.sh && scripts/macos-vm/start.sh && scripts/macos-vm/stop.sh`,
   then `prlctl snapshot MacOS --name provisioned` — the ready-to-test state reset.sh uses by default
   (skips the ~1 min provisioning + reboot on each start).

Everything else (passwordless sudo, no sleep / screen saver / lock, auto-login, no update checks) is
done by `provision.sh`, which `start.sh` runs on every start (idempotent). Note the snapshot does
not need to contain it: after `reset.sh` the first `start.sh` re-provisions and, when auto-login had
to be re-enabled, reboots the guest once (~1 minute extra).

Auto-login: `sysadminctl -autologin set` fails over SSH (`SACSetAutoLoginPassword error:22`), so
provision.sh writes `/etc/kcpassword` itself. The guest needs a desktop session for GUI apps.

## Daily use

```sh
scripts/macos-vm/start.sh                         # boot (headless), provision, wait for SSH + desktop session
scripts/macos-vm/deploy.sh                        # build/ReplayNES.app -> guest /Applications
scripts/macos-vm/deploy.sh dist/ReplayNES-0.4.0-macOS-arm64.zip    # or a release zip
build/tools/replaynes-cli/replaynes-cli make-test-rom /tmp/test.nes
ROM=/tmp/test.nes scripts/macos-vm/run-app.sh     # copy the ROM in, launch --rom ... --autoplay
UI_LANG=en ROM=/tmp/test.nes scripts/macos-vm/run-app.sh            # other UI language, this run only
scripts/macos-vm/screenshot.sh /tmp/shot.png      # PNG of the VM's screen
scripts/macos-vm/ssh.sh 'command'                 # shell / command in the guest
scripts/macos-vm/controllers.sh                   # see "Game controllers"
scripts/macos-vm/stop.sh
scripts/macos-vm/reset.sh                         # stop + roll back to snapshot "provisioned" (SNAPSHOT=clean: untouched install)
```

- `run-app.sh` passes arguments to the app (`--rom`, `--project`, `--library-root`, ...; paths are
  guest paths) and launches through `open` (LaunchServices + Gatekeeper, like Finder).
  `FRESH=1` wipes the app's preferences, Application Support and `~/Documents/ReplayNES` first.
- The guest language is whatever the snapshot has (the current `clean` is ja-JP, so the app comes up
  in Japanese; checked with 0.3.1). `UI_LANG=en` overrides it for one launch.
- Snapshots: `clean` = untouched install (Setup Assistant done, Remote Login on, controller mapping set
  in Game Controllers settings); `provisioned` = `clean` + provision.sh (auto-login, passwordless sudo,
  no sleep/lock, muted, sound device off). Both ja-JP.
- `reset.sh` = `prlctl snapshot-switch MacOS --id <id of snapshot "$SNAPSHOT">`, stopping the VM first.
  To change what "clean" means, boot the VM, change it, stop it, then
  `prlctl snapshot-delete`/`snapshot` (or add a second snapshot and use `SNAPSHOT=name`).
- Headless: `start.sh` sets `--startup-view headless`; `prlctl start` then shows no VM window
  (Parallels Desktop itself may still appear in the Dock / menu bar).

## Screenshots

`screenshot.sh` uses `prlctl capture MacOS --file out.png`: Parallels grabs the guest framebuffer,
so nothing captures the host screen, no Screen Recording permission is involved on either side, and
it works headless. Output is the guest's full display (about 2040x1344 here), Gatekeeper / TCC
dialogs included.

Input: there is no mouse-click helper. Keys can be sent with `prlctl send-key-event MacOS --key <code>`;
for most test setups use `ssh.sh` (`open`, `defaults`, `osascript` for things that do not need
Accessibility permission). Gatekeeper's "downloaded from the Internet" prompt (deploy with
`QUARANTINE=1`) therefore has to be answered by hand in the Parallels window, or avoided by
deploying without quarantine.

## Game controllers

`controllers.sh` (no arguments) lists host USB devices Parallels can hand to a macOS VM
(`prlsrvctl usb list`, "Compatible-with-macOS-VM"); `connect '<name>'` / `disconnect '<name>'` use
`prlctl set MacOS --device-connect/--device-disconnect`; `guest [seconds]` builds
`tools/gc-list.swift` on the host and runs it in the guest (IOKit HID gamepads +
`GCController.controllers()`, i.e. what ReplayNES sees). Parallels 20.3+ passes USB devices
through to macOS guests on Apple silicon; whether a given controller then appears in
GameController.framework is checked with `guest` (not verified end to end for Pro Controller /
8BitDo receivers: they were not plugged in during the last session).

## Limitations

- **GPU is paravirtualized.** Metal works, but frame pacing, latency and performance numbers are not
  representative; use `scripts/perf-smoke.sh` on the host. No ProMotion / VRR.
- Silent by design: `start.sh` disables the VM's sound device (`prlctl set <vm> --device-set sound0
  --disable`, re-applied on every start because a snapshot switch may restore it) and provisioning mutes
  the guest's output. Audio behaviour therefore can't be checked in the VM.
- No Xcode in the VM: it runs built apps, not `scripts/test-macos.sh`.
- Notification banners cannot be fully disabled from the command line.
- Apple's license allows macOS VMs only on Apple hardware, at most 2 at a time.
