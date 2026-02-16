#!/usr/bin/env bash
set -euo pipefail

BIN_DST="${HOME}/.local/bin/khor-daemon"
UI_DST="${HOME}/.local/share/khor"
UNIT_DST="${HOME}/.config/systemd/user/khor.service"

echo "==> Stopping service (if running)"
if command -v systemctl >/dev/null 2>&1; then
  systemctl --user disable --now khor.service 2>/dev/null || true
  systemctl --user daemon-reload 2>/dev/null || true
fi

echo "==> Removing files"
rm -f "${BIN_DST}" || true
rm -rf "${UI_DST}" || true
rm -f "${UNIT_DST}" || true

echo "uninstalled"

