# Windows

Status 2026-10-07: **Windows frontend preview** (`ReplayNES.exe`, CMake target `replaynes-win`):
the shared desktop frontend of the Steam Deck / Linux version (`apps/desktop`: Dear ImGui UI,
library, timeline / filmstrip, practice, takes, bookmarks, reset, autosave + resume, settings with
the controller diagram, built-in on-screen keyboard, flash reduction, English / Japanese,
display-locked pacing with just-in-time input) on SDL3 (window, WASAPI audio, gamepads) and a
Direct3D 11 renderer (`apps/windows`), with the **CRT display** (the nesterm physical model on
Direct3D 11 compute, HLSL generated from the Vulkan GLSL), **MP4 export** (Media Foundation: H.264 +
AAC) and **in-app updates** (WinSparkle, the macOS appcast format and EdDSA key). "Add to Steam" is
Linux-only. The engine, frontend core, desktop logic, their tests and `replaynes-cli` build and pass
on Windows; the setup below lets a Mac build, run and test Windows binaries without a Windows
toolchain install.

## The Windows frontend

### Layout (shared with Linux)

| Directory | What |
|---|---|
| `apps/desktop/` | shared SDL3 frontend: `app.cpp` (frame loop, `runApp`), `ui*.cpp`, input routing, pad navigation, OSK, audio DRC, perf stats, scripts (`rnl_app`); logic without SDL (`rnl_logic`: session lifecycle, emulation controller, library, thumbnails, settings, paths, l10n, the CRT setup model `render/crt_model.*` and live-view policy `render/crt_display_policy.*`) + `test_desktop_frontend`; the CRT conformance test shared by Vulkan and Direct3D 11 (`render/crt_conformance.h`); the exporters' shared parts (`export/export_frame.*`, `export_job.cpp`, `mp4_retime.*`, `export_cli.cpp` = `replaynes-export`); seams: `renderer.h` (`Renderer`), `platform/platform.h` (trash, open folder, gamescope), `update_service.h`, `fonts.h`, `export/mp4_export.h`, `render/post_process.h` / `crt_export.h`, `host_clock.h` |
| `apps/linux/` | `main_linux.cpp`, `VkRenderer` (Vulkan FIFO + `VK_KHR_present_wait`), the Vulkan CRT (`src/render`), FFmpeg export (`src/export`), fontconfig fonts, Flatpak portal updates, Add to Steam, Flatpak manifest |
| `apps/windows/` | `main_windows.cpp` (WIN32 subsystem, MMCSS "Games"), `D3D11Renderer`, the CRT (`crt_d3d11.*`, `crt_export_d3d11.cpp`, `shaders/crt/*.hlsl` generated, `crt_test_d3d11.cpp`), MP4 export (`mp4_export_mf.cpp`), updates (`update_winsparkle.*`), Windows fonts, resources (icon from `apps/macos/Resources/IconSource`, UTF-8 + per-monitor-v2 manifest, version info) |

Platform services on Windows (`apps/desktop/src/platform/platform_windows.cpp`): Known Folders -
library `Documents\ReplayNES\{ROM,Projects}`, session `%LOCALAPPDATA%\ReplayNES\Session`, settings
`%APPDATA%\ReplayNES\{settings.ini,bindings.json}`; trash = Recycle Bin through `IFileOperation`
(`FOFX_RECYCLEONDELETE`; the system asks before deleting permanently on a drive without a Recycle
Bin); "Open Folder" = Explorer. Paths are UTF-8 strings: the executables (also the tests) declare
`activeCodePage` UTF-8 in their manifest, so narrow file APIs and `std::filesystem` take them.

### Build

```bash
git submodule update --init third_party/SDL     # SDL 3.4.18 (pinned release), built with the app
scripts/build-windows.sh                        # Mac, llvm-mingw: build/windows-<arch>/apps/windows/ReplayNES.exe
                                                # + dist/ReplayNES-<version>-windows-{x64,arm64}.zip
```

On Windows: `cmake -S . -B build -A x64` (or `-A ARM64`, `-T ClangCL`) `&& cmake --build build
--config Release` -> `build/apps/windows/Release/ReplayNES.exe`. SDL3 is linked statically (no
`SDL3.dll`); llvm-mingw builds are also `-static` and stripped (one 8 MB exe). The zip holds
`ReplayNES.exe`, `WinSparkle.dll` (in-app updates; the official prebuilt DLL of WinSparkle 0.9.4 for
the architecture, downloaded by CMake with a pinned SHA-256 into `build/cache/`; `-DRNW_WINSPARKLE=OFF`
builds without it), `README.txt` (`apps/windows/README.txt`), `LICENSE.txt` and
`THIRD_PARTY_NOTICES.md`. Also built: `replaynes-export.exe` (headless MP4 export + self-test) and
`tests/test_crt_d3d11.exe` (CRT conformance). CI (`.github/workflows/windows.yml`) builds it with
MSVC x64 / arm64 and clang-cl, starts it (`--version`) and uploads `ReplayNES.exe` (MSVC) and the
llvm-mingw zips.

### Run

`ReplayNES.exe [--rom FILE] [--fullscreen] [--lang ja|en] [--vrr] [--log FILE] ...` (`--help`; the
same options as `replaynes-linux`, plus `--vrr` / `--log`). A WIN32-subsystem program: output goes to
the console it was started from, or to `--log FILE`. Language: Japanese when the first Windows
display language is Japanese (`--lang` / `REPLAYNES_LANG` override it). Fonts: Segoe UI, Yu Gothic
(then Meiryo / BIZ UDGothic / MS Gothic) and Segoe UI Symbol from `%WINDIR%\Fonts`.

Controls: the shared bindings (Steam Deck / Xbox layout by button position) and the same menus
(docs/design/UI_REDESIGN.md) - see [STEAM_DECK.md](STEAM_DECK.md#controls) and
`apps/windows/README.txt`; gamepads through SDL3 (XInput, GameInput, raw input / HIDAPI for
PlayStation and Switch Pro). **L+R together (LB+RB / L1+R1) opens the Quick Menu**, R alone pauses
(seek bar), L alone toggles slow motion (both on release); paused, L / R step frames, the south
button (A / Xbox) tapped resumes and the east button (B) drops an A/B marker. Menus confirm with the
east button and go back with the south one (Settings › Controls › Confirm Button swaps them); the
controller diagram and its action picker work with the controller alone. On the keyboard **Esc** (or F1) opens / closes it, and in
menus arrows / Enter / Backspace / Page Up / Page Down / Delete / F2 do what the hint bar shows. With
no controller the "☰ Menu Esc" pill in the top-right corner is a button (mouse / touch). F11 full
screen, F3 statistics. The built-in on-screen keyboard works with a controller; Steam's keyboard is
a Steam Deck feature.

### Rendering and pacing (`apps/windows/src/d3d11_renderer.*`)

* D3D11 device (hardware, FL 11_1 ... 10_0; WARP fallback, `REPLAYNES_D3D11_WARP=1` forces it),
  flip-model swap chain `DXGI_SWAP_EFFECT_FLIP_DISCARD`, 3 buffers, B8G8R8A8, frame latency waitable
  object + `SetMaximumFrameLatency(1)`, `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` when supported. Full
  screen is SDL's borderless full screen (independent flip when nothing covers it); no exclusive mode,
  no DXGI Alt+Enter.
* The picture: a dynamic 256x240 BGRA texture drawn nearest-neighbour into the shared `GameRect`
  (integer / FILL, 8:7, overscan crop) by a quad shader (HLSL compiled at start with `D3DCompile`),
  then `imgui_impl_dx11`. Filmstrip thumbnails: one 1024x1024 atlas (`UpdateSubresource`).
* Pacing source (the Linux `vkWaitForPresentKHR` counterpart): a waiter thread reads
  `IDXGISwapChain::GetFrameStatistics` whenever the waitable object signals (a frame left the queue)
  and every 2 ms while presents are pending. A present is on screen when the statistics'
  `PresentCount` reaches its `GetLastPresentCount()`, at `SyncQPCTime` - the vblank, on the QPC
  clock that `nowSeconds()` uses on Windows. The refresh period comes from `SyncRefreshCount` /
  `SyncQPCTime` differences over 1-3 s (`Renderer::refreshPeriod()`, used by the shared
  `DisplayScheduler` instead of its timestamp estimate). Without statistics the waitable object's
  signal time stands in. The shared frame loop then works as on Linux: cadence (60 Hz every refresh,
  120 Hz every 2nd, 3:2, ...), `rnf_input_deadline` lead, input sampled just in time, DRC audio.
* Timing on the host: `host_clock.h` = QPC; sleeps use a high-resolution waitable timer and a short
  spin (the last 0.4 ms); the frame thread joins MMCSS "Games".
* `--vrr`: in full screen, `Present(0, DXGI_PRESENT_ALLOW_TEARING)` and no present timing - the loop
  paces itself on the host clock at the NES rate (60.0988 Hz) and a G-SYNC / FreeSync display
  follows the presents. Windowed, or without tearing support, it presents with vsync as usual.

### CRT display (Direct3D 11 compute; `apps/windows/src/crt_d3d11.*`)

The same pipeline as Metal (macOS) and Vulkan (Linux) - same passes, constants, parameters,
temporal state and dispatch sizes ([CRT_PORT.md](CRT_PORT.md)) - as cs_5_0 compute shaders on
Direct3D 11 feature level 11_0+ (32 KB group shared memory for the FFT, raw buffers).

* **Shaders: generated, not hand-ported.** `scripts/generate-crt-hlsl.sh` translates the Vulkan GLSL
  (`apps/linux/shaders/crt`, the single source) with glslc -> SPIR-V -> SPIRV-Cross
  `--hlsl --shader-model 50` into `apps/windows/shaders/crt/*.hlsl`, which are committed (each records
  the SHA-256 of its GLSL inputs; CI runs `--check`). CMake embeds them (`cmake/embed_hlsl.cmake`) and
  the app compiles them at run time with `D3DCompile` (`d3dcompiler_47.dll` is part of Windows 10/11),
  `D3DCOMPILE_IEEE_STRICTNESS`; `precise` (NoContraction in SPIR-V) survives the translation. Chosen
  over committed DXBC: no Windows-only compile step in the loop (fxc / dxc aren't on the Mac), one
  shader source for Linux and Windows, readable diffs. Cost: ~1.7-2 s of compilation when the CRT is
  first switched on (in the VM), done on a worker thread - the plain picture shows meanwhile.
* Storage buffers become `(RW)ByteAddressBuffer` at `t<n>` / `u<n>` (binding n); which slot of a
  kernel is an SRV or a UAV comes from shader reflection; push constants become a 64-byte constant
  buffer (b0). The show pass is the generated vertex / pixel shader (Vulkan's clip-space y flipped).
* One GLSL change for FXC: `tube_v_growth` had a `continue` in the per-channel loop, which FXC cannot
  compile inside the dynamic loops ("forced to unroll loop, but unrolling failed"); it is an `if`
  now (same arithmetic; the Vulkan conformance is unchanged).
* Live view: `D3D11Renderer` runs it with the shared `CrtDisplayPolicy` (`apps/desktop/src/render`,
  also used by the Vulkan renderer): RF input from the PPU codes or the flash-filtered RGB picture,
  tube size 1:1 with the GameRect's tube rectangle, build-ahead (`rnf_build_ahead`) and adaptive
  resolution from GPU timestamps (`D3D11_QUERY_TIMESTAMP_DISJOINT`).
* Conformance: `test_crt_d3d11` (ctest `crt_conformance_d3d11`) = the Vulkan test's checks and
  tolerances (`render/crt_conformance.h`, shared) against `tests/fixtures/crt/reference.json`
  (copied next to the exe as `crt_reference.json`); `--warp` forces WARP, `--bench WxH` measures.
  Before the checks it probes whether the device fuses a `precise` multiply-add (it must not): the
  Parallels adapter does (its DXBC -> Metal translation ignores `precise`), so there the
  restructured kernels (shared-memory FFT etc.) can't be bit-identical to the direct port; that
  check then requires agreement within 2e-3 instead. WARP and conforming drivers get the exact check.

Results (2026-10-07):

| Device | receiver | tube / growth | supply | persistence | fast == direct |
|---|---|---|---|---|---|
| Parallels Display Adapter (VM, arm64, D3D11 over Metal) | 1.3e-6 | 1.8e-7 / 1.8e-7 | 3.6e-5 | 2.4e-4 | within 2e-3 (max 8.6e-4; device fuses) |
| WARP (VM arm64) | 8.3e-7 | 1.8e-7 / 2.4e-7 | 3.6e-5 | 4.9e-4 | bit-identical |
| MoltenVK (Mac, Vulkan port, reference) | 7.8e-7 | 1.8e-7 / 2.4e-7 | 3.6e-5 | 2.4e-4 | bit-identical |

GPU time in the VM: the Parallels adapter returns no usable timestamps (so build-ahead / adaptive
resolution never engage there); GPU-bound throughput (`--bench`, default effects, RF input):
**9.1 ms per frame at 1144x858, 14.9 ms at 1600x1200** (WARP: 324 ms at 1144x858). Live view, SMB,
1280x800 window (tube 960x720), `--perf-seconds 20`: 59.0 fps presented with the CRT vs 59.4 without
it in the same session (the VM's own pacing noise dominates; see below). Shader compilation: 1.8 s
(arm64 and emulated x64). MP4 export with "Apply CRT effect" runs the same renderer on its own
device (`crt_export_d3d11.cpp`; hardware, else WARP): 44 fps at 1280x960 in the VM. CI (WARP on the
x64 / arm64 runners): the conformance passes with fast == direct bit-identical (40-49 s).

### MP4 export (Media Foundation; `apps/windows/src/mp4_export_mf.cpp`)

Same exporter as on Linux apart from the encoder: an engine `rn_renderer` (fresh core, logical
time), the shared export settings / presets / geometry / validation, nearest scaling or the CRT
processor, the optional flash filter, the exact BT.709 limited-range 4:2:0 conversion (NV12 here) and
`ExportJob` (shared: `export/export_frame.*`, `export_job.cpp`); the project is never touched, a
failed / cancelled export removes its file.

* **Written under a temporary name** (`<name>.mp4.part` next to the destination) and renamed to
  `<name>.mp4` only after `Finalize`, the timescale fix-up and the self-check below succeeded
  (Linux: the same). Until 0.5.2 the sink wrote the final name directly: an export that never got
  to `Finalize` (process killed / crashed, PC asleep or shut down) left an `.mp4` with media data
  but no `moov` box, which no player opens ("moov atom not found"; reproduced by killing an
  export in the VM). Now such a run leaves at most a `.part` file.
* **Self-check before the rename** (`verifyExportedMp4`; `replaynes-export --check FILE.mp4
  [--frames N]` runs it on any file): `export/mp4_check.*` walks the box structure (top-level
  boxes up to the end of the file, one `moov`, no unterminated `mdat`), every chunk of every track
  inside an `mdat` and not overlapping another, and the first H.264 sample of every chunk plus the
  last one parsing as length-prefixed NAL units of exactly the sample size (a wrong / wrapped
  32-bit chunk offset fails here); then Media Foundation's MPEG-4 source opens the file (duration
  = the frame count), decodes the first frame and the last 3 s up to the final frame, and reads
  the last 3 s of audio. Any failure is the export's error message; the file is removed.

* `IMFSinkWriter` -> MPEG-4: H.264 High, VBR at the shared bit rate, GOP 120, **no B-frames** (no
  reordering delay against the audio); AAC-LC 48 kHz mono from the engine's 16-bit PCM. BT.709
  colour tags.
* **Encoder: hardware first.** Automatic mode tries a hardware H.264 MFT
  (`MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`) with a Direct3D 11 device manager
  (`MF_SINK_WRITER_D3D_MANAGER`; Media Foundation uploads the NV12 frames for MFTs that want
  surfaces), then without one, then Microsoft's software encoder; an attempt that ends up with a
  software MFT is dropped. The same settings go to every encoder. If the hardware encoder fails
  while encoding / finishing, or its file fails the self-check, the export is redone once with the
  software encoder on a second renderer (`ExportOptions::retryRenderer`, created next to the first
  by `ExportJob` / `replaynes-export`): same picture and timing, and the result's encoder reads
  "... - the hardware encoder failed (...)". "mf_hardware" / "mf_software" in Export -> Advanced
  force one (no retry). The result line names the encoder ("(hardware)" when it is one;
  `"hardware": true` in `replaynes-export`'s JSON). The VM has no hardware MFT (Parallels on
  Apple silicon: "H264 Encoder MFT (software)" only), so the hardware path and the retry are
  exercised by the self-test's simulated failure (`simulateHardwareFailureAfter`) there.
* Timestamps from counts: frame n at n x 655171 / 39375000 s, audio sample n at n / 48000 s (in
  Media Foundation's 100 ns units). The MPEG-4 sink stores the video at a rounded timescale (fps x
  1000 = 60098: 999 / 1000 ticks per frame), so after `Finalize` the moov box is rewritten
  (`export/mp4_retime.*`, unit-tested): video timescale **39375000, every frame 655171 ticks**, as the
  FFmpeg exporter writes it. The video `mdhd` is version 1 (64-bit duration: in this timescale any
  take longer than 109 s needs more than 32 bits - 27:53 = 65,895,133,667 ticks); `tkhd` / `mvhd`
  stay in the movie timescale (48000) and switch to version 1 only when needed; chunk offsets
  (`stco` / `co64`) and a 64-bit `mdat` size are left as the sink wrote them (moov is at the end,
  so rewriting it moves no media data). Unit tests: a 27:53 take (100577 frames) and a sparse
  file past 4 GiB (64-bit `mdat`, `co64`), plus files the check must reject (wrapped 32-bit
  offsets, no `moov`, a 32-bit `mdat` size past 4 GiB).
* Verification: `scripts/windows-vm/verify-export.sh [arch]` runs `replaynes-export --self-test` in
  the VM (synthetic test ROM + recorded input: renderer hash == a fresh renderer, flash + processor,
  sub-range, native 8:7, cancel, invalid settings, ExportJob), copies the files back and checks each
  with ffprobe on the Mac: codecs / profile / size / colour tags, frame count (packets and decoded),
  `time_base` 1/39375000, every PTS = f x 655171, `avg_frame_rate` 39375000/655171, duration, audio
  samples. 2026-10-07, arm64 VM, software encoder: all files OK (300 frames = 4.991779 s exactly);
  renderer hashes equal to the Linux / macOS exports of the same takes; an SMB take exported with the
  CRT effect on Windows and on the Mac (Vulkan) differs only by the encoders (PSNR 42 dB).
* Differences to the FFmpeg output: the AAC track ends with Media Foundation's padding (up to one
  1024-sample frame; decoded audio is sample-aligned with the Linux export), the H.264 VUI has no
  chroma-location (players assume left), and Microsoft's encoder needs more bits than x264 for the
  same picture quality.

### In-app updates (WinSparkle; `apps/windows/src/update_winsparkle.*`)

* WinSparkle 0.9.4 (MIT), the official prebuilt `WinSparkle.dll` next to `ReplayNES.exe`, loaded at
  run time (no import library; without the DLL there are no in-app updates). Same appcast format and
  **the same EdDSA key** as Sparkle on macOS: the public key is `SUPublicEDKey` of
  `apps/macos/project.yml`, read by CMake (`RNW_UPDATE_PUBLIC_KEY` overrides it for tests).
* Feed: `https://github.com/kathoc/ReplayNES/releases/latest/download/appcast-windows.xml`
  (`RNW_APPCAST_URL`; the environment variable `REPLAYNES_APPCAST_URL` overrides it at run time - the
  signature is checked against the built-in key regardless). One item per version, one enclosure per
  architecture (`sparkle:os="windows-x64"` / `"windows-arm64"`; an x64 build emulated on Arm gets
  x64), enclosure = the release zip. `scripts/release-windows.sh` builds and signs it
  ([RELEASE.md](RELEASE.md)).
* Settings › System › Updates: "Check Automatically" (WinSparkle's own setting in
  `HKCU\Software\ReplayNES\WinSparkle`; WinSparkle asks on the second launch, like Sparkle) and
  "Check Now" (WinSparkle's dialogs, Japanese / English with the app).
* Installing: WinSparkle downloads the zip and verifies its signature, then calls the app (it
  would only run installers): the zip is unpacked with Windows' `tar.exe` into
  `%LOCALAPPDATA%\ReplayNES\Update\staging-<pid>`, its `ReplayNES.exe` is started as
  `--finish-update <install folder> <pid> -- <the app's arguments>`, WinSparkle asks the app to quit
  (the session is saved as on any quit), the helper waits for it, copies the new files over the
  installation (backup + restore on failure; it needs write access to the folder - fine for an
  unzipped portable app), and starts the new version with the same arguments. Log:
  `%LOCALAPPDATA%\ReplayNES\Update\update.log`; old staging folders are removed at the next start.

### Limitations

* No "Add to Steam" (Linux-only).
* Not code-signed (SmartScreen asks on the first start of a downloaded zip, also after an update).
* Measured only in the VM (below): pacing / latency numbers of real x64 PCs with NVIDIA / AMD /
  Intel drivers are still to be taken; `--vrr` is untested on a real VRR display; the CRT's GPU time
  and the hardware H.264 encoder path need a real GPU.

Three ways to build, from lightest to most "native":

| Build | Where | Compiler | Used for |
|---|---|---|---|
| `scripts/build-windows.sh` | Mac | llvm-mingw (clang + UCRT), cross | daily development; binaries run in the Windows VM |
| `.github/workflows/windows.yml` | GitHub Actions | MSVC (cl), clang-cl, MSVC arm64, llvm-mingw | real x64 / arm64 hardware, MSVC compatibility |
| CMake on Windows itself | any Windows PC | MSVC / clang-cl | `cmake -S . -B build && cmake --build build --config Release && ctest --test-dir build -C Release` |

No Visual Studio is installed in the VM; MSVC is only exercised by CI.

The test executables include `test_desktop_frontend` (the shared desktop logic; also on macOS:
`ctest` in a root build, and on the Deck: `ctest` in the `apps/linux` build).

## Cross-compiling on the Mac (llvm-mingw)

Toolchain: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang/lld, mingw-w64 headers,
UCRT, libc++), release `20260922` (clang 23.1.2), macOS universal build, unpacked to
`~/.local/opt/llvm-mingw` (no PATH change, no Homebrew package). Another location: `LLVM_MINGW=/path`.

```bash
# once (~120 MB download, ~1 GB unpacked)
v=20260922; curl -fsSLO https://github.com/mstorsjo/llvm-mingw/releases/download/$v/llvm-mingw-$v-ucrt-macos-universal.tar.xz
mkdir -p ~/.local/opt && tar -xf llvm-mingw-$v-ucrt-macos-universal.tar.xz -C ~/.local/opt \
  && mv ~/.local/opt/llvm-mingw-$v-ucrt-macos-universal ~/.local/opt/llvm-mingw

scripts/build-windows.sh            # x86_64 + aarch64 -> build/windows-x86_64/, build/windows-aarch64/
scripts/build-windows.sh aarch64    # one architecture
```

CMake toolchain files: `cmake/toolchains/windows-{x86_64,aarch64}-llvm-mingw.cmake` (shared part
`windows-llvm-mingw.cmake`). Executables are linked `-static` (no libc++/libunwind DLLs to copy).
Built: the frontend `apps/windows/ReplayNES.exe` (+ the zips in `dist/`), engine, frontend core, the
21 test executables, `replaynes-cli.exe` and `replaynes-winprobe.exe` (below). Version-bump llvm-mingw in one place: `LLVM_MINGW_VERSION` in
the workflow plus the line above.

## The Windows VM (Parallels)

The developer's existing **Windows 11 on ARM** VM (`Windows 11`, arm64, 4 vCPU, 16 GB, Parallels
Tools installed) is used as is. It is also an everyday / gaming VM, so nothing in it was
reconfigured: no SSH server, no developer tools, no display or sound changes. Everything ReplayNES
puts in the guest is under `%USERPROFILE%\ReplayNES-dev` (binaries) and
`%TEMP%\replaynes-tests-<arch>` (test scratch files); deleting those two folders undoes it.

Snapshots (`prlctl snapshot-list "Windows 11"`):

| Snapshot | State |
|---|---|
| `before-replaynes-dev` | taken before any ReplayNES work (the untouched VM) |
| `replaynes-dev-ready` | after the setup and first test runs (`ReplayNES-dev` folder present) |

Restore with `prlctl snapshot-switch "Windows 11" -i <id>` (ids from `snapshot-list`) — only when
needed: it also rolls back the user's own changes since then.

### Host -> guest access: `prlctl exec`

`prlctl exec "Windows 11" --current-user <cmd>` runs a command **in the logged-in user's desktop
session** (here `p3f3\sonohoka`), with stdout/stderr and the exit code returned to the host.
Without `--current-user` it runs as `NT AUTHORITY\SYSTEM` (works before login, but cannot see the
Mac shares). That is enough for building/test automation, so the built-in OpenSSH Server was
**not** enabled (it would add a service, a firewall rule and keys to a gaming VM). Notes:

- Needs a logged-in user (the VM logs in automatically on boot). `start.sh` waits for it.
- Files come in through Parallels' shared Mac home folder, `\\Mac\Home` (the repo must be under
  `$HOME`); the guest copies them to a local folder with `robocopy` (running from the share works
  but is slower and has different locking semantics).
- `prlctl exec` sometimes fails with `PrlJob_GetResult` / `PrlJob_GetRetCode: Invalid argument`
  (exit 255) without returning the guest's result; `common.sh` retries those, so commands sent
  through it must be idempotent.
- The guest's console code page is 932 (Japanese); the scripts switch to UTF-8 (`chcp 65001`) and
  send PowerShell as `-EncodedCommand`, so quoting and non-ASCII text survive.
- If `prlctl exec` is ever unavailable (no Parallels Tools), the fallback is the built-in OpenSSH
  Server: `Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0`, `Start-Service sshd`,
  host key `~/.ssh/id_ed25519.pub` into `C:\ProgramData\ssh\administrators_authorized_keys`
  (ACL: Administrators + SYSTEM only). Not done here.

### Scripts (`scripts/windows-vm/`, VM name: `WINDOWS_VM`, default `Windows 11`)

| Script | Does |
|---|---|
| `start.sh` | start / resume the VM, wait for the user session |
| `stop.sh` | graceful shutdown (forced after `TIMEOUT`, default 120 s) |
| `deploy.sh [arch\|all]` | copy `build/windows-<arch>/` executables (tests, tools, `ReplayNES.exe`) to `%USERPROFILE%\ReplayNES-dev\<arch>` (stops a running `ReplayNES.exe` first) |
| `run-app.sh [--arch a] [--wait] [--fresh-session] [args]` | start `ReplayNES.exe` in the desktop session with the guest-local library `%USERPROFILE%\ReplayNES-dev\Library` (`--wait`: until it exits, then its log; `APP_ENV="REPLAYNES_LANG=en ..."`) |
| `run-tests.sh [arch\|all] [filter]` | deploy + run every test executable (ctest equivalent); logs in `build/windows-<arch>/vm-test-logs/`; exit status = failed count |
| `probe.sh [arch] [--wait-input N]` | run `replaynes-winprobe` (graphics / audio / input report) |
| `ssh.sh <cmd>` / `--ps <script>` / `--system <cmd>` | run a cmd.exe / PowerShell command in the guest |
| `screenshot.sh [--guest] [out.png]` | `prlctl capture` of the guest screen (`build/vm-shots/`); `--guest`: captured inside the guest (`guest-capture.ps1`, run hidden by `run-hidden.vbs`) - needed while `ReplayNES.exe` is on screen |

```bash
scripts/build-windows.sh && scripts/windows-vm/start.sh && scripts/windows-vm/run-tests.sh
scripts/windows-vm/stop.sh
```

VM quirks met with the frontend:

* **The VM's Documents folder is the Mac's** (`C:\Mac\Home\Documents`, Parallels shared profile):
  `ReplayNES.exe` with its default library would read and write the Mac user's
  `~/Documents/ReplayNES` (the macOS app's library). `run-app.sh` therefore always passes
  `--library-root %USERPROFILE%\ReplayNES-dev\Library`; ROMs for testing are copied there (never
  into the repository).
* **`prlctl capture` returns a black screen while a Direct3D flip-model window is visible** (also
  with the window not covering the screen). `screenshot.sh --guest` captures from inside the guest
  instead; the frontend's own `--script "shot FILE"` (the presented frame, as on the Deck) is used
  for the UI screenshots.
* Every `prlctl exec` of a console program opens a Windows Terminal window in the user's session
  (closed when it ends); `run-hidden.vbs` (wscript) avoids it for the capture.

Tests take their scratch directory from `RN_TEST_TMP` at run time (the build tree's `tests/tmp`
otherwise). `test_local_roms` / the ROM part of `test_flash_filter` skip in the VM: ROMs are never
copied there.

### Test results (VM, 2026-10-07)

| Build | How it runs | Result | Wall time |
|---|---|---|---|
| aarch64 (llvm-mingw) | native arm64 | 20/20 passed | 193 s (sequential) |
| x86_64 (llvm-mingw) | Windows x64 emulation (Prism) on arm64 | 20/20 passed | 565 s (sequential) |
| macOS arm64 (host, reference) | native, `ctest -j6` | 20/20 | ~60 s |

Emulated x64 runs the CPU-heavy tests ~3x slower than native arm64 (`test_flash_filter_exact`
171 s vs 63 s, `test_determinism` 104 s vs 35 s). That says nothing about a real x64 PC: use CI for
x64 hardware.

With the CRT / export / update work (same day, later): aarch64 22/22 (with `test_crt_d3d11`, 210 s),
x86_64 22/22 (590 s; `test_crt_d3d11` 30 s on the Parallels adapter). With the frontend (earlier):
aarch64 21/21 (with `test_desktop_frontend`, 187 s); x86_64 `test_desktop_frontend` passed; `RN_TEST_RECYCLE_BIN=1` (moves a real file with a Japanese name
to the Recycle Bin) passed; macOS root build `ctest` 21/21.

### The frontend in the VM (2026-10-07; **VM numbers, not representative of a PC**)

`scripts/windows-vm/run-app.sh --wait --fresh-session --perf-seconds 20 --warmup 8 --rom <SMB>`
(Super Mario Bros. on the title / attract screen, 1280x800 window unless noted; the VM's display:
1998x1594 at 120 Hz, "Parallels Display Adapter (WDDM)", D3D11 FL 11_1, tearing supported; WASAPI
48 kHz). Ran: library, settings, hub, play, Japanese / English (`--lang`), window and full screen,
`--vrr`, arm64 native and x64 emulated.

| Run | cadence (refresh) | emulated fps | sample -> on screen p50 / p99 (ms) | judder/min | audio underruns |
|---|---|---|---|---|---|
| arm64, window | locked k=2 (120.20 Hz) | 59.86 | 17.4 / 28.9 | 822 | 6 |
| arm64, full screen | free (118.3 Hz) | 59.81 | 17.3 / 29.2 | 897 | 7 |
| arm64, full screen, `--vrr` | host clock | 60.12 | - (no present timing) | - | 0 |
| x64 (emulated), window | locked k=2 (120.20 Hz) | 59.91 | 17.4 / 26.6 | 513 | 0 |

What the VM shows: the presents reach the screen on a virtual vblank whose timestamps (DXGI
`SyncQPCTime`) jitter by +-2 ms, presents miss their vblank often (the input lead sits at its
16.7 ms maximum) and the guest's audio clock runs ~0.5 % slow against QPC (the DRC ratio sits at
its 0.995 limit with `--vrr`). Found and fixed with it: present times taken from the frame
statistics instead of the waitable object's signal (the signal comes when DWM takes the frame, a
refresh before it is shown), the refresh period from vblank counts (the timestamp estimate flapped
between 118 and 121 Hz, i.e. between "locked" and "free"), no catch-up frame when the cadence leaves
"locked" (the shared `DisplayScheduler`; emulation ran at 63 fps), and a steady host-clock schedule
for `--vrr` (58.5 -> 60.1 fps). Frame pacing, latency and VRR still need a real PC.

Screenshots (in-app `--script "... shot FILE"`, the presented frames; `build/vm-shots/app/`):
`library-{en,ja}`, `playing-{en,ja}`, `hub-{en,ja}`, `settings-display-{en,ja}`,
`settings-audio-{en,ja}`, `settings-audio-playing-{en,ja}`; the desktop with the window:
`build/vm-shots/desktop-playing.png` (`screenshot.sh --guest`).
CRT / export / updates: `play-plain`, `play-crt`, `play-crt-off` (arm64), `x64-plain`, `x64-crt`,
`x64-crt-off`, `settings-display-crt-{en,ja}`, `final-system-{en,ja}` (Settings -> System with the
update controls), `final-export-{en,ja}`; WinSparkle's error for a wrongly signed update:
`build/vm-shots/update-bad-signature.png` (`prlctl capture` worked here - the app was behind
WinSparkle's window). These show the UI before the redesign.

**The redesigned UI (2026-10-08, arm64, 1280x800 window, keyboard only - the VM has no controller):**
`build/vm-shots/ui/w-{en,ja}-NN-*.png` - library, playing, the paused seek bar, Quick Menu, Retry,
Practice, Settings (Display, Controls, Keyboard, Controller, System), Game, and the Menu pill clicked
(closes / opens the menu) - from a `--script` driving the app with SDL key and mouse events
(`key Escape`, `key Return`, `clickpill`, `page <id>`). `layoutcheck 1920x1080 1280x800` walks every
screen of the menu model, the library and the seek bar at the window size and as a 1920x1080 layout:
nothing overflows or scrolls (60 / 60 screens).

### CI (`.github/workflows/windows.yml`)

Since the frontend the jobs also build `replaynes-win` (MSVC x64 / arm64, clang-cl x64, llvm-mingw
x86_64 + aarch64 with the release zips), start it once (`--version`) and upload `ReplayNES.exe` /
the zips. Since the CRT / export / updates: ctest includes `crt_conformance_d3d11` (WARP), the
macOS job checks the generated HLSL against the GLSL (`generate-crt-hlsl.sh --check`), the x64 runner
also runs `replaynes-export --self-test` (Media Foundation is present on the Server 2025 runner;
informational step) and the artifacts include `WinSparkle.dll`. All jobs green (22/22 tests); the
table below is the first run before the frontend.

| Job | Runner | Result (first run) |
|---|---|---|
| MSVC x64 | `windows-latest` (Windows Server 2025, x64) | build + ctest 20/20 |
| clang-cl x64 | `windows-latest`, `-T ClangCL` | build + ctest 20/20 (after the narrowing fix below) |
| MSVC arm64 | `windows-11-arm` | build + ctest 20/20 |
| llvm-mingw cross-build | `macos-latest` | x86_64 + aarch64 build |
| llvm-mingw x86_64 tests | `windows-latest`, executables from the cross-build | 20/20 |

Runner probe: no GPU (DXGI adapter = Microsoft Basic Render Driver / WARP: D3D11 FL 11_1, D3D12
FL 12_1 with shader model 6.8), Vulkan loader only, OpenGL 1.1 (GDI), no audio endpoint.

## Graphics / audio / input in the VM (`replaynes-winprobe`)

`tools/windows-probe/probe.cpp` (built on Windows only, `REPLAYNES_BUILD_WINDOWS_PROBE`) loads every
API at run time and prints what it finds; it also runs in CI on the GitHub runners. Results in the
VM (Windows 11 build 26200, arm64; Parallels Desktop 27.0.2, host Apple M1 Max):

| API | Result |
|---|---|
| DXGI | adapter "Parallels Display Adapter (WDDM)" (+ Microsoft Basic Render Driver); `ALLOW_TEARING` supported; output 120 Hz |
| Direct3D 11 | hardware device, **feature level 11_1** (WARP also 11_1) |
| Direct3D 12 | device created, but **max feature level 11_1, shader model 5.1 only (no DXIL / SM 6), resource binding tier 1** |
| Vulkan | `vulkan-1.dll` loader 1.3.301; devices are **"Microsoft Direct3D12 (...)": Dozen, Vulkan 1.2 layered on D3D12** — no native Vulkan driver |
| OpenGL | 4.3, "Parallels using Metal (Apple M1 Max)" |
| WASAPI | 1 render endpoint, mix format 48 kHz / 2 ch / float32; period 10 ms default, 2.67 ms min; `IAudioClient3` shared-mode engine period 128-480 frames (low-latency mode available) |
| XInput | `xinput1_4.dll` present; no pad connected during the probe |
| GameInput | `GameInput.dll` present (`GameInputCreate` exported) |
| Raw input (HID) | 2 keyboards, 2 mice, no game controller connected |

x64 (emulated) probe: identical graphics results. Controllers: Parallels passes USB controllers
through to the guest (the user's games work with them); none was attached during this run. To
check one: attach it to the VM (Devices > USB), then `scripts/windows-vm/probe.sh aarch64
--wait-input 15` and press a button (XInput slot + raw-input VID/PID are printed).

## Rendering backend: why Direct3D 11

**SDL3 for window / input / audio + a small Direct3D 11 renderer (Dear ImGui `imgui_impl_dx11`),
not Vulkan** - the recommendation made before the port, now implemented.

- D3D11 FL 11_0+ exists on every Windows 10/11 PC (x64 and arm64), in VMs, over Remote Desktop
  and on WARP; in this VM it is the only API with a real hardware path at full capability. ReplayNES
  needs little GPU: textured quads, the CRT shader (portable to HLSL SM 5.0), ImGui.
- Vulkan cannot be assumed: in this VM (and on Windows on Arm generally) it is Dozen over D3D12
  FL 11_1 / SM 5.1, and some x64 PCs (older Intel iGPUs, VMs, remote sessions) ship no Vulkan driver
  at all. Keeping the Linux Vulkan renderer as an optional Windows backend is possible later, not as
  the default.
- SDL_GPU on Windows means D3D12 (or Vulkan). Here D3D12 is FL 11_1 / SM 5.1 / binding tier 1,
  so SDL_GPU with DXBC shaders might work but is unverified; it would need a smoke test before
  being chosen.
- D3D11 also fits the rest of the plan in [PORTING.md](PORTING.md): Spout stream output shares a
  D3D11 texture, and Media Foundation H.264 export can take D3D11 surfaces.
- Pacing: flip-model swap chain (`FLIP_DISCARD`), waitable object for frame pacing, `ALLOW_TEARING`
  for VRR (supported here); audio through SDL3 (WASAPI); gamepads through SDL3's gamepad API
  (XInput / raw input / GameInput backends), as on Linux.

## Portability notes (engine / frontend core)

- File I/O already uses wide APIs on Windows (`_wfopen`, `std::filesystem::u8path`); flush is
  `_commit` (= `FlushFileBuffers`); atomic replace is `std::filesystem::rename` (`MoveFileExW` with
  `MOVEFILE_REPLACE_EXISTING` in both libc++ and the MSVC STL); the session lock is `LockFileEx`.
- Fixed while setting this up: the frontend core's ROM library used ANSI `stat` (UTF-8 paths broke
  under non-UTF-8 code pages) and a clock-difference conversion for file times (not reproducible
  between scans, which defeated the ROM hash cache); it now uses `GetFileAttributesExW`. Paths
  joined by the frontend core use `\` on Windows (as the engine's `rn::fs::join`); relative ROM
  paths shown to users keep `/`. `FILE_ATTRIBUTE_HIDDEN` files are skipped like macOS hidden files.
- Nestopia's `StringCompare` narrows `int` to 16-bit `wchar_t` in a braced initializer: an error in
  clang/gcc on Windows, silenced for that target only (`-Wno-c++11-narrowing`); MSVC only warns.
- MSVC needs `/utf-8` for the UTF-8 string literals in the tests (set in `tests/CMakeLists.txt`).

## Limits

- The VM is Windows on **ARM** on Apple silicon: GPU = Parallels' virtual adapter over Metal, x64
  binaries run emulated. It is good for "does it run / does it render / does the UI work" checks,
  not for performance, frame pacing or GPU-driver compatibility of typical x64 gaming PCs (NVIDIA /
  AMD / Intel drivers). Those need CI (correctness only) or a real PC.
- GitHub's runners have no GPU (D3D11 falls back to WARP) and no audio device.
- `prlctl exec` needs Parallels Tools and a logged-in user.
