#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$REPO_ROOT/out"

bash "$REPO_ROOT/scripts/setup_i386_runtime.sh" >/dev/null
bash "$REPO_ROOT/scripts/setup_i386_root.sh" >/dev/null
bash "$REPO_ROOT/scripts/sync_anyka_build_env.sh" >/dev/null

bash "$REPO_ROOT/scripts/run_anyka_toolchain_bwrap.sh" \
  arm-anykav200-linux-uclibcgnueabi-gcc.br_real \
  -Os -s -Wall -Wextra -std=c11 \
  -o "$REPO_ROOT/out/anyka_ring_rtsp_server_arm" \
  "$REPO_ROOT/src/anyka_ring_rtsp_server.c"

bash "$REPO_ROOT/scripts/run_anyka_toolchain_bwrap.sh" \
  arm-anykav200-linux-uclibcgnueabi-readelf \
  -h "$REPO_ROOT/out/anyka_ring_rtsp_server_arm"
