#!/usr/bin/env bash
# Drives mem signal → darkens filter, increases reverb, adds resonance strain.
# Uses multiple approaches to create real PSI memory pressure.
set -euo pipefail
echo "==> Memory pressure waves — Ctrl+C to stop"
echo "    Best with: drone or ambient preset"

cleanup() { kill 0 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

# Grab ~80% of available memory to actually trigger pressure.
AVAIL_MB=$(awk '/MemAvailable/ {printf "%d", $2/1024}' /proc/meminfo)
TARGET_MB=$(( AVAIL_MB * 80 / 100 ))
if [ "$TARGET_MB" -lt 512 ]; then TARGET_MB=512; fi
WORKERS=$(nproc 2>/dev/null || echo 4)
PER_WORKER_MB=$(( TARGET_MB / WORKERS ))

echo "    Available: ${AVAIL_MB}MB — will pressure with ${TARGET_MB}MB across ${WORKERS} workers"

if command -v stress-ng &>/dev/null; then
  echo "    Using stress-ng"
  while true; do
    # Ramp up: allocate memory across workers, touch every page.
    stress-ng --vm "$WORKERS" --vm-bytes "${PER_WORKER_MB}m" --vm-hang 3 --timeout 8s --quiet 2>/dev/null || true
    sleep 1
  done
else
  echo "    Using python3 allocator (install stress-ng for stronger effect)"
  while true; do
    # Spawn multiple python processes to eat memory in parallel.
    for _ in $(seq 1 "$WORKERS"); do
      python3 -c "
import time, os
size = $PER_WORKER_MB * 1024 * 1024
# mmap anonymous memory and touch every page to force allocation.
buf = bytearray(size)
# Touch pages to make kernel actually commit them.
for i in range(0, len(buf), 4096):
    buf[i] = 0xFF
time.sleep(4)
" &
    done
    wait
    sleep 1
  done
fi
