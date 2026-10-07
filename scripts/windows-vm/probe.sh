#!/bin/bash
# Run replaynes-winprobe (tools/windows-probe) in the VM: D3D11/D3D12/DXGI, Vulkan, OpenGL,
# WASAPI, XInput/GameInput/raw-input controllers. Results for the dev VM: docs/WINDOWS.md.
#   scripts/windows-vm/probe.sh [aarch64|x86_64] [--wait-input SECONDS]
set -euo pipefail
. "$(dirname "$0")/common.sh"
need_running
ARCH="${1:-aarch64}"; shift || true
"$(dirname "$0")/deploy.sh" "$ARCH" >/dev/null
win_cmd "\"$GUEST_DIR\\$ARCH\\replaynes-winprobe.exe\" $*" | tr -d '\r'
