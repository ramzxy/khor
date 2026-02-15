#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f "$ROOT_DIR/daemon/third_party/miniaudio.h" ]]; then
  echo "missing daemon/third_party/miniaudio.h. Run: ./scripts/fetch_deps.sh" >&2
  exit 1
fi
if [[ ! -f "$ROOT_DIR/daemon/third_party/httplib.h" ]]; then
  echo "missing daemon/third_party/httplib.h. Run: ./scripts/fetch_deps.sh" >&2
  exit 1
fi

mkdir -p "$ROOT_DIR/daemon/build"
cmake -S "$ROOT_DIR/daemon" -B "$ROOT_DIR/daemon/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT_DIR/daemon/build" -j

