#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$REPO_ROOT/out/i386_root"
RUNTIME="$REPO_ROOT/out/i386_runtime"

mkdir -p "$ROOT/lib/i386-linux-gnu" "$ROOT/usr/lib/i386-linux-gnu"

cp -Laf "$RUNTIME/libc6_2.35-0ubuntu3.13_i386/lib/i386-linux-gnu/." "$ROOT/lib/i386-linux-gnu/"
cp -Lf "$RUNTIME/libc6_2.35-0ubuntu3.13_i386/lib/ld-linux.so.2" "$ROOT/lib/i386-linux-gnu/"
cp -Laf "$RUNTIME/libgcc-s1_12.3.0-1ubuntu1~22.04.3_i386/lib/i386-linux-gnu/." "$ROOT/lib/i386-linux-gnu/"
cp -Laf "$RUNTIME/zlib1g_1.2.11.dfsg-2ubuntu9.2_i386/lib/i386-linux-gnu/." "$ROOT/lib/i386-linux-gnu/"
cp -Laf "$RUNTIME/libstdc++6_12.3.0-1ubuntu1~22.04.3_i386/usr/lib/i386-linux-gnu/." "$ROOT/usr/lib/i386-linux-gnu/"
cp -Laf "$RUNTIME/libstdc++6_12.3.0-1ubuntu1~22.04.3_i386/usr/lib/i386-linux-gnu/." "$ROOT/lib/i386-linux-gnu/"

find "$ROOT" -type f | sort
