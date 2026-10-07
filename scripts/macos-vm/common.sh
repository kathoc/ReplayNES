# Shared settings for scripts/macos-vm/*.sh (sourced, not run). See docs/MACOS_VM.md.
# Everything can be overridden from the environment.

VM="${VM:-replaynes-test}"                        # the working VM (disposable)
CLEAN_VM="${CLEAN_VM:-replaynes-test-clean}"      # the pristine, provisioned VM it is cloned from
IMAGE="${IMAGE:-ghcr.io/cirruslabs/macos-golden-gate-vanilla:27.0}"   # macOS 27 vanilla
VM_CPU="${VM_CPU:-4}"
VM_MEM_MB="${VM_MEM_MB:-8192}"
VM_DISPLAY="${VM_DISPLAY:-1920x1200}"
VM_DISK_GB="${VM_DISK_GB:-80}"
VM_USER="${VM_USER:-admin}"
VM_PASS="${VM_PASS:-admin}"                       # cirruslabs image default; the VM is NAT/local-only

# tart: PATH first, then the release tarball install location used by docs/MACOS_VM.md.
if command -v tart >/dev/null 2>&1; then TART="$(command -v tart)"
elif [ -x "$HOME/.local/opt/tart/tart.app/Contents/MacOS/tart" ]; then TART="$HOME/.local/opt/tart/tart.app/Contents/MacOS/tart"
else echo "tart not found (see docs/MACOS_VM.md, Setup)" >&2; exit 1; fi

STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/replaynes-vm"
mkdir -p "$STATE_DIR"
KNOWN_HOSTS="$STATE_DIR/known_hosts"              # VM host keys, kept out of ~/.ssh/known_hosts

# Host key of the user (first one found) for passwordless SSH into the guest.
SSH_KEY=""
for k in "$HOME/.ssh/id_ed25519" "$HOME/.ssh/id_ecdsa" "$HOME/.ssh/id_rsa"; do
  [ -f "$k.pub" ] && { SSH_KEY="$k"; break; }
done

SSH_OPTS=(-o UserKnownHostsFile="$KNOWN_HOSTS" -o StrictHostKeyChecking=accept-new
          -o ConnectTimeout=5 -o ServerAliveInterval=10 -o LogLevel=ERROR)
[ -n "$SSH_KEY" ] && SSH_OPTS+=(-i "$SSH_KEY")

vm_running() { "$TART" list --format json 2>/dev/null | python3 -c '
import json,sys; n=sys.argv[1]
sys.exit(0 if any(v.get("Name")==n and v.get("State")=="running" for v in json.load(sys.stdin)) else 1)' "$1"; }

vm_exists() { "$TART" get "$1" >/dev/null 2>&1; }

vm_ip() { "$TART" ip --wait 120 "$VM"; }

# ssh into the guest with the user's key (installed by create.sh).
vm_ssh() {
  local ip; ip="$(vm_ip)"
  ssh "${SSH_OPTS[@]}" -o BatchMode=yes "$VM_USER@$ip" "$@"
}

# ssh with the default password, via SSH_ASKPASS (no sshpass needed). Used once, to install the key.
vm_ssh_pw() {
  local ip askpass; ip="$(vm_ip)"
  askpass="$STATE_DIR/askpass.sh"
  printf '#!/bin/sh\necho %q\n' "$VM_PASS" > "$askpass"; chmod 700 "$askpass"
  SSH_ASKPASS="$askpass" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
    ssh "${SSH_OPTS[@]}" -o PreferredAuthentications=password,keyboard-interactive -o PubkeyAuthentication=no \
    "$VM_USER@$ip" "$@"
}

vm_scp() {   # vm_scp <local...> <remote-path>  (last argument is the guest path)
  local ip; ip="$(vm_ip)"
  local dst="${*: -1}"; local srcs=("${@:1:$#-1}")
  scp -q "${SSH_OPTS[@]}" -o BatchMode=yes "${srcs[@]}" "$VM_USER@$ip:$dst"
}

wait_ssh() {
  local i
  for i in $(seq 1 90); do
    if [ "${1:-}" = pw ]; then vm_ssh_pw true >/dev/null 2>&1 && return 0
    else vm_ssh true >/dev/null 2>&1 && return 0; fi
    sleep 2
  done
  echo "SSH into $VM did not come up" >&2; return 1
}

run_log() { echo "$STATE_DIR/$VM.run.log"; }      # tart run output of $VM (holds the VNC URL)
vnc_url() { grep -Eo 'vnc://[^ ]+' "$(run_log)" 2>/dev/null | tail -1; }
