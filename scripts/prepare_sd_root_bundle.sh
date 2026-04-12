#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE_ROOT="$REPO_ROOT/packages/sd_root_v3.2863.105/root"

rm -rf "$BUNDLE_ROOT"
mkdir -p "$BUNDLE_ROOT"

bash "$REPO_ROOT/scripts/build_rtsp_kick_anyka.sh" >/dev/null

cp "$REPO_ROOT/out/rtsp_kick_arm" "$BUNDLE_ROOT/rtsp_kick"
cp "$REPO_ROOT/sdcard/hack" "$BUNDLE_ROOT/hack"
cp "$REPO_ROOT/sdcard/hack.sh" "$BUNDLE_ROOT/hack.sh"
cp "$REPO_ROOT/sdcard/custom.sh" "$BUNDLE_ROOT/custom.sh"
cp "$REPO_ROOT/sdcard/vendor_rtsp_boot.sh" "$BUNDLE_ROOT/vendor_rtsp_boot.sh"
cp "$REPO_ROOT/sdcard/vendor_rtsp_boot.md5" "$BUNDLE_ROOT/vendor_rtsp_boot.md5"

chmod 755 \
  "$BUNDLE_ROOT/rtsp_kick" \
  "$BUNDLE_ROOT/hack" \
  "$BUNDLE_ROOT/hack.sh" \
  "$BUNDLE_ROOT/custom.sh" \
  "$BUNDLE_ROOT/vendor_rtsp_boot.sh"

printf 'Prepared SD root bundle in %s\n' "$BUNDLE_ROOT"
