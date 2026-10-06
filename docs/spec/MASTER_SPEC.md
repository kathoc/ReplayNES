# NES No-Miss Recorder MASTER SPEC

## Goal
Play NES/Famicom on macOS with low latency, and record not video but a deterministic emulation history. On a miss, go back to any point in the past and re-record from there. Playing back the final take gives continuous play with no failed sections, and it can be exported offline to MP4 with audio.

## Core principle
Logical time is the integer frameIndex. At each frame boundary, fix the P1/P2 input and system events, then advance the core by exactly 1 frame.
Playback identity = ROM SHA-256 + core ID/build + serialization version + region/timing + machine config + initial state + ordered frame input/event stream.
The PRNG of games such as Tetris is also reproduced from the full state (including CPU/RAM/PPU/APU/mapper) and the same input sequence. Do not let host clock, host RNG, or thread scheduling leak into game results.

## Recording
The source of truth is "the final input bitfield actually passed to the core". Record P1/P2 input, soft reset, power cycle, and similar events every frame or at change points. Record the input after turbo conversion, not the turbo settings themselves.

## Checkpoints
Save full savestates at regular intervals and at bookmark time. On seek, load the nearest state at or before the target frame and quickly re-run the input log to reach the target frame. States are for speed and are not the source of truth for history. A state carries the frame, the core compatibility ID, and a checksum.

## Branching
If you re-record after a rewind, create a new branch. The old take is not deleted immediately but kept for undo/recovery. Internally an immutable segment DAG is allowed, and the active take is a single path from root to leaf. The normal UI does not present the DAG in a complicated way.

## Persistence
`.nesrec` package:
manifest.json
timeline/index.json
timeline/segments/*
states/*
metadata/bookmarks.json
journal/*
The ROM itself is in principle not included; keep a path/reference and its SHA-256. A ROM with the same hash can be re-specified.
The manifest stores formatVersion, core ID/build, and state serialization version. Implement atomic write, journal, checksum, and crash recovery.

## Core compatibility
Fix the core compatibility ID at project creation. If an app update changes the core/state format, do not convert existing projects unconditionally. Consider a design that can bundle/select the old core, or an explicit migration, with reproducibility as the top priority.

## Controls
External controllers: GameController.framework. Keyboard supported. Remappable. Hotkeys are separate from game input. Pause when a controller disconnects.
pause / frame advance / slow 1/2, 1/4 / rewind / scrub / bookmark.
Pause and slow during production are not counted in the final take's time; final playback is at normal speed.

## Turbo
A/B etc. can be auto-fired with a per-frame period/duty. Input Pipeline: physical input -> mapping -> turbo/SOCD policy -> final NES bitfield -> recorder/core.

## Reset
Record Soft Reset and Power Cycle as separate system events, per frame. This makes games that use reset in their strategy reproducible.

## Low latency
Do not encode during play. Separate the responsibilities of emulation, render, audio, and UI. Metal display, a low audio buffer, and a shortened path from input sample to next frame. Measure latency. "Zero latency" is not a goal.

## Audio
Play the core's APU PCM in real time. Pause/rewind/scrub are muted in principle; muting during slow is acceptable in the first version. Export uses normal-speed PCM.

## MP4
Re-run the active take headless/offline from the beginning to generate video frames and PCM. H.264/HEVC + AAC with AVAssetWriter. Timestamps are generated from emulated time and do not depend on the wall clock. UI frame drops must not affect the export. Provide nearest-neighbor integer scaling, overscan, and pixel aspect settings.

## Determinism tests
Play the same ROM/state/input multiple times, and compare the machine-state hash, framebuffer hash, and audio hash at regular intervals. Playback from the start and playback from a mid-point savestate must also match. Do not put commercial ROMs in the repo; use legally usable test ROMs/fixtures.

## UI
Game viewport, record/play state, frame/time, pause/play, frame-step, slow, rewind, timeline scrubber, bookmark, reset, controller status, take/branch, export. Advanced branch management goes in a separate panel.

## Non-goals v1
ROM distribution, ROM modification, netplay, cheats, Lua automation, an advanced TAS input editor, live video cut-and-paste approaches.
