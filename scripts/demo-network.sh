#!/usr/bin/env bash
# Drives rx/tx signals → reverb, delay, arp gating.
# Creates a wash of reverb and echo that swells with traffic.
set -euo pipefail
echo "==> Network storm (rx + tx) — Ctrl+C to stop"
echo "    Best with: ambient or arp preset"

cleanup() { kill 0 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

while true; do
  # Parallel downloads from several fast public endpoints.
  curl -s -o /dev/null http://speedtest.tele2.net/1MB.zip &
  curl -s -o /dev/null http://proof.ovh.net/files/1Mb.dat &
  # UDP burst to localhost (tx without needing a remote server).
  dd if=/dev/urandom bs=8k count=128 2>/dev/null | nc -u -w0 127.0.0.1 19999 || true
  wait
  sleep 0.3
done
