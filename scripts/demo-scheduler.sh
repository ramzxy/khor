#!/usr/bin/env bash
# Drives csw signal → percussive clicks and rhythm.
# Spawns many threads that yield constantly, causing context switches.
set -euo pipefail
echo "==> Context switch storm — Ctrl+C to stop"
echo "    Best with: percussive preset"

cleanup() { kill 0 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

# Spawn competing busy-yield loops across cores.
NCPU=$(nproc 2>/dev/null || echo 4)
for _ in $(seq 1 "$NCPU"); do
  (
    while true; do
      # sched_yield forces a context switch each iteration.
      python3 -c "
import os, time
for _ in range(5000):
    os.sched_yield()
" 2>/dev/null || {
        # Fallback if python3 isn't available: rapid fork/yield via subshells.
        for _ in $(seq 1 500); do /bin/true; done
      }
    done
  ) &
done

echo "    Running $NCPU yield loops — watch the clicks build up"
wait
