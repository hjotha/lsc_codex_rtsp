#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$REPO_ROOT/out"

gcc -O2 -Wall -Wextra -std=c11 \
  -o "$REPO_ROOT/out/anyka_ring_rtsp_server_host" \
  "$REPO_ROOT/src/anyka_ring_rtsp_server.c"

file "$REPO_ROOT/out/anyka_ring_rtsp_server_host"
