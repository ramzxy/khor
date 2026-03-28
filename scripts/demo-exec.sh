#!/usr/bin/env bash
# Drives exec signal → note probability, accent chords, filter resonance.
# Spawns lots of short-lived processes in bursts.
set -euo pipefail
echo "==> Exec storm (process spawning) — Ctrl+C to stop"
echo "    Best with: ambient or percussive preset"

while true; do
  # Burst: spawn 30 processes rapidly.
  for _ in $(seq 1 30); do
    /bin/true &
  done
  wait
  # Pause to create rhythmic bursts rather than constant noise.
  sleep 0.$(( RANDOM % 4 + 1 ))
done
