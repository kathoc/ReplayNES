#!/bin/bash
# Run a command in the Windows VM from the host (no SSH server needed: `prlctl exec`, Parallels
# Tools). Output and exit code come back to the host.
#   scripts/windows-vm/ssh.sh dir %USERPROFILE%           cmd.exe, as the logged-in user
#   scripts/windows-vm/ssh.sh --ps 'Get-ComputerInfo'      PowerShell, as the logged-in user
#   scripts/windows-vm/ssh.sh --system whoami              cmd.exe as NT AUTHORITY\SYSTEM
set -euo pipefail
. "$(dirname "$0")/common.sh"
need_running
case "${1:-}" in
  ""|-h|--help) sed -n '2,6p' "$0"; exit 2 ;;
  --ps) shift; win_ps "$*" ;;
  --system) shift; win_cmd_system "$*" ;;
  *) win_cmd "$*" ;;
esac
