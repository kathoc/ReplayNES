# Steam Deck (Linux) port plan (2026-10-07)

Goal: a native Linux frontend, first-class on Steam Deck (SteamOS 3, Gaming Mode / gamescope and Desktop Mode), with feature parity to the macOS app: library, record/replay, rewind/FF/slow/step, takes/branches, A/B practice, autosave + resume, reset, flash reduction, CRT (nesterm physical model), filmstrip timeline, MP4 export, controller hotkeys + diagram remap, en/ja UI, low-latency display-locked pacing.

## Fixed decisions

- **Shared frontend core in C++** (`frontend/`, static lib + C API `frontend/include/replaynes/frontend.h`), used by macOS (Swift via C API), Linux, and future Windows/iOS. It takes over the platform-independent logic now in `apps/macos/Sources/Core`: display pacing math (DisplayPacing), input catalog/default bindings/layout migration/face-position mapping, resume record + session lock logic, timeline/filmstrip layout math + thumbnail grid/cache policy, library scan + project matching, flash-reduction UI levels, export settings/geometry, practice loop state machine, playback/record-toggle logic. Platform APIs (Metal, GameController, AppKit, AVFoundation, SDL) stay in the frontends. Swift tests are ported to C++ tests (ctest) where the logic moves; the macOS app must keep all its tests green.
- **Linux frontend** `apps/linux/`: SDL3 (window, gamepad incl. Steam Deck controls via Steam Input/hidapi, audio), **Vulkan** for presentation (FIFO with display-locked pacing; present-timing / `VK_KHR_present_wait`/`present_id` where available; mailbox avoided for determinism of latency measurement), **Dear ImGui** (MIT) for UI with full gamepad navigation (Deck has no keyboard in Gaming Mode), 1280×800 default layout + scalable.
- CRT: port the Metal kernels (`apps/macos/Sources/Core/CRT/CRTShaders.swift`) to GLSL compute → SPIR-V (compiled at build time with glslc from the SDK), same math, conformance tests against the same `tests/fixtures/crt/reference.json`.
- MP4 export: FFmpeg libav* (H.264/HEVC + AAC) — via the Flatpak `org.freedesktop.Platform.ffmpeg-full` extension or bundled build; timestamps from frame/sample counts, same as macOS.
- Localization: single source of strings for both frontends (generate from `apps/macos/Resources/Localizable.xcstrings` into a runtime table for Linux, or a shared JSON). Language rule: Japanese if the first system locale is ja, else English.
- Paths (XDG): ROM library `~/Documents/ReplayNES/ROM` (fallback `$XDG_DOCUMENTS_DIR`), projects `…/Projects`, session/resume `$XDG_DATA_HOME/ReplayNES/Session`, settings `$XDG_CONFIG_HOME/ReplayNES`.
- **Packaging: Flatpak** (`io.github.replaynes.ReplayNES`, runtime org.freedesktop.Platform 25.08), installable on SteamOS without unlocking the read-only rootfs; plus instructions to add it to Steam as a non-Steam game for Gaming Mode. Build either on the Deck (flatpak-builder via `org.flatpak.Builder` flatpak) or in a podman/distrobox container; never unlock/modify the SteamOS root filesystem.
- Deck access: `ssh deck@steamdeck.local` (key auth works). Work dir on Deck: `~/ReplayNES-dev`. Never copy ROMs into the repo; ROMs for local testing on the Deck can be copied from the Mac's `roms/` to the Deck's library folder only (never committed).

## Steps & owners

1. heavy-implementer (worktree A): shared C++ frontend core + C API + C++ tests; switch the macOS app to it (delete the duplicated Swift logic); macOS tests green.
2. heavy-implementer (worktree B, in parallel): Linux frontend skeleton on the engine C API: SDL3 + Vulkan display-locked pacing + audio + gamepad, ImGui shell, Flatpak build + run on the Deck, latency measurement on the Deck (Gaming Mode and Desktop).
3. heavy-implementer: Linux feature parity on top of the shared core (library, timeline/filmstrip, practice, takes, reset, resume, settings + controller remap diagram, flash reduction, CRT Vulkan port, MP4 export, en/ja).
4. main: integration review, README/README.ja (Steam Deck install section), release artifacts (Flatpak bundle `.flatpak` attached to the GitHub release).
