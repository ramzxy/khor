#!/usr/bin/env bash
# Drives ALL signals simultaneously for maximum musical chaos.
# Layers network, exec, disk, scheduler, and memory pressure.
set -euo pipefail
echo "==> Everything at once — Ctrl+C to stop"
echo "    Best with: percussive preset for rhythmic chaos, or ambient for a dense wash"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cleanup() { kill 0 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

"$DIR/demo-network.sh" &
"$DIR/demo-exec.sh" &
"$DIR/demo-disk.sh" &
"$DIR/demo-scheduler.sh" &

echo "    All four generators running — listen to the layers build"
wait
