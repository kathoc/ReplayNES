# Testing the macOS app in a clean macOS VM

A disposable macOS 27 virtual machine (tart, Apple's Virtualization.framework) for checking the
built ReplayNES.app on a pristine system: first launch / Gatekeeper, empty preferences and library,
English vs Japanese UI, no Xcode or developer tools installed. It runs headless, so it never takes
over the desktop, and screenshots of the VM's screen are taken on the host without any privacy prompt.

Scripts: `scripts/macos-vm/`. VM state outside the repo: `~/.tart/` (images, VMs) and
`~/.local/state/replaynes-vm/` (VNC URL, the VMs' SSH host keys). ROMs and VM images never go into
the repo.

## Why tart

- Free to use here: Tart 2.40 ships under the Functional Source License (FSL-1.1-ALv2, see
  `LICENSE` in the release tarball; each version becomes Apache 2.0 two years after release).
  Any use is a Permitted Purpose except a Competing Use (offering Tart, or a product substituting
  for it, commercially to others); testing our own app is fine. (Older Tart versions used the
  Fair Source License, free up to a CPU-core limit per organization.)
- One CLI for everything: pulling ready-made macOS images from ghcr.io (no IPSW install, no Setup
  Assistant clicks), `clone` (APFS copy-on-write: a fresh VM in seconds, ~no disk), headless runs
  with a built-in VNC server.
- UTM is free too, but has no ready-made images and its `utmctl` cannot create or snapshot macOS
  VMs headlessly. Apple's Virtualization sample would need building and has no image registry.
  Parallels Desktop (27, licensed) can also run macOS guests (it uses the same
  Virtualization.framework underneath) and lists USB game controllers as "Compatible-with-macOS-VM",
  but it needs an IPSW install plus a manual Setup Assistant run per VM and has no image registry;
  see "Game controllers" below for where it may still be worth it.

## Setup (once)

```sh
# 1. tart: the Homebrew tap (cirruslabs/cli/tart) is currently broken with recent Homebrew
#    ("Calling depends_on :macos with depends_on macos: is disabled"), so use the signed,
#    notarized release tarball (Developer ID: Cirrus Labs, Inc.):
gh release download -R cirruslabs/tart -p tart.tar.gz      # latest release
mkdir -p ~/.local/opt/tart && tar -xzf tart.tar.gz -C ~/.local/opt/tart
ln -sf ~/.local/opt/tart/tart.app/Contents/MacOS/tart ~/.local/bin/tart
#    (once the tap is fixed: brew install cirruslabs/cli/tart)

# 2. vncdotool, for screenshots (and mouse/keyboard input) over the VM's VNC server
uv tool install vncdotool            # or: pipx install vncdotool

# 3. the VMs (pulls ~25 GB the first time, then ~5 minutes)
scripts/macos-vm/create.sh
```

`create.sh` pulls `ghcr.io/cirruslabs/macos-golden-gate-vanilla:27.0` (macOS 27 "Golden Gate",
the vanilla image: a fresh install with auto-login and Remote Login on, nothing else installed - no
Homebrew, no Xcode / Command Line Tools), and makes:

| VM | role |
| --- | --- |
| `replaynes-test-clean` | the clean snapshot: provisioned once, never used for tests |
| `replaynes-test` | the working VM, cloned from the snapshot; trash it at will |

Both: 4 CPUs, 8 GB RAM, 1920x1200 display, 80 GB virtual disk (~69 GiB APFS in the guest).
On the host: ~35 GB image cache in `~/.tart/cache` plus the two VMs, which share blocks with it
(APFS clones); the working VM only grows by what a test writes.
`tart delete ghcr.io/cirruslabs/macos-golden-gate-vanilla:27.0` drops the cache (a `FORCE=1`
rebuild then pulls it again).
Provisioning (inside the guest only): your `~/.ssh/id_*.pub` in `authorized_keys`, passwordless
sudo, no display/system sleep, no screen saver, no lock screen, no automatic update checks, no
Spotlight indexing, crash dialogs off, Dock auto-hidden, APFS container grown to the full disk.

Settings: `VM`, `CLEAN_VM`, `IMAGE`, `VM_CPU`, `VM_MEM_MB`, `VM_DISPLAY`, `VM_DISK_GB` (see
`scripts/macos-vm/common.sh`). `FORCE=1 scripts/macos-vm/create.sh` rebuilds both VMs.

### Credentials

User `admin`, password `admin` (the cirruslabs image default, kept). The VM is on tart's NAT network
(192.168.64.x, reachable from this Mac only); scripts log in with your SSH key. The VNC server
listens on a random port with a random one-time password printed to
`~/.local/state/replaynes-vm/<vm>.run.log`.

## Daily use

```sh
scripts/macos-vm/reset.sh                         # fresh replaynes-test from the clean snapshot
scripts/macos-vm/start.sh                         # boot headless (+ VNC); waits for SSH
scripts/macos-vm/deploy.sh                        # build/ReplayNES.app -> guest /Applications
scripts/macos-vm/deploy.sh dist/ReplayNES-0.3.1-macOS-arm64.zip    # or a release zip
build/tools/replaynes-cli/replaynes-cli make-test-rom /tmp/test.nes
ROM=/tmp/test.nes scripts/macos-vm/run-app.sh     # copy the ROM in, launch --rom ... --autoplay
UI_LANG=ja ROM=/tmp/test.nes scripts/macos-vm/run-app.sh            # Japanese UI, this run only
scripts/macos-vm/screenshot.sh /tmp/shot.png      # PNG of the VM's screen
scripts/macos-vm/ssh.sh                           # shell in the guest (or: ssh.sh 'command')
scripts/macos-vm/stop.sh
```

- `run-app.sh` passes its arguments to the app (`--rom`, `--project`, `--library-root`,
  `--test-actions`, `--snapshot-at`, ... see `apps/macos/Sources/App/ReplayNESApp.swift`); paths are
  guest paths. It launches through `open`, i.e. LaunchServices + Gatekeeper, like Finder.
  `FRESH=1` deletes the app's preferences, Application Support and `~/Documents/ReplayNES` first.
- `UI_LANG=ja` sets `-AppleLanguages (ja)` for that launch only. To switch the whole guest:
  `scripts/macos-vm/ssh.sh 'defaults write -g AppleLanguages -array ja en'` (and
  `defaults delete -g AppleLanguages` to go back); the next app launch should pick it up. As of
  0.3.0 it does not: with the guest set to `(ja, en)` the app still comes up in English, because
  `UILanguage.apply()` reads the global list through `UserDefaults(suiteName: UserDefaults.globalDomain)`,
  which is nil ("Using NSGlobalDomain as an NSUserDefaults suite name does not make sense and will
  not work"), so it always pins English. `-AppleLanguages (ja)` (`UI_LANG=ja`) works.
- `screenshot.sh --do '<vncdo commands>'` drives the mouse/keyboard, e.g.
  `--do 'move 960 600 click 1'` or `--do 'key cmd-q'` (coordinates in VM pixels, 1920x1200).
- Several VMs: `VM=other scripts/macos-vm/...` (tart can run at most 2 macOS VMs at a time).

### Snapshot / reset

`replaynes-test-clean` is the snapshot. `reset.sh` deletes `replaynes-test` and clones it again
(copy-on-write, seconds), so every test can start from the freshly provisioned system: no
preferences, empty library, no quarantine decisions remembered. To change what "clean" means
(e.g. a different guest language), boot the clean VM itself, change it, stop it:
`VM=replaynes-test-clean scripts/macos-vm/start.sh`, `VM=replaynes-test-clean scripts/macos-vm/ssh.sh ...`,
`VM=replaynes-test-clean scripts/macos-vm/stop.sh`.

## First launch and Gatekeeper

The app is ad-hoc signed and not notarized. Copied over SSH it carries no quarantine flag and
launches directly. To test what a user who downloaded the zip sees, deploy with
`QUARANTINE=1` (adds the `com.apple.quarantine` attribute Safari would):

- On macOS 27 (26A428) the quarantined, ad-hoc signed 0.3.0 app shows the dialog
  "“ReplayNES” is an app downloaded from the Internet. Are you sure you want to open it?" with
  **Cancel / Open** (no "cannot be verified" block, no trip to System Settings). Click Open
  (in the VM: `scripts/macos-vm/screenshot.sh --do 'move 1018 484 click 1'` at 1920x1200), and the
  app starts; the decision is remembered for that copy.
- `run-app.sh` reports "running" while the dialog is up (the process exists, waiting on Gatekeeper),
  so always look at a screenshot after a first launch.
- If a future macOS blocks it outright: right-click → Open in Finder, or System Settings → Privacy &
  Security → "Open Anyway" (what README.md tells users).

For testing, `scripts/macos-vm/ssh.sh 'xattr -dr com.apple.quarantine /Applications/ReplayNES.app'`
clears it (deploying without `QUARANTINE=1` does the same).

## Screenshots: how and why

`start.sh` runs `tart run --no-graphics --vnc-experimental`: Virtualization.framework's own VNC
server serves the VM's framebuffer, independent of the guest OS. `screenshot.sh` reads it with
`vncdo capture`. So:

- nothing captures the host screen (no `screencapture` on the host, no host privacy prompt);
- nothing in the guest needs Screen Recording permission (`screencapture` over SSH in the guest is
  denied by TCC, and granting it needs a click in the guest's System Settings);
- it shows exactly what is on the VM's display, Gatekeeper / TCC dialogs included.

The app's own scripted checks (`--snapshot-at`, `--snapshot-windows`) work in the VM too; fetch
their output with `scp` via `scripts/macos-vm/ssh.sh`.

## Game controllers

`scripts/macos-vm/controllers.sh` builds `scripts/macos-vm/tools/gc-list.swift` on the host and runs
it in the guest: HID gamepads/joysticks (IOKit) and `GCController.controllers()`, i.e. what
ReplayNES sees. Under tart it always finds none.

Parallels Desktop 20.3+ added USB passthrough for macOS guests on Apple silicon (macOS 15+ hosts;
not for audio devices or iPhones), and `prlsrvctl usb list` marks the Switch Pro Controller and
the 8BitDo Pro 3 receiver "Compatible-with-macOS-VM: YES". Whether they then show up in the guest's
GameController.framework is not verified yet (a Parallels macOS 27 VM, `replaynes-pd`, was
installed but stops at Setup Assistant, which has to be clicked through once by hand). To check:
finish Setup Assistant in the Parallels window, enable Remote Login, connect the controller to
the VM (Devices → USB & Bluetooth, or `prlctl set replaynes-pd --device-connect 'Pro Controller #4'`),
copy `~/.local/state/replaynes-vm/gc-list` in and run it.

## Limitations

- **GPU is paravirtualized.** Metal works (the app renders), but frame pacing, latency and
  performance numbers from the VM are not representative of real hardware; use
  `scripts/perf-smoke.sh` on the host for those. ProMotion / VRR are not available (the virtual
  display is 60 Hz).
- **No game controllers.** tart (Virtualization.framework) passes no USB / Bluetooth HID devices:
  `scripts/macos-vm/controllers.sh` reports 0 HID gamepads and 0 GameController.framework
  controllers in the guest while the host sees a wired Switch Pro Controller. Test controllers on the
  host (or see "Game controllers"). Keyboard/mouse input arrives through VNC (`screenshot.sh --do ...`).
- **No audio by default.** `start.sh` passes `--no-audio` so a test never plays on the host's
  speakers; the app then runs with no output device (worth testing as such). `AUDIO=1` enables the
  virtual sound device (output goes to the host's default output).
- No Xcode in the VM (the vanilla image has no developer tools, deliberately): it runs built apps,
  not `scripts/test-macos.sh`. Building/testing stays on the host.
- Notification banners cannot be fully switched off from the command line (Focus is not
  scriptable); provisioning shortens them and turns off the update checks that would cause most.
- Apple's license allows macOS VMs only on Apple hardware and at most 2 at a time per Mac.
