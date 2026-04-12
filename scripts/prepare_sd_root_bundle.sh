#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE_ROOT="$REPO_ROOT/packages/sd_root_v3.2863.105/root"

rm -rf "$BUNDLE_ROOT"
mkdir -p "$BUNDLE_ROOT"

bash "$REPO_ROOT/scripts/build_rtsp_kick_anyka.sh" >/dev/null

cp -a "$REPO_ROOT/sdcard/." "$BUNDLE_ROOT/"
cp "$REPO_ROOT/out/rtsp_kick_arm" "$BUNDLE_ROOT/rtsp_kick"

chmod 755 \
  "$BUNDLE_ROOT/hostapd" \
  "$BUNDLE_ROOT/rtsp_kick" \
  "$BUNDLE_ROOT/hack.sh" \
  "$BUNDLE_ROOT/custom.sh" \
  "$BUNDLE_ROOT/vendor_rtsp_boot.sh"

printf 'Prepared SD root bundle in %s\n' "$BUNDLE_ROOT"
