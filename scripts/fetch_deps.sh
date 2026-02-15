#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP_DIR="$ROOT_DIR/daemon/third_party"
mkdir -p "$TP_DIR"

fetch() {
  local url="$1"
  local out="$2"
  echo "fetch: $url"
  curl -fsSL "$url" -o "$out.tmp"
  mv "$out.tmp" "$out"
}

# Pinned to specific commits/tags to avoid silent upstream changes.
fetch "https://raw.githubusercontent.com/mackron/miniaudio/0.11.22/miniaudio.h" "$TP_DIR/miniaudio.h"
fetch "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h" "$TP_DIR/httplib.h"

echo "deps fetched into: $TP_DIR"

