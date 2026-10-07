# Windows development

There is no Windows frontend yet. What exists: the portable engine (`engine/`), the shared
frontend core (`frontend/`), their tests and `replaynes-cli` build and pass on Windows, and the
setup below lets a Mac build, run and test Windows binaries without a Windows toolchain install.
The Windows frontend will follow (see [Rendering backend](#recommendation-for-the-windows-frontend)).

Three ways to build, from lightest to most "native":

| Build | Where | Compiler | Used for |
|---|---|---|---|
| `scripts/build-windows.sh` | Mac | llvm-mingw (clang + UCRT), cross | daily development; binaries run in the Windows VM |
| `.github/workflows/windows.yml` | GitHub Actions | MSVC (cl), clang-cl, MSVC arm64, llvm-mingw | real x64 / arm64 hardware, MSVC compatibility |
| CMake on Windows itself | any Windows PC | MSVC / clang-cl | `cmake -S . -B build && cmake --build build --config Release && ctest --test-dir build -C Release` |

No Visual Studio is installed in the VM; MSVC is only exercised by CI.

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
Built: engine, frontend core, the 20 test executables, `replaynes-cli.exe` and
`replaynes-winprobe.exe` (below). Version-bump llvm-mingw in one place: `LLVM_MINGW_VERSION` in
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
| `deploy.sh [arch\|all]` | copy `build/windows-<arch>/` executables to `%USERPROFILE%\ReplayNES-dev\<arch>` |
| `run-tests.sh [arch\|all] [filter]` | deploy + run every test executable (ctest equivalent); logs in `build/windows-<arch>/vm-test-logs/`; exit status = failed count |
| `probe.sh [arch] [--wait-input N]` | run `replaynes-winprobe` (graphics / audio / input report) |
| `ssh.sh <cmd>` / `--ps <script>` / `--system <cmd>` | run a cmd.exe / PowerShell command in the guest |
| `screenshot.sh [out.png]` | `prlctl capture` of the guest screen (`build/vm-shots/`) |

```bash
scripts/build-windows.sh && scripts/windows-vm/start.sh && scripts/windows-vm/run-tests.sh
scripts/windows-vm/stop.sh
```

Tests take their scratch directory from `RN_TEST_TMP` at run time (the build tree's `tests/tmp`
otherwise). `test_local_roms` / the ROM part of `test_flash_filter` skip in the VM: ROMs are never
copied there.

### Test results (VM, 2026-10-07)

| Build | How it runs | Result | Wall time |
|---|---|---|---|
| aarch64 (llvm-mingw) | native arm64 | 20/20 passed | 193 s (sequential) |
| x86_64 (llvm-mingw) | Windows x64 emulation (Prism) on arm64 | 20/20 passed | RESULT_X |
| macOS arm64 (host, reference) | native, `ctest -j6` | 20/20 | ~60 s |

Emulated x64 runs the CPU-heavy tests 3-4x slower than native arm64 (`test_flash_filter_exact`
205 s vs 64 s). That says nothing about a real x64 PC: use CI (below) for x64 hardware.

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

## Recommendation for the Windows frontend

**SDL3 for window / input / audio + a small Direct3D 11 renderer (Dear ImGui `imgui_impl_dx11`),
not Vulkan.**

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
