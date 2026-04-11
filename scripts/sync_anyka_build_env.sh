#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_SRC="$REPO_ROOT/toolchain/arm-anykav200-crosstool"
SRC="${ANYKA_TOOLCHAIN_SRC:-$DEFAULT_SRC}"

if [ -d "$SRC/usr/bin" ] && [ -f "$SRC/usr/bin/arm-anykav200-linux-uclibcgnueabi-gcc.br_real" ]; then
  TOOLCHAIN_DIR="$SRC"
elif [ -d "$SRC/arm-anykav200-crosstool/usr/bin" ] && [ -f "$SRC/arm-anykav200-crosstool/usr/bin/arm-anykav200-linux-uclibcgnueabi-gcc.br_real" ]; then
  TOOLCHAIN_DIR="$SRC/arm-anykav200-crosstool"
else
  echo "Could not find extracted Anyka toolchain." >&2
  echo "Set ANYKA_TOOLCHAIN_SRC to the directory that contains usr/bin/arm-anykav200-linux-uclibcgnueabi-gcc.br_real" >&2
  exit 2
fi

mkdir -p "$HOME/lsc-build-env"
rsync -a --delete "$TOOLCHAIN_DIR/" "$HOME/lsc-build-env/anyka-toolchain/"
rsync -a --delete "$REPO_ROOT/out/i386_root/" "$HOME/lsc-build-env/i386_root/"

du -sh "$HOME/lsc-build-env/anyka-toolchain" "$HOME/lsc-build-env/i386_root"
