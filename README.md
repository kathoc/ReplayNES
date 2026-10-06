English | [日本語](README.ja.md)

# ReplayNES

**Made a mistake? Go back and record it again. What you end up with is a flawless, no-miss run.**

ReplayNES is an NES / Famicom emulator for macOS. It records your play not as video but as
a history of inputs, so you can rewind to any moment you like and re-record from there.
When you play the finished take from the beginning, the failed parts are gone and it plays as one continuous run.
You can export it as-is to an MP4 with audio.

![ReplayNES (running a homemade test ROM)](docs/images/screenshot.png)

## Features

- **ROM Library**: ROMs placed in `~/Documents/ReplayNES/ROM` are listed, and you can start playing just by picking one. Projects are saved automatically to `~/Documents/ReplayNES/Projects`, and you can resume each ROM with "Continue".
- **Flash Reduction (care for flashing lights)**: Detects scenes where the whole screen flashes intensely and tones down only what is displayed (on by default). It does not affect game progress or recording.
- **Rewind and re-record**: You can go back to any frame. If you play from there, it becomes a new take, and the previous take is kept rather than erased.
- **Practice Mode (A/B Repeat)**: Mark a tricky section with A and B and practice it over and over without recording. Up to 8 sections are saved per project.
- **Controller-only operation**: R2 rewinds, L2 fast-forwards, R pauses, L slows down, and while paused the D-pad steps frame by frame. The controller works even when ReplayNES is not in front (for example while you are operating OBS).
- **Even Tetris-style randomness is reproduced exactly**: Playback is recomputed from "the console state + the input for every frame", so you get exactly the same result as when you recorded.
- **Production aids**: Pause, frame advance, advance by N frames, 1/2 slow motion, timeline seeking, bookmarks, soft reset, and power cycling. None of these count toward the final take's duration, and playback is always at normal speed.
- **Turbo**: Set the cycle and press duration of A/B turbo in frames. What is recorded is the input after turbo has been applied.
- **Pick up the next day**: If you save the project (`.nesrec`), you can open it later and still rewind even further into the past. Autosave and crash recovery are supported, and even if you quit the app (including a force quit), it automatically picks up where you left off at the next launch.
- **MP4 export**: The video is redrawn offline based on in-game time, so dropped frames during play do not affect it. Pixels are scaled up with nearest-neighbor, and you can specify overscan cropping and the pixel aspect ratio (1:1 / 8:7).
- **Low latency**: Rendering uses Metal, and the audio buffer is kept small. There is also a latency measurement display (⌘L).
- **Automatic updates**: New versions are checked from GitHub Releases, and the signature (EdDSA) is verified before updating (using [Sparkle](https://sparkle-project.org)). You can also check from the menu via "ReplayNES → Check for Updates…".

## System Requirements

- macOS 14 or later, Apple Silicon (arm64)
- A keyboard, or a game controller that macOS recognizes (GameController.framework supported)
- Game ROM files (`.nes`) are not included. Please use ones you have legally prepared from games you own.

## Installation

1. Download `ReplayNES-<version>-macOS-arm64.zip` from [Releases](https://github.com/kathoc/ReplayNES/releases) and unzip it.
2. Move `ReplayNES.app` to the "Applications" folder.
3. Because it has not been notarized, launch it the first time with **right-click → "Open"**. If it still will not open, choose "Open Anyway" in "System Settings → Privacy & Security".
4. On the second launch you will be asked "Automatically check for updates?". After that, updates can be done inside the app (0.1.2 and later; from 0.1.1 or earlier, replace the app manually). In the "Updates" tab of Settings, you can turn automatic checking and automatic installation on or off at any time.

## Usage

### Getting Started

- **Library (start screen / ⇧⌘L)**: The easiest way to start (see "Library" below).
- **New Project (⌘N)**: Choose a ROM and decide where to save the `.nesrec` project. Recording starts right away.
- **Try a ROM (No Project) (⇧⌘N)**: Use this to play without creating a project (your work is stored temporarily and resumes where you left off at the next launch).
- **Open Project (⌘O) / Save (⌘S)**: An opened project is shown paused. Resume with Space / R on the controller (or the ▶︎ button).

### The Screen

- The bottom bar is a simple layout of just the "Record" button, go to start, rewind (while held), pause / resume, fast-forward (while held), slow, the timeline, "Practice", and the display size. While paused, step back / frame advance buttons appear. Bookmarks, the takes list, advance by N frames, reset, and so on are in the "…" menu and the menu bar.
- **Display Size**: "Integer" is the largest integer scale that fits on the screen, and the pixels line up crisply. "FILL" expands to fill the window while keeping the aspect ratio (4:3, or pixel aspect 8:7). You can choose it at the right end of the bottom bar or from the "View" menu (⌘F toggles), and the setting is saved.
- **Timeline (filmstrip)**: Like video editing software, screenshots of the whole take are lined up. Dragging moves to that position (silent, paused). The playhead (red while recording, white while playing, orange while practicing), bookmarks (yellow), and A/B sections (numbered colored bands) are overlaid. Thumbnails are generated on a separate core from the game processing, so operation stays light even with a one-hour take (for long takes, thumbnails are replaced with the correct pictures from left to right, and until then a nearby thumbnail is shown). Each thumbnail is pinned to its time within the take, so as the take grows while recording, the whole strip shrinks smoothly, and the rightmost thumbnail gradually appears from partway into view.
- The sidebar (takes, bookmarks, controllers) is hidden by default. You can show it with the button at the right end of the toolbar or with ⌥⌘S.
- "Help → Controls Guide" (⌘?) has a list of controller and keyboard controls.

### Record Button (Record / Playback)

- A glowing red "Record" means record mode. Click it and it turns gray, switching to **playback mode**, which plays the recorded take. If you were at the end of the take, playback starts automatically from the beginning; otherwise it starts from the current position (the last place you sought to).
- In playback mode, click the gray "Record" to return to record mode paused at that position. The next input you make continues the recording (if you are partway through, it branches into a new take, and the original continuation is kept too).
- In playback mode, it pauses when it reaches the end. Press ▶︎ again to play from the beginning.

### Library

ReplayNES creates the following folders at launch (it shows an error if it cannot create them). The first time, macOS asks for permission to access the Documents folder, so choose "Allow".

| Folder | Contents |
|---|---|
| `~/Documents/ReplayNES/ROM` | Where you put your own ROMs (`.nes`). Subfolders one level down are also loaded |
| `~/Documents/ReplayNES/Projects` | Where projects started from the Library (`<ROM name> <yyyy-MM-dd HHmm>.nesrec`) are saved |

1. Put `.nes` files in the ROM folder (you can open the folder with the "Open in Finder" button). Additions and deletions are picked up automatically (you can also refresh with "Reload").
2. On the start screen (or the menu "File → Library…" / ⇧⌘L), choose a ROM and **double-click / press Return / click "Play"**. A new project is created without asking where to save it, and the game starts right away. Your work is autosaved.
3. On the right, "Projects for This ROM" lists the projects made with that ROM. **"Continue"** opens where you left off. Projects are matched to ROMs by the ROM's contents (SHA-256), so the match is kept even if you rename the ROM file.
4. If you choose a different ROM while playing, you are asked whether to save in the usual way if there are unsaved changes (for a temporarily stored session, it asks "Do you want to save?". See "Resume" below). If a ROM could not be loaded, the error is shown.

You can narrow down ROM names with the search field. As before, ⌘N (choose where to save and create a new project) and ⌘O (open a project from anywhere) are also available.

### Flash Reduction (care for flashing lights)

It detects scenes such as explosions and lightning where **a wide area of the screen flashes intensely** and tones down only the displayed picture (it reduces the number of flashes and keeps the darker side). It uses the WCAG 2.x general flash and red flash thresholds (no more than 3 flashes per second over about 25% or more of the screen) as a guide.

- In Settings → "Display & Audio" → "Flash Reduction" you can choose **Off / Low / Standard / High**. The default is **Standard (on)**.
  - Low: exactly the WCAG threshold (up to 3 times per second)
  - Standard: detects early, up to 2 times per second (recommended)
  - High: up to 1 time per second. It also weakens small remaining flicker
- Small flashes (such as a character blinking) and normal scrolling are shown as they are. While it is reducing, "Flash Reduction Active" is shown at the top left of the screen (you can hide it in Settings).
- It has no effect at all on recorded input, game progress, or reproducibility (hashes). It is display-only processing.
- When exporting to MP4, choosing "Apply Flash Reduction" applies it to the exported video as well (the initial value follows the current setting).

> **Note**: This feature does not reliably prevent photosensitive seizures or the like. If you are sensitive to flashing, please take sufficient care, and stop playing immediately if you feel anything unusual. See [docs/FLASH_REDUCTION.md](docs/FLASH_REDUCTION.md) for how it works and its limits.

### Controller and Key Assignments (changeable in Settings)

Controller buttons are assigned by **position** (right button = Famicom A, bottom button = B). On Nintendo controllers, A = A and B = B as printed.

| Action | Controller (position) | Pro Controller / Joy-Con | Xbox | PlayStation | Keyboard |
|---|---|---|---|---|---|
| D-pad | D-pad / Left Stick | | | | ← ↑ → ↓ |
| A / B | Right / bottom button | A / B | B / A | ○ / ✕ | X / Z |
| Turbo A / Turbo B | Top / left button | X / Y | Y / X | △ / □ | S / A |
| START / SELECT | Menu / Options | + / − | ≡ / View | OPTIONS / CREATE | Return / Right Shift (or `\`) |
| Rewind (while held) | **Right Trigger** | ZR | RT | R2 | Delete |
| Fast-forward (while held) | **Left Trigger** | ZL | LT | L2 | Tab |
| Pause / Resume | **Right Shoulder** | R | RB | R1 | Space |
| Slow 1/2 ⇔ normal speed | **Left Shoulder** | L | LB | L1 | L |
| Step back / frame advance while paused | **D-pad ← / →** (hold for continuous) | `,` / `.` |
| Bookmark | — | B |

- Rewind, fast-forward, pause, slow, and frame advance are "hotkeys" and are never recorded as game input. The D-pad ← / → while paused is not sent to the game either (you can turn this off in Settings → Hotkeys).
- **Fast-forward** just plays the recorded take at high speed and records nothing. When it reaches the end of what is recorded (the paused position), it stops there and pauses.
- If you keep playing in record mode after rewinding, it automatically branches into a new take from that position and recording continues.
- **Background input**: While a controller is connected, input is accepted even when ReplayNES is not in front (for example while operating OBS), and it does not pause automatically. Sound also keeps playing. The keyboard works only while ReplayNES is in front.
- If the controller is disconnected, it pauses automatically.
- If you update from 0.1.x and the controller hotkeys are still the old defaults (L1 rewind / R1 fast-forward / L2 and R2 frame advance), they are automatically switched to the new assignments above. If you had changed them yourself, they are left as they are.
- **Button layout diagram**: In Settings → "Controllers" (or "Button Layout…" in the sidebar), a diagram of the connected controller (Nintendo / Xbox / PlayStation / other shapes) and the assignment of each button are shown. Pressing a button lights it up on the diagram, so you can check which button is which. Clicking a button in the diagram lets you choose the action to assign (Famicom button, turbo, hotkeys such as rewind, or none). "Reset Pad N to Defaults" resets only that controller to its defaults. The traditional method of assigning by pressing a key or button is in the "Game Input" and "Hotkeys" tabs.
- Up to 0.2.0, A / B and X / Y were swapped on Nintendo controllers (Pro Controller, Joy-Con). If you were using the face button assignments at their defaults, they automatically become the correct layout after the update. If you had changed them yourself, they are carried over when you connect that controller, so the same buttons perform the same actions as before.

### Rewind and Re-record

1. If you make a mistake, **hold R2 (or Delete) to rewind**, or drag the timeline bar to go back.
2. If you resume play from the position you went back to, recording of a new take starts from that position. The original play that came after it is kept rather than erased.
3. If the previous take was better, you can go back with **"Back to Previous Take" (⌥⌘Z)**. All takes can be checked in "Takes" (⇧⌘T).
4. In playback mode (the gray "Record" button), the recorded take is played. If you want to make changes, press the "Record" button to return to record mode.

If you add a bookmark (B / ⌘D), you can jump straight to that position.

### Production Aids

- Pause (R / Space / ⌘P), frame advance (D-pad → while paused / `.` / ⌘→), advance a specified number of frames ("…" menu → "Advance by Frames")
- Slow: 1/2 ⇔ normal speed (L / ⌘2). Slow motion and pausing are not counted in the final take's duration.
- Soft reset (⌘R) and power cycle (⇧⌘R) record which frame they were done on, and are reproduced on the same frame during playback.
- No sound is produced while paused, rewinding, seeking, in slow motion, or fast-forwarding.

### Practice Mode (A/B Repeat)

Practice only the tricky section over and over without recording.

1. Press "Practice" (⇧⌘P) in the bottom bar and 8 section panels appear over the game screen.
2. At the start of a section press "**A**", then keep playing from there and press "**B**" at the end position (you can also set them while paused). Each section shows its length (minutes:seconds.frames), and from "…" you can rename or clear it.
3. Press ▶︎ (Practice This Section) and play starts automatically from A. **When you reach B, the screen holds still for 0.5 seconds (the sound fades out naturally), then goes back to A as if rewinding and starts again automatically**.
4. While practicing you can also use R2 rewind (up to A), R pause, L slow, and D-pad frame advance / step back while paused.
5. "**Stop Practicing**" (in the panel / in the bottom bar / ⇧⌘P) returns you exactly to the position in the take from before you started practicing.

- Nothing is recorded during practice. The length and contents of the take do not change.
- Set B at a position you reached by "playing on from A". If you rewound, moved on the timeline, or switched takes after A, a message to that effect is shown (start over from A, or set A again).
- Sections are saved in the project (they are included in autosave too). If a section's data is damaged, you can choose "Discard Damaged Sections and Open" when opening (the discarded sections are shown. Takes are not affected).
- During practice, the player controls for play video (timeline seeking, bookmarks, take switching) are unavailable.

#### Specifying A/B on the Timeline

For a recorded take, you can define sections on the timeline without playing again.

- Use "**A/B 1**" at the right of the timeline to choose which section (1 to 8, color-coded) to edit.
- **Dragging** on the band on the timeline (the thin row above the thumbnails) makes that range the A→B of the selected section (on the thumbnails, **Shift+drag**).
- **Dragging either end** of a section adjusts A / B. **Clicking a section** starts practicing that section.
- "**Set A Here (Playhead)**" (⌥⌘I) / "**Set B Here (Playhead)**" (⌥⌘O) at the playhead. They are also in the "Playback" menu and the "A/B" menu. Set B Here can be used even if you have not played on from A, as long as A is on this take (the "B" in the Practice panel works the same way).
- Only sections on the current take are shown (sections made on a different take, and an A set during practice, do not appear on the band). Sections cannot be set while practicing.

### Pick Up the Next Day

Save with `⌘S` and open it later with `⌘O` to resume from where the last-used take left off. From there you can also rewind even further into the past.
Your work is autosaved while you work (every 2 seconds by default; it also saves immediately when you pause and when the app goes to the background). If a previous abnormal exit is found when you open a project, it is recovered up to the last autosave and a message says so.

### Resume (Pick Up Where You Left Off, Even After Quitting)

- **Automatic continuation**: When you quit with ⌘Q or by closing the window, you are not asked "Do you want to save?". Your work in progress is saved as it is, and at the next launch the previous project (or the session you were playing without a project) opens **paused at the previous position and mode**, and "Resumed where you left off" is shown. If you were practicing, the Practice panel opens too. After a force quit or power outage, it resumes from the position of the last autosave (within a few seconds).
- **For projects**: On quit, it only writes to the project's autosave (journal), and what you saved with ⌘S does not change. When switching to another project or ROM, you are asked whether to save, as before (choosing "Don't Save" returns to the last saved state).
- **Where temporary storage lives**: Work done while playing without a project ("Try a ROM (No Project)", or when you open a ROM file directly) is stored temporarily at `~/Library/Application Support/ReplayNES/Session/current.nesrec` (the resume information is `resume.json` in the same folder). Temporary storage does not appear under "Continue" in the Library. If you save it anywhere you like with ⌘S (Save), it becomes a normal project.
- **When it is discarded**: If you try to open another ROM / project while the temporary storage holds recorded content (Library, ⌘N, ⌘O, Try a ROM, opening a file), you are asked "Do you want to save?". Choosing a destination with "Save…" keeps it as a project, and choosing "Don't Save" discards the temporary storage (the same applies to "Close Project"). Temporary storage with nothing recorded is discarded without asking.
- **When resuming fails**: If it cannot resume because the ROM was moved or deleted, it was recorded with a different core, or the file is damaged, it shows the reason and returns to the start screen (Library). If the ROM can be specified again, you can specify it right there. The temporary storage is **kept rather than deleted**, and resuming is tried again at the next launch. When you start a different ROM, you can choose to save or discard the leftover previous temporary storage.
- If you launch two copies of ReplayNES at the same time, the one launched later does not resume and does not touch the temporary storage.

If you moved the location of the ROM file, specify the ROM again when opening. It is confirmed to be the same ROM by SHA-256.
**The ROM itself is not stored in the project.**

### Export to MP4 (⌘E)

- Format: H.264 or HEVC with AAC (48 kHz)
- Size: integer multiples of 256×240, 1280×960, 1920×1440, and so on
- You can choose overscan cropping and the pixel aspect ratio 1:1 / 8:7.
- With "Apply Flash Reduction", you can get a video with intense flashing toned down (see "Flash Reduction" above).
- Export recomputes everything from the beginning in a separate emulator. Exporting never changes the project, and you can keep working during export.

### Streaming (OBS)

The game screen is output via [Syphon](https://syphon.github.io) and can be brought directly into streaming software such as OBS (this is not a virtual camera).

1. In ReplayNES, turn on "Settings → Display & Audio → Stream Output (Syphon)" (you can also toggle it from the menu "View → Stream Output (Syphon)"). While it is on, "Syphon Live" is shown at the top left of the game screen.
2. In OBS (macOS version), add "Source → ＋ → Syphon Client" and choose "[ReplayNES] ReplayNES" as the server.
3. For audio, add OBS's "macOS Audio Capture" source (captures an application's audio) and choose ReplayNES. No setting is needed on the ReplayNES side.

- Only the game screen is output (the UI and badges are not included). Flash reduction is already applied just as in the display, and the entire 256×240 including the overscan area is enlarged with nearest-neighbor interpolation.
- Output size: native 256×240, 2x to 4x (default is 4x, 1024×960), 1280×960 / 1920×1440 (4:3, black bars on the left and right). The 8:7 pixel aspect ratio can also be chosen (separate from the display setting).
- Output runs on a separate thread from emulation, and nothing is drawn when there is no receiver. A delay of a few frames may appear on the OBS side.
- While paused, the last picture keeps being shown.
- The controller works even when ReplayNES is not in front, so you can keep playing while operating OBS.

## About Core Compatibility (Important)

Recorded data depends strongly on "which emulator core plays it back". If the core version differs even slightly, the same inputs can give shifted results.
For that reason, each project records the core compatibility ID from when it was created (example: `nestopia-ue@7b5c87d8dc3c+p2+adapter1+ntsc`).

- A project with a different compatibility ID **will not open without a warning**. It will not be silently converted either.
- If the compatibility ID changes in a future version, we will either bundle the old core or provide an explicit migration procedure ([docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)).
- For important projects, we recommend keeping them together with the version of ReplayNES used to create them.

## Privacy

ReplayNES connects to the network only when it accesses GitHub (github.com and its download servers) to check for and download updates.
All it sends are ordinary HTTPS requests to fetch the update information (`appcast.xml`) and the update file; it sends no system information, usage data, or the contents of ROMs or projects. There is no telemetry either.

- Automatic checking is enabled only if you allowed it in the prompt shown at the second launch.
- If you turn off "Automatically check for updates" in the "Updates" tab of Settings, it does not connect except when you check manually from the menu.

What it reads and writes is limited to the ROMs and projects you specify, the Library folder (`~/Documents/ReplayNES`), settings files, and the update cache.

## Known Limitations

- Only NTSC (Japan / North America) timing is supported. PAL and the Famicom Disk System (FDS) have not been verified.
- Determinism tests (whether the same input gives the same result) are done with the bundled homemade test ROMs. Many of the mappers used by commercial games have not been verified individually. If you notice a problem, please let us know.
- Autosave runs between emulation frames. On a slow disk it may rarely be delayed by one frame.
- Audio absorbs the drift between the timer and the audio clock by dropping samples. A faint pop of noise may rarely occur. No sound is produced during slow motion and fast-forward. The exported audio is mono.
- Pixel-perfect integer scaling happens only at a 1:1 pixel aspect ratio. At 8:7, the horizontal scaling width is not uniform.
- The UI is available in English and Japanese. It has been tested only on Apple Silicon. Testing with physical game controllers has been limited.
- Because it has not been notarized, the first launch requires some steps (see [Installation](#installation)).
- There is no Windows / Linux version yet (planned).

## Build from Source

```bash
git clone --recursive https://github.com/kathoc/ReplayNES.git
```

```bash
cd ReplayNES && scripts/build-macos.sh
```

Requirements: Xcode 16 or later, CMake, Ninja, XcodeGen (`brew install cmake ninja xcodegen`). On the first build, Swift Package Manager fetches Sparkle. Syphon is built from source from the submodule `third_party/syphon` (no extra download of the Metal Toolchain is needed). You can check stream output without OBS using `scripts/check-syphon-macos.sh`.

The engine and tests (common to macOS / Linux / Windows) are as follows.

```bash
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release && ninja -C build && ctest --test-dir build
```

With the command-line tool `replaynes-cli`, you can verify determinism (`determinism`), verify projects (`verify`), generate test ROMs (`make-test-rom`), and more.

For the design, see [docs/ARCHITECTURE_DECISION.md](docs/ARCHITECTURE_DECISION.md); for the file format, [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md); and for porting to other OSes, [docs/PORTING.md](docs/PORTING.md).

## License

GPL-2.0-or-later. The NES core uses [Nestopia UE](https://github.com/0ldsk00l/nestopia) (GPL-2.0-or-later). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
ReplayNES does not include game software, and does not distribute any.
