#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DAEMON="$ROOT_DIR/daemon/build/khor-daemon"

if [[ ! -x "$DAEMON" ]]; then
  echo "missing $DAEMON. Run: ./scripts/linux-build.sh" >&2
  exit 1
fi

# Running the daemon under sudo/root often breaks audio because PipeWire/Pulse is a per-user service.
# If invoked via sudo, grant the binary the needed BPF capabilities and then drop back to the user.
if [[ "${EUID:-$(id -u)}" -eq 0 && -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
  echo "khor: running under sudo; will drop privileges to '$SUDO_USER' for audio" >&2
  if command -v setcap >/dev/null 2>&1; then
    if setcap cap_bpf,cap_perfmon,cap_sys_resource+ep "$DAEMON"; then
      echo "khor: setcap ok ($(getcap "$DAEMON" 2>/dev/null || true))" >&2
    else
      echo "khor: warning: setcap failed; eBPF may require sudo/root" >&2
    fi
  else
    echo "khor: warning: 'setcap' not found; install libcap and re-run" >&2
  fi

  RUN_UID="$(id -u "$SUDO_USER")"
  RUN_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
  if [[ -z "$RUN_HOME" ]]; then RUN_HOME="/home/$SUDO_USER"; fi

  exec sudo -u "$SUDO_USER" -H env \
    XDG_RUNTIME_DIR="/run/user/$RUN_UID" \
    HOME="$RUN_HOME" \
    "$DAEMON" "$@"
fi

exec "$DAEMON" "$@"

