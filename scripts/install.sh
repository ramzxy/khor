#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BIN_DST="${HOME}/.local/bin/khor-daemon"
UI_DST="${HOME}/.local/share/khor/ui"
UNIT_DST="${HOME}/.config/systemd/user/khor.service"

echo "==> Building daemon"
"${ROOT_DIR}/scripts/linux-build.sh"

echo "==> Building UI"
if command -v npm >/dev/null 2>&1; then
  # Use package-lock when present.
  if [[ -f "${ROOT_DIR}/ui/package-lock.json" ]]; then
    npm --prefix "${ROOT_DIR}/ui" ci
  else
    npm --prefix "${ROOT_DIR}/ui" install
  fi
  npm --prefix "${ROOT_DIR}/ui" run build
else
  echo "npm not found; skipping UI build" >&2
fi

echo "==> Installing binary to ${BIN_DST}"
install -d "${HOME}/.local/bin"
install -m 0755 "${ROOT_DIR}/daemon/build/khor-daemon" "${BIN_DST}"

if [[ -d "${ROOT_DIR}/ui/dist" ]]; then
  echo "==> Installing UI to ${UI_DST}"
  rm -rf "${UI_DST}"
  install -d "${UI_DST}"
  cp -a "${ROOT_DIR}/ui/dist/." "${UI_DST}/"
else
  echo "ui/dist not found; UI will not be served until you build it (npm --prefix ui run build)" >&2
fi

echo "==> Installing systemd user unit to ${UNIT_DST}"
install -d "${HOME}/.config/systemd/user"
install -m 0644 "${ROOT_DIR}/systemd/khor.service" "${UNIT_DST}"

echo
echo "==> Next steps"
echo "1) One-time capabilities (for eBPF without sudo/root runtime):"
echo "   sudo setcap cap_bpf,cap_perfmon,cap_sys_resource+ep \"${BIN_DST}\""
echo "   getcap \"${BIN_DST}\""
echo
echo "2) Start at login:"
echo "   systemctl --user daemon-reload"
echo "   systemctl --user enable --now khor.service"
echo
echo "3) UI:"
echo "   open http://127.0.0.1:17321"

