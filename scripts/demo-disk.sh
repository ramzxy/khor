#!/usr/bin/env bash
# Drives io signal → filter cutoff sweep (80Hz to 9kHz).
# Writes and reads temp files to generate block I/O.
set -euo pipefail
echo "==> Disk I/O storm — Ctrl+C to stop"
echo "    Best with: drone or ambient preset"

TMPDIR=$(mktemp -d /tmp/khor-demo-disk.XXXXX)
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

while true; do
  # Write 16MB in random chunks.
  dd if=/dev/urandom of="$TMPDIR/blob" bs=64k count=256 conv=fdatasync 2>/dev/null
  # Read it back.
  dd if="$TMPDIR/blob" of=/dev/null bs=64k 2>/dev/null
  # Small file churn.
  for i in $(seq 1 50); do
    echo "x" > "$TMPDIR/f$i"
  done
  sync
  rm -f "$TMPDIR"/f*
  sleep 0.2
done
