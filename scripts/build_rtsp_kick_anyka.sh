#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$REPO_ROOT/out"

OUT_BIN="$REPO_ROOT/out/rtsp_kick_arm"

DEFAULT_ANYKA_SRC="$REPO_ROOT/toolchain/arm-anykav200-crosstool"
ANYKA_SRC="${ANYKA_TOOLCHAIN_SRC:-$DEFAULT_ANYKA_SRC}"

if [ -d "$ANYKA_SRC/usr/bin" ] && [ -f "$ANYKA_SRC/usr/bin/arm-anykav200-linux-uclibcgnueabi-gcc.br_real" ]; then
  HAVE_ANYKA=1
elif [ -d "$ANYKA_SRC/arm-anykav200-crosstool/usr/bin" ] && [ -f "$ANYKA_SRC/arm-anykav200-crosstool/usr/bin/arm-anykav200-linux-uclibcgnueabi-gcc.br_real" ]; then
  HAVE_ANYKA=1
else
  HAVE_ANYKA=0
fi

if [ "$HAVE_ANYKA" = "1" ]; then
  export ANYKA_TOOLCHAIN_SRC="$ANYKA_SRC"
  bash "$REPO_ROOT/scripts/setup_i386_runtime.sh" >/dev/null
  bash "$REPO_ROOT/scripts/setup_i386_root.sh" >/dev/null
  bash "$REPO_ROOT/scripts/sync_anyka_build_env.sh" >/dev/null

  bash "$REPO_ROOT/scripts/run_anyka_toolchain_bwrap.sh" \
    arm-anykav200-linux-uclibcgnueabi-gcc.br_real \
    -Os -s -Wall -Wextra -std=c11 \
    -o "$OUT_BIN" \
    "$REPO_ROOT/src/rtsp_kick.c"

  bash "$REPO_ROOT/scripts/run_anyka_toolchain_bwrap.sh" \
    arm-anykav200-linux-uclibcgnueabi-readelf \
    -h "$OUT_BIN"
  exit 0
fi

if command -v arm-linux-gnueabi-gcc >/dev/null 2>&1; then
  arm-linux-gnueabi-gcc \
    -Os -static -s -Wall -Wextra -std=c11 \
    -o "$OUT_BIN" \
    "$REPO_ROOT/src/rtsp_kick.c"

  file "$OUT_BIN"
  arm-linux-gnueabi-readelf -h "$OUT_BIN"
  exit 0
fi

echo "No usable toolchain found for rtsp_kick." >&2
echo "Expected Anyka toolchain under $ANYKA_SRC or arm-linux-gnueabi-gcc in PATH." >&2
exit 2
