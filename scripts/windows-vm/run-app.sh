#!/bin/bash
# Runs the Windows frontend (ReplayNES.exe, deployed by deploy.sh) in the VM's desktop session.
# The VM's Documents folder may be redirected to the Mac (Parallels shared profile), so the app
# always gets a guest-local library: %USERPROFILE%\ReplayNES-dev\Library\{ROM,Projects} (ROMs for
# testing are put there by hand or with ROM=; never into the repository).
#   scripts/windows-vm/run-app.sh [--arch aarch64|x86_64] [--wait] [--fresh-session] [app arguments...]
#     --wait: wait until the app exits (perf runs: --perf-seconds N) and print its log
#     --fresh-session: an empty session folder %USERPROFILE%\ReplayNES-dev\perf-session (perf runs:
#       no "save the previous session?" prompt, no resume)
#   ROM=/host/path.nes  copy that ROM into the guest library first
#   APP_ENV="NAME=value ..."  environment for the app (REPLAYNES_LANG=en, REPLAYNES_DEBUG_PRESENT=1)
# Log: %USERPROFILE%\ReplayNES-dev\app-<arch>.log (--log); stats: pass --stats-log yourself.
# Stop it: scripts/windows-vm/ssh.sh taskkill /im ReplayNES.exe
set -euo pipefail
. "$(dirname "$0")/common.sh"
need_running
ARCH=aarch64
WAIT=0
FRESH=0
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2 ;;
    --wait) WAIT=1; shift ;;
    --fresh-session) FRESH=1; shift ;;
    *) break ;;
  esac
done
D='%USERPROFILE%\ReplayNES-dev'
win_cmd "(if not exist \"$D\\Library\\ROM\" mkdir \"$D\\Library\\ROM\")" >/dev/null
if [ -n "${ROM:-}" ]; then
  win_cmd "copy /y \"$(host_unc "$ROM")\" \"$D\\Library\\ROM\\\" >nul" >/dev/null
fi
if [ "$FRESH" = 1 ]; then
  win_ps "Remove-Item -Recurse -Force -ErrorAction SilentlyContinue \"\$env:USERPROFILE\\ReplayNES-dev\\perf-session\"" >/dev/null || true
  set -- --session-root "$D\\perf-session" "$@"
fi
# PowerShell argument list: each argument a double-quoted PowerShell string ($env:USERPROFILE
# expands; ` escapes) that keeps double quotes around it on the program's command line.
ps_args=""
for a in --library-root "$D\\Library" --log "$D\\app-$ARCH.log" "$@"; do
  a="${a//\`/\`\`}"
  a="${a//\$/\`\$}"
  a="${a//\"/\`\"}"
  a="${a//%USERPROFILE%/\$env:USERPROFILE}"
  ps_args+="\"\`\"$a\`\"\","
done
ps_args="${ps_args%,}"
envset=""
for kv in ${APP_ENV:-}; do envset+="\$env:${kv%%=*}='${kv#*=}'; "; done
WAITFLAG=""
[ "$WAIT" = 1 ] && WAITFLAG="-Wait"
win_ps "$envset\$p = Start-Process -FilePath \"\$env:USERPROFILE\\ReplayNES-dev\\$ARCH\\ReplayNES.exe\" -ArgumentList @($ps_args) -PassThru $WAITFLAG; if (\$p.HasExited) { 'exit ' + \$p.ExitCode } else { 'pid ' + \$p.Id }" \
  | grep -v -e '^#< CLIXML' -e '^<Objs' || true
if [ "$WAIT" = 1 ]; then
  win_cmd "type \"$D\\app-$ARCH.log\""
fi
