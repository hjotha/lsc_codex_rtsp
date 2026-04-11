#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE="$REPO_ROOT/out/i386_runtime"

mkdir -p "$BASE"
cd "$BASE"

wget -q -N http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.35-0ubuntu3.13_i386.deb
wget -q -N http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-12/libstdc++6_12.3.0-1ubuntu1~22.04.3_i386.deb
wget -q -N http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-12/libgcc-s1_12.3.0-1ubuntu1~22.04.3_i386.deb
wget -q -N http://archive.ubuntu.com/ubuntu/pool/main/z/zlib/zlib1g_1.2.11.dfsg-2ubuntu9.2_i386.deb

for p in *.deb; do
  d="${p%.deb}"
  mkdir -p "$d"
  dpkg-deb -x "$p" "$d"
done

find "$BASE" \
  \( -name 'ld-linux.so.2' -o -name 'libc.so.6' -o -name 'libstdc++.so.6' -o -name 'libgcc_s.so.1' -o -name 'libm.so.6' -o -name 'libz.so.1' \) \
  | sort
