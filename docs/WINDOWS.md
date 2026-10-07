# Windows

Status 2026-10-07: **Windows frontend preview** (`ReplayNES.exe`, CMake target `replaynes-win`):
the shared desktop frontend of the Steam Deck / Linux version (`apps/desktop`: Dear ImGui UI,
library, timeline / filmstrip, practice, takes, bookmarks, reset, autosave + resume, settings with
the controller diagram, built-in on-screen keyboard, flash reduction, English / Japanese,
display-locked pacing with just-in-time input) on SDL3 (window, WASAPI audio, gamepads) and a
Direct3D 11 renderer (`apps/windows`). Not yet ported (step 2): the CRT display (HLSL port of the
Vulkan compute model) and MP4 export (Media Foundation); in-app updates and "Add to Steam" are
Linux-only. The engine, frontend core, desktop logic, their tests and `replaynes-cli` build and pass
on Windows; the setup below lets a Mac build, run and test Windows binaries without a Windows
toolchain install.

## The Windows frontend

### Layout (shared with Linux)

| Directory | What |
|---|---|
| `apps/desktop/` | shared SDL3 frontend: `app.cpp` (frame loop, `runApp`), `ui*.cpp`, input routing, pad navigation, OSK, audio DRC, perf stats, scripts (`rnl_app`); logic without SDL (`rnl_logic`: session lifecycle, emulation controller, library, thumbnails, settings, paths, l10n) + `test_desktop_frontend`; seams: `renderer.h` (`Renderer`), `platform/platform.h` (trash, open folder, gamescope), `update_service.h`, `fonts.h`, `export/mp4_export.h`, `render/post_process.h` / `crt_export.h`, `host_clock.h` |
| `apps/linux/` | `main_linux.cpp`, `VkRenderer` (Vulkan FIFO + `VK_KHR_present_wait`), the Vulkan CRT (`src/render`), FFmpeg export (`src/export`), fontconfig fonts, Flatpak portal updates, Add to Steam, Flatpak manifest |
| `apps/windows/` | `main_windows.cpp` (WIN32 subsystem, MMCSS "Games"), `D3D11Renderer`, Windows fonts, export / CRT stubs, resources (icon from `apps/macos/Resources/IconSource`, UTF-8 + per-monitor-v2 manifest, version info) |

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
`ReplayNES.exe`, `README.txt` (`apps/windows/README.txt`), `LICENSE.txt` and
`THIRD_PARTY_NOTICES.md`. CI (`.github/workflows/windows.yml`) builds it with MSVC x64 / arm64 and
clang-cl, starts it (`--version`) and uploads `ReplayNES.exe` (MSVC) and the llvm-mingw zips.

### Run

`ReplayNES.exe [--rom FILE] [--fullscreen] [--lang ja|en] [--vrr] [--log FILE] ...` (`--help`; the
same options as `replaynes-linux`, plus `--vrr` / `--log`). A WIN32-subsystem program: output goes to
the console it was started from, or to `--log FILE`. Language: Japanese when the first Windows
display language is Japanese (`--lang` / `REPLAYNES_LANG` override it). Fonts: Segoe UI, Yu Gothic
(then Meiryo / BIZ UDGothic / MS Gothic) and Segoe UI Symbol from `%WINDIR%\Fonts`.

Controls: the shared bindings (Steam Deck / Xbox layout by button position) - see the table in
[STEAM_DECK.md](STEAM_DECK.md#controls) and `apps/windows/README.txt`; gamepads through SDL3
(XInput, GameInput, raw input / HIDAPI for PlayStation and Switch Pro). Keyboard as on Linux (F11
full screen, F3 statistics). The built-in on-screen keyboard works with a controller; Steam's
keyboard is a Steam Deck feature.

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

### Not ported yet / limitations

* CRT display and MP4 export: Settings -> Display says so; Export… is not on the hub, the Project
  menu entry is disabled with the note. The seams stay (`render/post_process.h`,
  `export/mp4_export.h`, `render/crt_export.h`; stubs in `apps/windows/src/export_unavailable.cpp`).
* No in-app updates (Settings shows the version only), no "Add to Steam".
* Not code-signed (SmartScreen asks on the first start of a downloaded zip).
* Measured only in the VM (below): pacing / latency numbers of real x64 PCs with NVIDIA / AMD /
  Intel drivers are still to be taken; `--vrr` is untested on a real VRR display.

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

With the frontend (same day, later): aarch64 21/21 (with `test_desktop_frontend`, 187 s); x86_64
`test_desktop_frontend` passed; `RN_TEST_RECYCLE_BIN=1` (moves a real file with a Japanese name
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

### CI (`.github/workflows/windows.yml`)

Since the frontend the jobs also build `replaynes-win` (MSVC x64 / arm64, clang-cl x64, llvm-mingw
x86_64 + aarch64 with the release zips), start it once (`--version`) and upload `ReplayNES.exe` /
the zips. Not run yet for this change (it was not pushed); the results below are the first run
before the frontend.

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
