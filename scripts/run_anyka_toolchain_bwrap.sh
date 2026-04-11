#!/usr/bin/env bash
set -euo pipefail

ROOT="${HOME}/lsc-build-env"
I386ROOT="$ROOT/i386_root/lib/i386-linux-gnu"
TOOLBASE="$ROOT/anyka-toolchain"

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <tool> [args...]" >&2
  exit 2
fi

tool="$1"
shift

exec bwrap \
  --bind / / \
  --tmpfs /lib \
  --ro-bind "$I386ROOT/ld-linux.so.2" /lib/ld-linux.so.2 \
  --ro-bind "$I386ROOT/libc.so.6" /lib/libc.so.6 \
  --ro-bind "$I386ROOT/libm.so.6" /lib/libm.so.6 \
  --ro-bind "$I386ROOT/libdl.so.2" /lib/libdl.so.2 \
  --ro-bind "$I386ROOT/libgcc_s.so.1" /lib/libgcc_s.so.1 \
  --ro-bind "$I386ROOT/libstdc++.so.6.0.30" /lib/libstdc++.so.6 \
  --ro-bind "$I386ROOT/libz.so.1.2.11" /lib/libz.so.1 \
  --setenv LD_LIBRARY_PATH "$TOOLBASE/usr/lib:/lib" \
  "$TOOLBASE/usr/bin/$tool" "$@"
