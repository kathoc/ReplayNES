# Shared settings for scripts/macos-vm/*.sh (sourced, not run). See docs/MACOS_VM.md.
# Parallels Desktop backend. The VM is generic ("MacOS"); nothing here is ReplayNES-specific
# except the deploy/run-app helpers. Everything can be overridden from the environment.

VM="${REPLAYNES_VM:-${VM:-MacOS}}"
SNAPSHOT="${SNAPSHOT:-provisioned}"               # snapshot reset.sh switches to ("clean" = untouched install)
VM_USER="${VM_USER:-admin}"
VM_PASS="${VM_PASS:-admin}"                       # local test account; the VM is on Parallels' shared NAT

PRLCTL="${PRLCTL:-$(command -v prlctl || echo /usr/local/bin/prlctl)}"
PRLSRVCTL="${PRLSRVCTL:-$(command -v prlsrvctl || echo /usr/local/bin/prlsrvctl)}"
[ -x "$PRLCTL" ] || { echo "prlctl not found (Parallels Desktop required, see docs/MACOS_VM.md)" >&2; exit 1; }

STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/replaynes-vm"
mkdir -p "$STATE_DIR"
KNOWN_HOSTS="$STATE_DIR/known_hosts"              # VM host keys, kept out of ~/.ssh/known_hosts

SSH_KEY=""
for k in "$HOME/.ssh/id_ed25519" "$HOME/.ssh/id_ecdsa" "$HOME/.ssh/id_rsa"; do
  [ -f "$k.pub" ] && { SSH_KEY="$k"; break; }
done
SSH_OPTS=(-o UserKnownHostsFile="$KNOWN_HOSTS" -o StrictHostKeyChecking=accept-new
          -o ConnectTimeout=5 -o ServerAliveInterval=10 -o LogLevel=ERROR)
[ -n "$SSH_KEY" ] && SSH_OPTS+=(-i "$SSH_KEY")

vm_exists() { "$PRLCTL" list -a -o name 2>/dev/null | awk -v n="$VM" 'NR>1 && $0==n {f=1} END{exit !f}'; }
vm_status() { "$PRLCTL" list -a -o status,name 2>/dev/null | awk -v n="$VM" 'NR>1 { s=$1; $1=""; sub(/^ +/,""); if ($0==n) print s }'; }
vm_running() { [ "$(vm_status)" = running ]; }

# Guest IPv4 (Parallels shared network); empty until the guest tools report it.
vm_ip_now() { "$PRLCTL" list -a -o name,ip 2>/dev/null | awk -v n="$VM" 'NR>1 { ip=$NF; $NF=""; sub(/ +$/,""); if ($0==n && ip ~ /^[0-9.]+$/) print ip }'; }
vm_ip() {
  local ip i
  for i in $(seq 1 60); do ip="$(vm_ip_now)"; [ -n "$ip" ] && { echo "$ip"; return 0; }; sleep 2; done
  echo "no IP for $VM" >&2; return 1
}

vm_ssh() { local ip; ip="$(vm_ip)"; ssh "${SSH_OPTS[@]}" -o BatchMode=yes "$VM_USER@$ip" "$@"; }
vm_scp() {   # vm_scp <local...> <remote-path>  (last argument is the guest path)
  local ip; ip="$(vm_ip)"
  local dst="${*: -1}"; local srcs=("${@:1:$#-1}")
  scp -q "${SSH_OPTS[@]}" -o BatchMode=yes "${srcs[@]}" "$VM_USER@$ip:$dst"
}
# Run a command as root in the guest (Parallels Tools; works before SSH/login).
vm_root() { "$PRLCTL" exec "$VM" "$@"; }

wait_ssh() {
  local i
  for i in $(seq 1 90); do vm_ssh true >/dev/null 2>&1 && return 0; sleep 2; done
  echo "SSH into $VM did not come up" >&2; return 1
}
