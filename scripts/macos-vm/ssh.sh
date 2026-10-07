#!/bin/bash
# ssh into the running test VM (key auth set up by create.sh). Extra arguments run as a command.
#   scripts/macos-vm/ssh.sh              scripts/macos-vm/ssh.sh 'sw_vers'
set -euo pipefail
. "$(dirname "$0")/common.sh"
IP="$(vm_ip)"
if [ $# -eq 0 ]; then exec ssh "${SSH_OPTS[@]}" "$VM_USER@$IP"; fi
exec ssh "${SSH_OPTS[@]}" -o BatchMode=yes "$VM_USER@$IP" "$@"
